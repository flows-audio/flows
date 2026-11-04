#include "Analyzer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <limits>
#include <map>
#include <numbers>
#include <set>
#include <utility>

namespace flows::app {
namespace {
constexpr std::size_t windowSize = 4096;
constexpr std::size_t waveformPoints = 192;
constexpr std::size_t spectrumPoints = 512;
constexpr std::size_t melBands = 64;

std::vector<bool> audioValues(const Program& program) {
    std::vector<bool> result(program.nodes.size(), false);
    for (std::size_t i = 0; i < program.nodes.size(); ++i) {
        const auto& node = program.nodes[i];
        if (node.op == Op::phasor || node.op == Op::sampler || node.op == Op::sine || node.op == Op::saw || node.op == Op::unison_sine || node.op == Op::unison_saw) result[i] = true;
        else if (node.op != Op::adsr && node.op != Op::key_trigger && node.op != Op::lfo)
            for (auto input : node.inputs) if (input < i && result[input]) result[i] = true;
    }
    return result;
}
std::string shapeName(AnalysisShape shape) {
    switch (shape) {
        case AnalysisShape::waveform: return "waveform";
        case AnalysisShape::scalar: return "scalar";
        case AnalysisShape::spectrum: return "spectrum";
        case AnalysisShape::mel_spectrum: return "mel_spectrum";
    }
    return "unknown";
}
struct Selection {
    std::vector<NamedAnalysis> views;
    std::vector<bool> needed;
    std::vector<ValueId> roots;
    std::string error;
};
Selection select(const Program& program, std::span<const std::string> names, bool defaultAnalysis) {
    Selection selected;
    selected.needed.resize(program.analysis.size(), false);
    if (names.empty() && defaultAnalysis) {
        selected.views.assign(program.named_analysis.begin(), program.named_analysis.begin() + std::min<std::size_t>(4, program.named_analysis.size()));
    } else {
        std::set<std::string> unique;
        for (const auto& name : names) {
            if (!unique.insert(name).second) continue;
            auto found = std::find_if(program.named_analysis.begin(), program.named_analysis.end(), [&](const auto& view) { return view.name == name; });
            if (found == program.named_analysis.end()) { selected.error = "Unknown analysis view: " + name; return selected; }
            selected.views.push_back(*found);
        }
    }
    std::set<ValueId> roots;
    std::size_t selectedCount = 0, transforms = 0;
    std::set<std::uint32_t> transformInputs;
    for (const auto& view : selected.views) {
        std::vector<std::uint32_t> pending{view.value};
        std::set<std::uint32_t> dependencies;
        while (!pending.empty()) {
            const auto id = pending.back(); pending.pop_back();
            if (id >= program.analysis.size()) { selected.error = "Analysis references an invalid node."; return selected; }
            if (!dependencies.insert(id).second) continue;
            if (dependencies.size() > 128) { selected.error = "Analysis budget exceeded: each view may depend on at most 128 nodes."; return selected; }
            const auto& node = program.analysis[id];
            if (!selected.needed[id]) {
                selected.needed[id] = true;
                if (++selectedCount > 512) { selected.error = "Analysis budget exceeded: selected views may use at most 512 nodes."; return selected; }
                if (node.op == AnalysisOp::spectrum || node.op == AnalysisOp::mel_spectrum) {
                    if (!node.inputs.empty()) transformInputs.insert(node.inputs[0]);
                    if (++transforms > 32) { selected.error = "Analysis budget exceeded: selected views may use at most 32 spectral transforms."; return selected; }
                }
            }
            if (node.op == AnalysisOp::probe) {
                if (node.signal >= program.nodes.size()) { selected.error = "Probe references an invalid signal."; return selected; }
                roots.insert(node.signal);
            }
            for (auto input : node.inputs) {
                if (input >= id) { selected.error = "Analysis graph must be topological."; return selected; }
                pending.push_back(input);
            }
        }
    }
    // Waveform result previews also expose a spectrum. Count this work before
    // capture; direct probes reuse the already-computed raw capture FFT.
    for (const auto& view : selected.views) {
        const auto& node = program.analysis[view.value];
        if (node.shape == AnalysisShape::waveform && node.op != AnalysisOp::probe && transformInputs.insert(view.value).second)
            if (++transforms > 32) { selected.error = "Analysis budget exceeded: selected views may use at most 32 spectral transforms."; return selected; }
    }
    selected.roots.assign(roots.begin(), roots.end());
    if (roots.size() > captureChannels) selected.error = "Capture budget exceeded: selected analysis needs " + std::to_string(roots.size()) + " signals; the limit is 8.";
    return selected;
}
std::string signalName(const Program& program, ValueId id) {
    for (const auto& named : program.named_values) if (named.value == id) return named.name;
    return "$value" + std::to_string(id);
}
std::vector<std::complex<double>> fft(const std::vector<double>& samples) {
    std::vector<std::complex<double>> data(windowSize);
    for (std::size_t i = 0; i < windowSize; ++i) {
        const double hann = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) / (windowSize - 1));
        data[i] = samples[i] * hann;
    }
    for (std::size_t i = 1, j = 0; i < windowSize; ++i) {
        std::size_t bit = windowSize >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(data[i], data[j]);
    }
    for (std::size_t length = 2; length <= windowSize; length <<= 1) {
        const auto increment = std::polar(1.0, -2.0 * std::numbers::pi / static_cast<double>(length));
        for (std::size_t start = 0; start < windowSize; start += length) {
            std::complex<double> phase{1, 0};
            for (std::size_t j = 0; j < length / 2; ++j) {
                const auto a = data[start + j], b = data[start + j + length / 2] * phase;
                data[start + j] = a + b; data[start + j + length / 2] = a - b; phase *= increment;
            }
        }
    }
    return data;
}
std::vector<double> frequencies(std::size_t count, double sampleRate) {
    std::vector<double> values(count);
    for (std::size_t i = 0; i < count; ++i) values[i] = static_cast<double>(i) * sampleRate / windowSize;
    return values;
}
struct Evaluation {
    AnalysisShape shape = AnalysisShape::waveform;
    Unit unit = Unit::scalar;
    std::vector<double> values;
    std::vector<double> frequencies;
    bool polyControl = false;
    std::size_t sampleCount = 0;
    bool windowReady = true;
    bool signedSpectrum = false;
    std::string error;
};
AnalysisView display(std::string name, const Evaluation& value, std::string kind = "analysis") {
    AnalysisView out; out.name = std::move(name); out.shape = shapeName(value.shape); out.unit = std::string(unit_name(value.unit)); out.kind = std::move(kind); out.error = value.error;
    out.valid = out.error.empty() && !value.values.empty();
    out.sampleCount = value.sampleCount;
    for (double sample : value.values) if (!std::isfinite(sample) || std::abs(sample) > std::numeric_limits<float>::max()) {
        out.valid = false; out.error = "Analysis result exceeds the finite display range."; break;
    }
    out.windowReady = out.valid && value.windowReady;
    out.signedSpectrum = value.signedSpectrum;
    if (!out.valid) return out;
    if (value.shape == AnalysisShape::scalar) { out.scalar = value.values[0]; out.values.push_back(static_cast<float>(out.scalar)); return out; }
    if (value.shape == AnalysisShape::waveform) {
        double power = 0;
        for (auto sample : value.values) { power += sample * sample; out.peak = std::max(out.peak,std::abs(sample)); }
        out.rms = std::sqrt(power / value.values.size());
        // Trigger the detail scope on an interpolated rising zero crossing.
        // Keep a fixed sample span so pitch remains visible rather than normalized.
        const auto span = std::min<std::size_t>(1024, value.values.size() / 2);
        if (span >= 2) {
            double start = 0;
            for (std::size_t i = 1; i + span + 1 < value.values.size() / 2 + span; ++i)
                if (value.values[i-1] <= 0 && value.values[i] > 0) {
                    start = double(i-1) - value.values[i-1] / (value.values[i]-value.values[i-1]); break;
                }
            const auto points = std::min<std::size_t>(span, 1024);
            for (std::size_t p = 0; p < points; ++p) {
                const double at = start + double(p) * (span-1) / (points-1);
                const auto i = static_cast<std::size_t>(at);
                out.detail.push_back(static_cast<float>(value.values[i] + (value.values[i+1]-value.values[i]) * (at-i)));
            }
            out.detailSeconds = double(span-1); // Converted to seconds by snapshot().
        }
        const auto buckets = std::min(waveformPoints / 2, value.values.size());
        for (std::size_t bucket = 0; bucket < buckets; ++bucket) {
            const auto first = bucket * value.values.size() / buckets, last = (bucket + 1) * value.values.size() / buckets;
            auto limits = std::minmax_element(value.values.begin() + static_cast<std::ptrdiff_t>(first), value.values.begin() + static_cast<std::ptrdiff_t>(last));
            // Preserve chronological extrema, including a short transient peak.
            if (limits.first < limits.second) { out.values.push_back(static_cast<float>(*limits.first)); out.values.push_back(static_cast<float>(*limits.second)); }
            else { out.values.push_back(static_cast<float>(*limits.second)); out.values.push_back(static_cast<float>(*limits.first)); }
        }
    } else {
        const auto count = std::min(value.shape == AnalysisShape::mel_spectrum ? melBands : spectrumPoints, value.values.size());
        for (std::size_t bucket = 0; bucket < count; ++bucket) {
            const auto edge = [&](std::size_t b) { return value.shape == AnalysisShape::mel_spectrum ? b * value.values.size() / count : static_cast<std::size_t>(std::floor(std::expm1(std::log1p(double(value.values.size())) * double(b) / count))); };
            const auto first = std::min(edge(bucket), value.values.size()), last = bucket+1 == count ? value.values.size() : std::min(edge(bucket+1), value.values.size());
            if (first >= last) continue;
            auto peak = first;
            for (auto i = first + 1; i < last; ++i) if (std::abs(value.values[i]) > std::abs(value.values[peak])) peak = i;
            out.values.push_back(static_cast<float>(value.values[peak]));
            if (peak < value.frequencies.size()) out.frequencies.push_back(static_cast<float>(value.frequencies[peak]));
        }
    }
    return out;
}
} // namespace

