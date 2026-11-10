// Flows Embed: the compiler and DSP engine as a single-threaded WebAssembly
// module with no JavaScript runtime. The same .wasm runs on the page (compile,
// diagnostics, parameter list) and inside an AudioWorkletProcessor (compile,
// render), so embedding never needs SharedArrayBuffer, pthreads or
// cross-origin isolation. JavaScript reaches it through the C API below
// (embed/src/wasm.ts).
//
// Everything after flows_embed_prepare() is allocation-free: notes and
// parameter changes go into fixed queues and render() only calls Engine.
#include "compiler/program.hpp"
#include "engine/engine.hpp"
#include "engine/sample.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace flows;

// Matches the app's realtime block limit; hosts render larger requests in chunks.
constexpr std::uint32_t maxBlock = 1024;
constexpr std::size_t queueCapacity = 256, heldCapacity = 1024;

void appendEscaped(std::string& out, std::string_view text) {
    out += '"';
    for (const unsigned char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof buffer, "\\u%04x", c);
                out += buffer;
            } else
                out += static_cast<char>(c);
        }
    }
    out += '"';
}
void appendNumber(std::string& out, double value) {
    if (!std::isfinite(value)) {
        out += "null";
        return;
    }
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.17g", value);
    out += buffer;
}

struct Instrument {
    std::optional<Program> program;
    std::vector<Diagnostic> diagnostics;
    std::string info, error;
    SampleBank samples;
    std::unique_ptr<Engine> engine;
    std::array<float, maxBlock> left{}, right{};
    // Pending input, sorted by sample. Fixed capacity: never allocates.
    std::array<Event, queueCapacity> events{};
    std::size_t eventCount = 0;
    std::array<ParameterUpdate, queueCapacity> updates{};
    std::size_t updateCount = 0;
    struct Held {
        std::uint64_t id = 0;
        double pitch = 0;
    };
    std::array<Held, heldCapacity> held{};
    std::size_t heldCount = 0;
    std::uint32_t channels = 1;
    std::uint64_t dropped = 0;

    std::uint64_t clock() const { return engine ? engine->stats().processed_samples : 0; }
    Held* find(std::uint64_t id) {
        for (std::size_t i = 0; i < heldCount; ++i)
            if (held[i].id == id) return &held[i];
        return nullptr;
    }
};

std::string describe(const Instrument& instrument) {
    std::string out = "{\"ok\":";
    out += instrument.program ? "true" : "false";
    out += ",\"diagnostics\":[";
    for (std::size_t i = 0; i < instrument.diagnostics.size(); ++i) {
        const auto& d = instrument.diagnostics[i];
        if (i) out += ',';
        out += "{\"line\":" + std::to_string(d.span.line) + ",\"column\":" + std::to_string(d.span.column)
            + ",\"offset\":" + std::to_string(d.span.offset) + ",\"length\":" + std::to_string(d.span.length) + ",\"message\":";
        appendEscaped(out, d.message);
        out += '}';
    }
    out += ']';
    if (instrument.program) {
        const auto& program = *instrument.program;
        out += ",\"channels\":" + std::to_string(program.nodes[program.output].channels);
        // Two sources with the same structure differ only in live values: the page
        // can update those in place instead of replacing the running engine.
        out += ",\"structure\":";
        appendEscaped(out, std::to_string(std::hash<std::string>{}(dsp_structure_key(program))));
        out += ",\"voice\":";
        out += program.voice_policy.mode == VoiceMode::mono ? "\"mono\"" : "\"poly\"";
        out += ",\"parameters\":[";
        for (std::size_t i = 0; i < program.parameters.size(); ++i) {
            const auto& p = program.parameters[i];
            if (i) out += ',';
            out += "{\"index\":" + std::to_string(i) + ",\"id\":";
            appendEscaped(out, p.binding_id);
            out += ",\"name\":";
            appendEscaped(out, p.name);
            out += ",\"unit\":";
            appendEscaped(out, unit_name(p.unit));
            out += ",\"value\":";
            appendNumber(out, p.value < program.nodes.size() ? program.nodes[p.value].initial_value : 0.0);
            out += ",\"min\":";
            appendNumber(out, p.min);
            out += ",\"max\":";
            appendNumber(out, p.max);
            out += ",\"step\":";
            appendNumber(out, p.step);
            out += ",\"default\":";
            appendNumber(out, p.default_value);
            out += ",\"hasDefault\":";
            out += p.has_default ? "true" : "false";
            out += ",\"hasRange\":";
            out += p.has_range ? "true" : "false";
            out += ",\"live\":";
            out += p.live ? "true" : "false";
            out += ",\"discrete\":";
            out += p.discrete ? "true" : "false";
            out += ",\"logarithmic\":";
            out += p.logarithmic ? "true" : "false";
            out += ",\"scale\":";
            appendEscaped(out, p.control_scale);
            out += ",\"scaleOffset\":";
            appendNumber(out, p.control_offset);
            // The literal that sets this value, as UTF-8 byte offsets into the source.
            out += ",\"offset\":" + std::to_string(p.span.offset) + ",\"length\":" + std::to_string(p.span.length);
            out += '}';
        }
        out += "],\"samples\":[";
        for (std::size_t i = 0; i < program.samples.size(); ++i) {
            if (i) out += ',';
            appendEscaped(out, program.samples[i].path);
        }
        out += ']';
    }
    out += '}';
    return out;
}
}  // namespace

