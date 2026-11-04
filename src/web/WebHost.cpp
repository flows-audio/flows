// Browser host. The portable core runs on the page's main thread (requests)
// and pthreads (compilation, analysis, assistant); AudioRuntime runs in a Wasm
// Audio Worklet that shares the same memory, so the lock-free queues between
// them work exactly as in the native app. JavaScript reaches this file
// through the flows_* exports below (see ui/src/web/core.ts).
#include "app/AppCore.hpp"
#include "app/Files.hpp"
#include "app/Protocol.hpp"
#include "app/Text.hpp"

#include <emscripten.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>
#include <emscripten/webaudio.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>

namespace {
namespace fs = std::filesystem;
using flows::json::Value;
using namespace flows;
using namespace flows::app;

// Web MIDI messages from the page to the audio worklet.
struct WebMidi {
    std::array<std::uint8_t, 3> bytes{};
    std::uint8_t size = 0;
};

struct Host {
    std::vector<std::string> bundleStorage;
    std::vector<BundledFile> bundle;
    std::unique_ptr<AppCore> core;
    StateStream stream;
    SpscQueue<WebMidi, 512> midi;
    std::array<WebMidi, 256> midiBlock{};
    std::array<RawMidi, 256> rawBlock{};
    EMSCRIPTEN_WEBAUDIO_T context = 0, node = 0;
    void* audioStack = nullptr;
};
Host* host = nullptr;

char* copy(const std::string& text) {
    auto* result = static_cast<char*>(std::malloc(text.size() + 1));
    std::memcpy(result, text.data(), text.size());
    result[text.size()] = 0;
    return result;
}
std::string reply(bool ok, const Value& value) {
    return json::stringify(ok ? json::object({{"ok", true}, {"result", value}}) : json::object({{"ok", false}, {"error", value}}));
}

// The audio worklet scope has no high-resolution wall clock in every browser.
EM_JS(double, flowsWorkletNow, (), {
    return (globalThis.performance && performance.now) ? performance.now() / 1000 : 0;
});
double workletClock() noexcept { return flowsWorkletNow(); }

bool process(int, const AudioSampleFrame*, int numOutputs, AudioSampleFrame* outputs, int, const AudioParamFrame*, void*) {
    if (!host || !host->core || numOutputs < 1) return true;
    auto& output = outputs[0];
    const int frames = output.samplesPerChannel;
    std::size_t count = 0;
    WebMidi message;
    // One past the runtime's 256-message budget is enough to report overflow.
    while (count < host->rawBlock.size() && host->midi.pop(message)) {
        host->midiBlock[count] = message;
        host->rawBlock[count] = {host->midiBlock[count].bytes.data(), message.size, 0};
        ++count;
    }
    AudioBlock block;
    block.left = output.data;
    block.right = output.numberOfChannels > 1 ? output.data + frames : nullptr;
    block.frames = frames;
    block.midi = std::span<const RawMidi>(host->rawBlock.data(), count);
    host->core->audio().process(block);
    for (int channel = 2; channel < output.numberOfChannels; ++channel)
        std::memset(output.data + channel * frames, 0, sizeof(float) * static_cast<std::size_t>(frames));
    return true;
}

EM_JS(void, flowsAudioStatus, (const char* state, int node), {
    if (Module.onFlowsAudio) Module.onFlowsAudio(UTF8ToString(state), node);
});
void processorCreated(EMSCRIPTEN_WEBAUDIO_T context, bool success, void*) {
    if (!success) {
        flowsAudioStatus("failed", 0);
        return;
    }
    int channels[1] = {2};
    EmscriptenAudioWorkletNodeCreateOptions options{};
    options.numberOfInputs = 0;
    options.numberOfOutputs = 1;
    options.outputChannelCounts = channels;
    host->node = emscripten_create_wasm_audio_worklet_node(context, "flows-instrument", &options, &process, nullptr);
    emscripten_audio_node_connect(host->node, context, 0, 0);
    flowsAudioStatus("running", host->node);
}
void workletStarted(EMSCRIPTEN_WEBAUDIO_T context, bool success, void*) {
    if (!success) {
        flowsAudioStatus("failed", 0);
        return;
    }
    WebAudioWorkletProcessorCreateOptions options{};
    options.name = "flows-instrument";
    emscripten_create_wasm_audio_worklet_processor_async(context, &options, &processorCreated, nullptr);
}

// Assistant HTTP: fetch() runs on the main thread; the calling pthread waits.
struct FetchCall {
    std::atomic<std::uint32_t> done{0};
    int status = 0, error = 0;
    char* body = nullptr;
    std::size_t length = 0;
};
EM_JS(void, flowsFetchStart, (FetchCall * call, const char* url, const char* body, std::size_t bodyLength,
                              const char* headers, int timeoutMs, std::size_t limit), {
    Module.flowsFetch(call, UTF8ToString(url), HEAPU8.slice(body, body + bodyLength), JSON.parse(UTF8ToString(headers)),
                      timeoutMs, limit);
});
EM_JS(void, flowsFetchAbort, (FetchCall * call), { Module.flowsFetchAbort(call); });
struct FetchStart {
    FetchCall* call;
    const std::string *url, *body, *headers;
    int timeoutMs;
    std::size_t limit;
};
class WebHttpClient final : public HttpClient {
public:
    HttpResult post(const std::string& url, const std::string& body,
                    const std::vector<std::pair<std::string, std::string>>& headers, const std::function<bool()>& cancelled,
                    int timeoutMs, std::size_t responseLimit) override {
        auto call = std::make_unique<FetchCall>();
        json::Object headerObject;
        for (const auto& [name, value] : headers) headerObject.set(name, value);
        const auto headerText = json::stringify(Value(std::move(headerObject)));
        // The main thread copies the arguments before this returns.
        FetchStart start{call.get(), &url, &body, &headerText, timeoutMs, responseLimit};
        emscripten_proxy_sync(emscripten_proxy_get_system_queue(), emscripten_main_runtime_thread_id(), &startFetch, &start);
        bool aborted = false;
        while (call->done.load() == 0) {
            if (!aborted && cancelled()) {
                aborted = true;
                emscripten_proxy_async(emscripten_proxy_get_system_queue(), emscripten_main_runtime_thread_id(), &abortFetch,
                                       call.get());
            }
            emscripten_futex_wait(&call->done, 0, 25);
        }
        HttpResult result;
        result.status = call->status;
        if (call->body) {
            result.body.assign(call->body, call->length);
            std::free(call->body);
        }
        result.error = aborted ? HttpResult::Error::cancelled : static_cast<HttpResult::Error>(call->error);
        return result;
    }

private:
    static void startFetch(void* argument) {
        const auto& start = *static_cast<FetchStart*>(argument);
        flowsFetchStart(start.call, start.url->c_str(), start.body->data(), start.body->size(), start.headers->c_str(),
                        start.timeoutMs, start.limit);
    }
    static void abortFetch(void* call) { flowsFetchAbort(static_cast<FetchCall*>(call)); }
};
}  // namespace