ProbeSelectionResult makeProbeSelection(const Program& program, std::uint64_t revision,
                                       std::span<const ValueId> additionalValues, int voice,
                                       std::span<const std::string> analysisNames, bool defaultAnalysis, ProbeProjection projection) {
    ProbeSelectionResult result; result.selection.revision = revision; result.selection.voice = voice; result.selection.projection = projection;
    if (voice < -1 || voice >= 32) { result.error = "Voice selection must be mixed (-1) or a slot from 0 to 31."; return result; }
    if (projection<ProbeProjection::mid || projection>ProbeProjection::side) { result.error="Invalid audio projection."; return result; }
    auto selected = select(program, analysisNames, defaultAnalysis); result.error = selected.error;
    for (const auto& view : selected.views) result.analysisNames.push_back(view.name);
    if (!result.error.empty()) return result;
    auto roots = selected.roots;
    for (auto value : additionalValues) if (std::find(roots.begin(), roots.end(), value) == roots.end()) roots.push_back(value);
    if (roots.size() > captureChannels) { result.error = "Capture budget exceeded: selection needs " + std::to_string(roots.size()) + " signals; the limit is 8."; return result; }
    const auto audio = audioValues(program);
    for (auto value : roots) {
        const bool probeRoot = std::find(selected.roots.begin(), selected.roots.end(), value) != selected.roots.end();
        if (value >= program.nodes.size() || (!probeRoot && std::none_of(program.named_values.begin(), program.named_values.end(), [&](const auto& named) { return named.value == value; }))) {
            result.error = "Only existing named DSP signals can be captured."; return result;
        }
        const auto index = result.selection.count++;
        result.selection.values[index] = value; result.selection.audio[index] = audio[value];
    }
    return result;
}