extern "C" {
__attribute__((export_name("flows_embed_alloc"))) void* flows_embed_alloc(std::size_t size) { return std::malloc(size ? size : 1); }
__attribute__((export_name("flows_embed_release"))) void flows_embed_release(void* pointer) { std::free(pointer); }

// Compiles UTF-8 source. Always returns an instrument (null only when memory
// is exhausted); flows_embed_info() reports success and diagnostics.
__attribute__((export_name("flows_embed_compile"))) Instrument* flows_embed_compile(const char* source, std::size_t length) {
    try {
        auto instrument = std::make_unique<Instrument>();
        auto result = compile(std::string_view(source, length), "embed.flows");
        instrument->diagnostics = std::move(result.diagnostics);
        instrument->program = std::move(result.program);
        if (instrument->program) {
            instrument->channels = instrument->program->nodes[instrument->program->output].channels;
            instrument->samples.resize(instrument->program->samples.size());
        }
        instrument->info = describe(*instrument);
        return instrument.release();
    } catch (...) {
        return nullptr;
    }
}
// JSON: {ok, diagnostics, channels, voice, parameters, samples}. Owned by the instrument.
__attribute__((export_name("flows_embed_info"))) const char* flows_embed_info(Instrument* instrument) { return instrument->info.c_str(); }
// The last preparation or sample error, or an empty string.
__attribute__((export_name("flows_embed_error"))) const char* flows_embed_error(Instrument* instrument) { return instrument->error.c_str(); }

// Decodes a WAV or AIFF file for program.samples[index].
__attribute__((export_name("flows_embed_sample"))) int flows_embed_sample(Instrument* instrument, int index, const unsigned char* bytes, std::size_t length) {
    if (!instrument->program || index < 0 || static_cast<std::size_t>(index) >= instrument->samples.size()) return 0;
    try {
        instrument->samples[static_cast<std::size_t>(index)] = load_sample_bytes(std::span(bytes, length));
        return 1;
    } catch (const std::exception& error) {
        instrument->error = instrument->program->samples[static_cast<std::size_t>(index)].path + ": " + error.what();
        return 0;
    }
}

// Builds the engine. boundarySeconds 0 matches offline renders exactly; the
// app uses 3 ms of click-free note boundaries for live playing.
__attribute__((export_name("flows_embed_prepare"))) int flows_embed_prepare(Instrument* instrument, double sampleRate, double boundarySeconds, double bpm) {
    if (!instrument->program) return 0;
    try {
        for (std::size_t i = 0; i < instrument->samples.size(); ++i)
            if (!instrument->samples[i]) throw std::runtime_error("Sample not loaded: " + instrument->program->samples[i].path);
        instrument->engine.reset();
        instrument->engine = std::make_unique<Engine>(*instrument->program, EngineConfig{sampleRate, maxBlock, 32, boundarySeconds},
                                                      instrument->samples);
        instrument->engine->set_capture_values({});
        instrument->engine->set_transport({std::clamp(bpm, 20.0, 300.0), 0, true, true});
        instrument->eventCount = instrument->updateCount = instrument->heldCount = 0;
        instrument->error.clear();
        return 1;
    } catch (const std::exception& error) {
        instrument->engine.reset();
        instrument->error = error.what();
        return 0;
    }
}

// Engine time in samples: the first sample of the next render.
__attribute__((export_name("flows_embed_clock"))) double flows_embed_clock(Instrument* instrument) { return static_cast<double>(instrument->clock()); }

namespace {
bool enqueue(Instrument& instrument, Event event, double at) {
    if (!instrument.engine || instrument.eventCount == queueCapacity) {
        ++instrument.dropped;
        return false;
    }
    const auto clock = instrument.clock();
    event.sample = at > static_cast<double>(clock) ? static_cast<std::uint64_t>(at) : clock;
    // Stable insertion keeps same-sample events in arrival order.
    auto position = instrument.eventCount;
    while (position > 0 && instrument.events[position - 1].sample > event.sample) {
        instrument.events[position] = instrument.events[position - 1];
        --position;
    }
    instrument.events[position] = event;
    ++instrument.eventCount;
    return true;
}
}  // namespace

// at: engine sample (see flows_embed_clock); earlier times play at the next render.
// Returns 0 when the identity is already held or the queue is full.
__attribute__((export_name("flows_embed_note_on"))) int flows_embed_note_on(Instrument* instrument, double id, double pitch, double velocity, double at) {
    const auto identity = static_cast<std::uint64_t>(id);
    if (id < 1 || !std::isfinite(pitch) || !std::isfinite(velocity) || instrument->find(identity) || instrument->heldCount == heldCapacity)
        return 0;
    Event event;
    event.type = EventType::note_on;
    event.note_id = identity;
    event.pitch = std::clamp(pitch, 0.0, 127.0);
    event.velocity = std::clamp(velocity, 0.0, 1.0);
    if (!enqueue(*instrument, event, at)) return 0;
    instrument->held[instrument->heldCount++] = {identity, event.pitch};
    return 1;
}
// Releases a held identity; unknown identities are ignored.
__attribute__((export_name("flows_embed_note_off"))) int flows_embed_note_off(Instrument* instrument, double id, double at) {
    auto* found = instrument->find(static_cast<std::uint64_t>(id));
    if (!found) return 0;
    Event event;
    event.type = EventType::note_off;
    event.note_id = found->id;
    event.pitch = found->pitch;
    event.velocity = 0;
    if (!enqueue(*instrument, event, at)) return 0;
    *found = instrument->held[--instrument->heldCount];
    return 1;
}
// Sets parameter `index` (flows_embed_info().parameters) with its authored smoothing.
__attribute__((export_name("flows_embed_param"))) int flows_embed_param(Instrument* instrument, int index, double value) {
    if (!instrument->engine || index < 0 || static_cast<std::size_t>(index) >= instrument->program->parameters.size()) return 0;
    const auto& descriptor = instrument->program->parameters[static_cast<std::size_t>(index)];
    if (!descriptor.live || !std::isfinite(value) || instrument->updateCount == queueCapacity) return 0;
    if (descriptor.has_range) value = std::clamp(value, descriptor.min, descriptor.max);
    instrument->updates[instrument->updateCount++] = {instrument->clock(), descriptor.value, value};
    return 1;
}
// Releases every voice now (keeps parameters and the sample clock).
__attribute__((export_name("flows_embed_panic"))) void flows_embed_panic(Instrument* instrument) {
    instrument->eventCount = instrument->heldCount = 0;
    if (instrument->engine) instrument->engine->panic();
}
__attribute__((export_name("flows_embed_set_bpm"))) void flows_embed_set_bpm(Instrument* instrument, double bpm) {
    if (instrument->engine && std::isfinite(bpm)) instrument->engine->set_transport({std::clamp(bpm, 20.0, 300.0), 0, true, false});
}

// Renders up to 1024 frames into flows_embed_left/right. Mono instruments
// write the same signal to both. Returns the number of active voices, or -1
// when the engine rejected the block (it then outputs silence and resets).
__attribute__((export_name("flows_embed_render"))) int flows_embed_render(Instrument* instrument, int frames) {
    if (!instrument->engine || frames <= 0 || frames > static_cast<int>(maxBlock)) return -1;
    const auto count = static_cast<std::size_t>(frames);
    const auto start = instrument->clock(), end = start + count;
    std::size_t due = 0;
    while (due < instrument->eventCount && instrument->events[due].sample < end) ++due;
    const auto events = std::span<const Event>(instrument->events.data(), due);
    const auto updates = std::span<const ParameterUpdate>(instrument->updates.data(), instrument->updateCount);
    auto left = std::span(instrument->left).first(count), right = std::span(instrument->right).first(count);
    const auto result = instrument->channels == 2 ? instrument->engine->process_stereo(events, left, right, nullptr, updates)
                                                  : instrument->engine->process(events, left, nullptr, updates);
    std::copy(instrument->events.begin() + static_cast<std::ptrdiff_t>(due), instrument->events.begin() + static_cast<std::ptrdiff_t>(instrument->eventCount),
              instrument->events.begin());
    instrument->eventCount -= due;
    instrument->updateCount = 0;
    if (instrument->channels != 2) std::copy(left.begin(), left.end(), right.begin());
    if (!result) {
        instrument->heldCount = 0;
        return -1;
    }
    return static_cast<int>(instrument->engine->stats().active_voices);
}
__attribute__((export_name("flows_embed_left"))) float* flows_embed_left(Instrument* instrument) { return instrument->left.data(); }
__attribute__((export_name("flows_embed_right"))) float* flows_embed_right(Instrument* instrument) { return instrument->right.data(); }
__attribute__((export_name("flows_embed_free"))) void flows_embed_free(Instrument* instrument) { delete instrument; }
}