extern "C" {
// Called by JavaScript when a fetch settles. error: 0 none, 2 timeout, 3 connect, 4 too large.
EMSCRIPTEN_KEEPALIVE void flows_fetch_complete(FetchCall* call, int status, char* body, std::size_t length, int error) {
    call->status = status;
    call->body = body;
    call->length = length;
    call->error = error;
    call->done.store(1);
    emscripten_futex_wake(&call->done, 1);
}

EMSCRIPTEN_KEEPALIVE int flows_start(const char* workspace, const char* settings, const char* capabilities) {
    try {
        host = new Host;
        std::vector<fs::path> files;
        for (const auto* folder : {"/bundle/presets", "/bundle/samples"}) {
            std::error_code error;
            for (fs::directory_iterator it(folder, error), end; !error && it != end; it.increment(error))
                if (it->is_regular_file()) files.push_back(it->path());
        }
        std::sort(files.begin(), files.end());
        host->bundleStorage.reserve(files.size());
        for (const auto& file : files) {
            host->bundleStorage.push_back(files::read(file, 8 * 1024 * 1024));
            host->bundle.push_back({file.filename().string(), host->bundleStorage.back()});
        }
        CoreOptions options;
        options.workspace = workspace;
        options.workspaceIdentity = workspace;
        options.settingsDirectory = settings;
        options.standalone = true;
        options.bundled = host->bundle;
        options.secrets = memoryAISecrets(false);
        options.http = std::make_shared<WebHttpClient>();
        options.capabilities = json::parse(capabilities);
        host->core = std::make_unique<AppCore>(std::move(options));
        host->core->audio().setClock(&workletClock);
        return 1;
    } catch (const std::exception& error) {
        EM_ASM({ console.error('Flows core failed to start: ' + UTF8ToString($0)); }, error.what());
        return 0;
    }
}
// Returns {"ok":true,"result":...} or {"ok":false,"error":"..."}. Free with flows_free.
EMSCRIPTEN_KEEPALIVE char* flows_request(const char* method, const char* params) {
    try {
        auto result = host->core->request(method, json::parseBounded(params, 128u * 1024 * 1024));
        return copy(reply(true, result));
    } catch (const std::exception& error) {
        return copy(reply(false, Value(std::string(error.what()))));
    }
}
// State slices changed since the last call, or null.
EMSCRIPTEN_KEEPALIVE char* flows_state() {
    const auto delta = host->stream.next(host->core->snapshot());
    return delta ? copy(json::stringify(*delta)) : nullptr;
}
EMSCRIPTEN_KEEPALIVE char* flows_telemetry() { return copy(json::stringify(host->core->telemetry())); }
EMSCRIPTEN_KEEPALIVE char* flows_agent_events() { return copy(json::stringify(json::array(host->core->takeAgentEvents()))); }
EMSCRIPTEN_KEEPALIVE void flows_free(char* text) { std::free(text); }
EMSCRIPTEN_KEEPALIVE void flows_set_capability(const char* name, const char* value) {
    host->core->setCapability(name, json::parse(value));
}
// Starts the Wasm audio worklet on an AudioContext registered with
// emscriptenRegisterAudioObject. Completion arrives via Module.onFlowsAudio.
EMSCRIPTEN_KEEPALIVE void flows_audio_attach(EMSCRIPTEN_WEBAUDIO_T context) {
    host->context = context;
    host->core->audio().prepare(emscripten_audio_context_sample_rate(context), emscripten_audio_context_quantum_size(context));
    constexpr std::size_t stackSize = 512 * 1024;
    if (!host->audioStack) host->audioStack = std::aligned_alloc(16, stackSize);
    emscripten_start_wasm_audio_worklet_thread_async(context, host->audioStack, stackSize, &workletStarted, nullptr);
}
// The output stopped or resumed (suspended context, hidden page). Only
// atomics change here: the worklet may be rendering at the same time.
EMSCRIPTEN_KEEPALIVE void flows_audio_running(int running, double sampleRate) {
    auto& audio = host->core->audio();
    if (running) {
        audio.rate.store(sampleRate);
        audio.running.store(true);
        audio.panic.store(true);
    } else
        audio.release();
}
EMSCRIPTEN_KEEPALIVE int flows_midi(int status, int data1, int data2, int size) {
    WebMidi message;
    message.bytes = {static_cast<std::uint8_t>(status), static_cast<std::uint8_t>(data1), static_cast<std::uint8_t>(data2)};
    message.size = static_cast<std::uint8_t>(size);
    return host->midi.push(message) ? 1 : 0;
}
EMSCRIPTEN_KEEPALIVE void flows_release_midi() { host->core->audio().releaseMidiNotes(); }
}