struct Analyzer::Impl {
    Program program;
    Selection selected;
    std::uint64_t expectedRevision = 0, resets = 0;
    std::array<std::array<double, windowSize>, captureChannels> history{};
    static constexpr std::size_t beatPoints = 256;
    struct Beat {
        std::array<float, beatPoints> low{}, high{};
        std::array<double, beatPoints> cycle;
        Beat() { cycle.fill(-std::numeric_limits<double>::infinity()); }
    };
    std::array<Beat, captureChannels> beats;
    CaptureFrame previous{};
    bool continuous = true, captureIdle = true, haveMode = false;
    std::uint64_t keyEpoch = 0, performanceStart = 0;
    double quietSeconds = 0;
    void observe(const CaptureFrame& frame) {
        if(frame.channels>captureChannels||frame.count>maxFrames||!std::isfinite(frame.sampleRate)||frame.sampleRate<=0||frame.start>std::numeric_limits<std::uint64_t>::max()-frame.count){consume(frame);return;}
        const bool modeChanged = haveMode && continuous != frame.continuous;
        const bool newKey = keyEpoch != frame.keyEpoch;
        continuous = frame.continuous; haveMode = true; keyEpoch = frame.keyEpoch;
        if (modeChanged) {
            clear("waiting for current performance capture"); hasPrevious = false;
            captureIdle = true; quietSeconds = 0;
        }
        if (continuous) { captureIdle = false; quietSeconds = 0; consume(frame); return; }
        if (frame.performanceActive) {
            quietSeconds = 0;
            if (captureIdle) {
                clear("waiting for performance capture"); hasPrevious = false;
                captureIdle = false;
                performanceStart = newKey && frame.keyStart >= frame.start && frame.keyStart < frame.start + frame.count
                    ? frame.keyStart : frame.start;
            }
        } else {
            if (captureIdle) { previous = frame; dirty = true; return; }
            // Flush the FFT and let the renderer's release envelope reach silence.
            // The session supplies zero outside the sounding voice lifetime.
            quietSeconds += double(frame.count) / frame.sampleRate;
            if (quietSeconds >= 1.5 + double(windowSize) / frame.sampleRate) {
                clear("waiting for a key or sequencer playback"); hasPrevious = false;
                captureIdle = true; previous = frame; return;
            }
        }
        CaptureFrame observed = frame;
        const auto start = std::max(frame.start, performanceStart);
        const auto skip = static_cast<std::size_t>(start - frame.start);
        if (skip >= frame.count) return;
        observed.start = start; observed.count = frame.count - static_cast<std::uint32_t>(skip);
        // Live measurement is independent of transport playback. A manual
        // note (or its release/FFT drain) must not invent a looping beat clock.
        for (std::size_t c = 0; c < frame.channels; ++c)
            std::copy_n(frame.samples[c].begin() + skip, observed.count, observed.samples[c].begin());
        // Overlapping keys do not break a mixed spectrum or discard release tails.
        consume(observed);
    }
    std::array<std::size_t, captureChannels> write{}, filled{};
    bool hasPrevious = false, dirty = true;
    std::string reason = "waiting for capture";
    AnalysisSnapshot cached;

