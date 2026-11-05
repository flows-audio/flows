#include "app/RealtimeSession.hpp"
#include "app/Analyzer.hpp"
#include "app/DataflowGraph.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>

namespace { bool watched = false; std::size_t allocations = 0, deletions = 0; }
void* operator new(std::size_t size) { if (watched) ++allocations; if (auto* result = std::malloc(size ? size : 1)) return result; throw std::bad_alloc(); }
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* pointer) noexcept { if (watched && pointer) ++deletions; std::free(pointer); }
void operator delete[](void* pointer) noexcept { ::operator delete(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { ::operator delete(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { ::operator delete(pointer); }

using namespace flows;
using namespace flows::app;
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
std::unique_ptr<PreparedProgram> prepare(std::uint64_t revision, float amplitude, std::string gain = "-6db") {
    const auto source = "import standard as std\nfrom lib.sample import ClassicSampler\nInst.sigstream(std.event.keys.poly()) as key:\n    player = ClassicSampler.make()\n    raw = player(file: \"Samples/test.wav\", trigger: key.trigger, pitch: key.pitch, root: C3, mode: loop, level: " + gain + ")\n    p_raw = probe(raw)\n    return raw * key.gate\n";
    auto compiled = compile(source, "sample-session.flows");
    check(bool(compiled.program), "Sample session source must compile");
    auto data = std::make_shared<SampleData>();
    data->left.assign(1024, amplitude); data->right.assign(1024, -amplitude);
    auto result = std::make_unique<PreparedProgram>(std::move(*compiled.program), source, revision, 48000, SampleBank{data});
    const auto named = std::find_if(result->program.named_values.begin(), result->program.named_values.end(), [](const auto& value) { return value.name == "raw"; });
    check(named != result->program.named_values.end() && result->audioValues[named->value], "Sampler must be classified as audible");
    result->probes = {revision, {named->value}, {true}, 1, -1};
    const auto graph = makeDataflowGraph(result->program);
    check(std::any_of(graph.nodes.begin(), graph.nodes.end(), [](const auto& node) { return node.signal == "raw" && node.kind == "audio" && node.op == "sampler"; }), "Sampler dataflow node must be audio");
    return result;
}
void process(RealtimeSession& session, std::span<const Event> events, std::array<float, 256>& left, std::array<float, 256>& right) {
    watched = true; session.processStereo(events, left, right); watched = false;
    check(allocations == 0 && deletions == 0, "Sample session allocated or destroyed memory on audio");
    while (session.frames().readSlot()) session.frames().commitRead();
    check(session.status.errors == 0, "Sample session reported an error");
}
int main() {
    try {
        RealtimeSession session;
        auto first = prepare(1, .25f);
        const auto identity = first->structure;
        session.submit(std::move(first));
        std::array<float, 256> left{}, right{};
        const std::array on{Event{0, EventType::note_on, 1, 48, 1}};
        process(session, on, left, right);
        const auto generation = session.status.generation.load();
        check(left[144] > .1f && right[144] < -.1f, "Sample session lost stereo playback");
        auto gainEdit = prepare(2, .25f, "-12db");
        check(gainEdit->structure == identity, "Gain edits must retain sampler state");
        session.submit(std::move(gainEdit)); process(session, {}, left, right); session.reap();
        check(session.status.generation == generation, "A live sampler gain edit replaced the engine");
        auto changedAsset = prepare(3, .5f, "-12db");
        check(changedAsset->structure != identity, "New PCM under the same path must change prepared identity");
        session.submit(std::move(changedAsset));
        for (int block = 0; block < 6; ++block) { process(session, {}, left, right); session.reap(); }
        check(session.status.generation > generation, "Changed PCM did not install a new engine");
        check(left[100] > .12f && left[100] < .13f, "Changed PCM did not reach output after crossfade");
        const std::array off{Event{session.status.samples.load(), EventType::note_off, 1, 48, 0}};
        process(session, off, left, right);
        check(session.status.voices == 0, "Sampler loop gate release did not retire its voice");
        std::cout << "Sample session: stereo, classification, asset identity, live edits and realtime heap safety passed.\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