    void clear(std::string why) { beats = {}; write.fill(0); filled.fill(0); dirty = true; reason = std::move(why); ++resets; }
    void clearChannel(std::size_t channel) {
        if (filled[channel] != 0) ++resets;
        beats[channel] = {};
        write[channel] = filled[channel] = 0;
        dirty = true;
    }
    void consume(const CaptureFrame& frame) {
        if (!selected.error.empty()) { clear(selected.error); return; }
        if (frame.revision != expectedRevision) { clear("waiting for the current source revision"); hasPrevious = false; return; }
        if (!std::isfinite(frame.sampleRate) || frame.sampleRate <= 0 || frame.channels > captureChannels || frame.count > maxFrames || frame.count > std::numeric_limits<std::uint64_t>::max() - frame.start) {
            clear("invalid capture frame"); hasPrevious = false; return;
        }
        if (frame.count == 0) return;
        std::set<ValueId> values;
        for (std::size_t channel = 0; channel < frame.channels; ++channel) {
            if (!values.insert(frame.values[channel]).second || frame.values[channel] >= program.nodes.size()) { clear("invalid capture channel mapping"); hasPrevious = false; return; }
            for (std::size_t i = 0; i < frame.count; ++i) if (!std::isfinite(frame.samples[channel][i])) { clear("non-finite captured signal"); hasPrevious = false; return; }
        }
        for (auto root : selected.roots) if (!values.contains(root)) { clear("waiting for a required probe subscription"); hasPrevious = false; return; }
        if (hasPrevious) {
            bool different = frame.revision != previous.revision || frame.generation != previous.generation || frame.parameterEpoch != previous.parameterEpoch || frame.sampleRate != previous.sampleRate || frame.channels != previous.channels;
            different = different || frame.observationEpoch != previous.observationEpoch || frame.voice != previous.voice || frame.voiceGeneration != previous.voiceGeneration || frame.projection != previous.projection;
            for (std::size_t i = 0; i < frame.channels && !different; ++i) {
                different = frame.values[i] != previous.values[i] || frame.audio[i] != previous.audio[i];
            }
            if (different) clear("waiting for a continuous window after a version or voice change");
            else if (frame.sequence != previous.sequence + 1 || frame.start != previous.start + previous.count) clear("capture gap: waiting for 4096 continuous samples");
            else for (std::size_t i = 0; i < frame.channels; ++i) {
                // A mixed control channel follows one voice; its identity must not
                // invalidate independent mixed-audio channels in the same frame.
                if (!frame.audio[i] && (frame.channelVoices[i] != previous.channelVoices[i] ||
                    frame.channelVoiceGenerations[i] != previous.channelVoiceGenerations[i])) clearChannel(i);
            }
        }
        if (hasPrevious && (frame.transport.bpm != previous.transport.bpm || frame.transport.playing != previous.transport.playing || frame.transport.positioned != previous.transport.positioned ||
            (frame.transport.positioned && std::abs(frame.transport.ppq - (previous.transport.ppq + (previous.transport.playing ? previous.count * previous.transport.bpm / (60 * previous.sampleRate) : 0))) > 1e-7))) beats = {};
        previous = frame; hasPrevious = true;
        if (frame.transition) { clear("transition: waiting for a continuous window"); return; }
        for (std::size_t channel = 0; channel < frame.channels; ++channel) {
            if (frame.continuous && frame.transport.positioned && frame.transport.playing && std::isfinite(frame.transport.ppq) && std::isfinite(frame.transport.bpm) && frame.transport.bpm > 0) {
                auto& beat = beats[channel];
                for (std::size_t i = 0; i < frame.count; ++i) {
                    const double ppq = frame.transport.ppq + double(i) * frame.transport.bpm / (60 * frame.sampleRate);
                    const double cycle = std::floor(ppq);
                    const auto bin = std::min(beatPoints-1, static_cast<std::size_t>((ppq-cycle) * beatPoints));
                    const float sample = frame.samples[channel][i];
                    if (beat.cycle[bin] != cycle) { beat.cycle[bin] = cycle; beat.low[bin] = beat.high[bin] = sample; }
                    else { beat.low[bin] = std::min(beat.low[bin], sample); beat.high[bin] = std::max(beat.high[bin], sample); }
                }
            }
            for (std::size_t i = 0; i < frame.count; ++i) history[channel][(write[channel] + i) % windowSize] = frame.samples[channel][i];
            write[channel] = (write[channel] + frame.count) % windowSize;
            filled[channel] = std::min(windowSize, filled[channel] + frame.count);
        }
        dirty = true;
    }
    AnalysisSnapshot snapshot() {
        if (!dirty) return cached;
        dirty = false; cached = {}; cached.revision = expectedRevision; cached.resets = resets; cached.status = reason;
        if (hasPrevious || (captureIdle && previous.revision == expectedRevision)) {
            cached.voice = previous.voice; cached.projection = previous.projection; cached.generation = previous.generation;
            cached.parameterEpoch = previous.parameterEpoch; cached.observationEpoch = previous.observationEpoch; cached.sampleRate = previous.sampleRate;
            if (hasPrevious) cached.end = previous.start + previous.count;
        }
        if (!hasPrevious || previous.channels == 0 || !selected.error.empty()) {
            for (const auto& named : selected.views) {
                AnalysisView waiting; waiting.name = named.name; waiting.kind = "analysis";
                if (named.value < program.analysis.size()) { waiting.shape = shapeName(program.analysis[named.value].shape); waiting.unit = std::string(unit_name(program.analysis[named.value].unit)); }
                waiting.error = selected.error.empty() ? reason : selected.error; cached.results.push_back(std::move(waiting));
            }
            return cached;
        }
        cached.ready = std::all_of(filled.begin(), filled.begin() + previous.channels, [](auto count) { return count == windowSize; });
        const auto longest = *std::max_element(filled.begin(), filled.begin() + previous.channels);
        cached.start = cached.end - longest;
        cached.status = cached.ready ? "ready" : reason;
        std::map<ValueId, Evaluation> captures;
        for (std::size_t channel = 0; channel < previous.channels; ++channel) {
            Evaluation value; value.unit = program.nodes[previous.values[channel]].unit; value.values.resize(filled[channel]);
            value.sampleCount = filled[channel]; value.windowReady = filled[channel] == windowSize;
            if (value.values.empty()) value.error = reason;
            value.polyControl = previous.voice < 0 && !previous.audio[channel] && program.voice_policy.mode == VoiceMode::poly;
            for (std::size_t i = 0; i < value.values.size(); ++i)
                value.values[i] = history[channel][(write[channel] + windowSize - filled[channel] + i) % windowSize];
            auto kind = previous.voice >= 0 ? "selected_voice" : previous.audio[channel] ? "mixed_audio" : "last_voice_control";
            auto view = display(signalName(program, previous.values[channel]), value, kind);
            view.detailSeconds /= previous.sampleRate;
            if (view.valid && previous.continuous && previous.transport.positioned && previous.transport.playing && std::isfinite(previous.transport.ppq) && previous.transport.bpm > 0) {
                const double ppq = previous.transport.ppq + double(previous.count-1) * previous.transport.bpm / (60 * previous.sampleRate);
                const double cycle = std::floor(ppq);
                view.beatAvailable = true; view.beatPhase = ppq-cycle;
                for (std::size_t bin = 0; bin < beatPoints; ++bin) {
                    const double age = cycle-beats[channel].cycle[bin];
                    view.beatMin.push_back(beats[channel].low[bin]); view.beatMax.push_back(beats[channel].high[bin]);
                    view.beatAge.push_back(age == 0 || age == 1 ? static_cast<float>(age) : -1.f);
                }
            }
            cached.hasCapture = cached.hasCapture || view.valid;
            if (value.windowReady && !value.polyControl) {
                const auto transformed = fft(value.values);
                Evaluation spectrum; spectrum.shape = AnalysisShape::spectrum;
                spectrum.values.resize(windowSize/2+1); spectrum.frequencies = frequencies(spectrum.values.size(),previous.sampleRate);
                for (std::size_t bin=0;bin<spectrum.values.size();++bin)
                    spectrum.values[bin] = std::abs(transformed[bin]) * ((bin==0 || bin==windowSize/2) ? 1.0 : 2.0) / ((windowSize-1)*0.5);
                auto plotted = display(view.name,spectrum);
                view.spectrum = std::move(plotted.values); view.spectrumFrequencies = std::move(plotted.frequencies);
            }
            cached.signals.push_back(std::move(view));
            captures.emplace(previous.values[channel], std::move(value));
        }
        std::vector<Evaluation> values(program.analysis.size());
        std::map<std::uint32_t, std::vector<std::complex<double>>> transforms;
        for (std::uint32_t id = 0; id < program.analysis.size(); ++id) {
            if (!selected.needed[id]) continue;
            const auto& node = program.analysis[id]; auto& value = values[id]; value.shape = node.shape; value.unit = node.unit;
            if (node.op == AnalysisOp::probe) { value = captures.at(node.signal); continue; }
            if (node.op == AnalysisOp::constant) { value.values = {node.initial_value}; value.signedSpectrum = node.initial_value < 0; continue; }
            const auto unary = node.op == AnalysisOp::power || node.op == AnalysisOp::spectrum || node.op == AnalysisOp::mel_spectrum;
            if (node.inputs.size() != (unary ? 1u : 2u)) { value.error = "Invalid analysis argument count."; continue; }
            const auto& a = values[node.inputs[0]];
            value.sampleCount = a.sampleCount;
            if (!a.error.empty()) { value.error = a.error; continue; }
            if (a.polyControl) { value.error = "Select one voice to analyze polyphonic control signals."; continue; }
            if (unary) {
                if (a.shape != AnalysisShape::waveform || a.values.size() != windowSize) { value.error = "Waiting for 4096 continuous samples."; continue; }
                if (node.op == AnalysisOp::power) {
                    double power = 0; for (auto sample : a.values) power += sample * sample;
                    value.values = {power / windowSize};
                } else {
                    if (!transforms.contains(node.inputs[0])) transforms[node.inputs[0]] = fft(a.values);
                    const auto& transformed = transforms.at(node.inputs[0]);
                    if (node.op == AnalysisOp::spectrum) {
                        value.values.resize(windowSize / 2 + 1); value.frequencies = frequencies(value.values.size(), previous.sampleRate);
                        const double windowSum = (windowSize - 1) * 0.5;
                        for (std::size_t bin = 0; bin < value.values.size(); ++bin) value.values[bin] = std::abs(transformed[bin]) * ((bin == 0 || bin == windowSize / 2) ? 1.0 : 2.0) / windowSum;
                    } else {
                        std::array<double, melBands + 2> edges{};
                        const double topMel = 2595.0 * std::log10(1.0 + previous.sampleRate / 1400.0);
                        for (std::size_t i = 0; i < edges.size(); ++i) edges[i] = 700.0 * (std::pow(10.0, topMel * static_cast<double>(i) / (melBands + 1) / 2595.0) - 1.0);
                        value.values.assign(melBands, 0); value.frequencies.resize(melBands);
                        const double normalization = windowSize * (windowSize - 1) * 3.0 / 8.0;
                        for (std::size_t band = 0; band < melBands; ++band) {
                            value.frequencies[band] = edges[band + 1];
                            for (std::size_t bin = 0; bin <= windowSize / 2; ++bin) {
                                const double hz = static_cast<double>(bin) * previous.sampleRate / windowSize;
                                if (hz <= edges[band] || hz >= edges[band + 2]) continue;
                                const double weight = hz <= edges[band + 1] ? (hz - edges[band]) / (edges[band + 1] - edges[band]) : (edges[band + 2] - hz) / (edges[band + 2] - edges[band + 1]);
                                const double doubling = bin == 0 || bin == windowSize / 2 ? 1.0 : 2.0;
                                value.values[band] += weight * std::norm(transformed[bin]) * doubling / normalization;
                            }
                        }
                    }
                }
            } else {
                const auto& b = values[node.inputs[1]];
                value.signedSpectrum = node.op == AnalysisOp::subtract || a.signedSpectrum || b.signedSpectrum;
                value.sampleCount = std::max(a.sampleCount, b.sampleCount);
                value.windowReady = a.windowReady && b.windowReady;
                if (!b.error.empty()) { value.error = b.error; continue; }
                if (b.polyControl) { value.error = "Select one voice to analyze polyphonic control signals."; continue; }
                const bool aScalar = a.shape == AnalysisShape::scalar, bScalar = b.shape == AnalysisShape::scalar;
                if (a.values.empty() || b.values.empty() || (!aScalar && !bScalar && (a.shape != b.shape || a.values.size() != b.values.size() || a.frequencies != b.frequencies))) { value.error = "Analysis operands have incompatible windows or axes."; continue; }
                const auto count = std::max(a.values.size(), b.values.size()); value.values.resize(count); value.frequencies = aScalar ? b.frequencies : a.frequencies;
                for (std::size_t i = 0; i < count; ++i) {
                    const double x = a.values[aScalar ? 0 : i], y = b.values[bScalar ? 0 : i];
                    switch (node.op) {
                        case AnalysisOp::add: value.values[i] = x + y; break;
                        case AnalysisOp::subtract: value.values[i] = x - y; break;
                        case AnalysisOp::multiply: value.values[i] = x * y; break;
                        case AnalysisOp::divide: if (y == 0) value.error = "Division by zero in analysis."; else value.values[i] = x / y; break;
                        default: value.error = "Unsupported analysis operation."; break;
                    }
                    if (!std::isfinite(value.values[i])) value.error = "Non-finite analysis result.";
                    if (!value.error.empty()) { value.values.clear(); break; }
                }
            }
        }
        for (const auto& named : selected.views) {
            auto view = display(named.name, values[named.value]);
            view.detailSeconds /= previous.sampleRate;
            const auto& node = program.analysis[named.value];
            // A direct probe is another view of the exact captured samples;
            // reuse its FFT rather than calculating a second spectrum.
            if (node.op == AnalysisOp::probe) {
                const auto root = std::find(previous.values.begin(), previous.values.begin() + previous.channels, node.signal);
                if (root != previous.values.begin() + previous.channels) {
                    const auto& captured = cached.signals[static_cast<std::size_t>(root - previous.values.begin())];
                    view.spectrum = captured.spectrum; view.spectrumFrequencies = captured.spectrumFrequencies;
                    view.beatAvailable = captured.beatAvailable; view.beatPhase = captured.beatPhase;
                    view.beatMin = captured.beatMin; view.beatMax = captured.beatMax; view.beatAge = captured.beatAge;
                }
            }
            const auto& value = values[named.value];
            if (node.op != AnalysisOp::probe && value.shape == AnalysisShape::waveform && view.valid &&
                value.windowReady && !value.polyControl && value.values.size() == windowSize) {
                if (!transforms.contains(named.value)) transforms[named.value] = fft(value.values);
                const auto& transformed = transforms.at(named.value);
                Evaluation spectrum; spectrum.shape = AnalysisShape::spectrum;
                spectrum.values.resize(windowSize / 2 + 1); spectrum.frequencies = frequencies(spectrum.values.size(), previous.sampleRate);
                for (std::size_t bin = 0; bin < spectrum.values.size(); ++bin)
                    spectrum.values[bin] = std::abs(transformed[bin]) * ((bin == 0 || bin == windowSize / 2) ? 1.0 : 2.0) / ((windowSize - 1) * 0.5);
                auto plotted = display(view.name, spectrum);
                view.spectrum = std::move(plotted.values); view.spectrumFrequencies = std::move(plotted.frequencies);
            }
            cached.results.push_back(std::move(view));
        }
        return cached;
    }
};

Analyzer::Analyzer() : impl_(std::make_unique<Impl>()) {}
Analyzer::~Analyzer() = default;
void Analyzer::configure(const Program& program, std::uint64_t revision, std::span<const std::string> analysisNames, bool defaultAnalysis) {
    impl_->program = program; impl_->expectedRevision = revision; impl_->selected = select(program, analysisNames, defaultAnalysis); impl_->hasPrevious = false; impl_->captureIdle = true; impl_->quietSeconds = 0;
    impl_->clear(impl_->selected.error.empty() ? "waiting for 4096 continuous samples" : impl_->selected.error);
}
void Analyzer::consume(const CaptureFrame& frame) { impl_->observe(frame); }
AnalysisSnapshot Analyzer::snapshot() {
    auto result=impl_->snapshot();
    result.continuous=impl_->continuous;result.captureHeld=false;
    result.captureIdle=!impl_->continuous&&impl_->captureIdle;result.keyEpoch=impl_->keyEpoch;
    if(result.captureIdle && impl_->selected.error.empty()) {
        result.signals.clear();result.results.clear();result.status="waiting for a key or sequencer playback";
    }
    return result;
}
void Analyzer::reset(std::string reason) { impl_->hasPrevious = false; impl_->captureIdle = true; impl_->quietSeconds = 0; impl_->clear(std::move(reason)); }
} // namespace flows::app
