#include "app/RealtimeSession.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
using namespace flows;
using namespace flows::app;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
std::unique_ptr<PreparedProgram> prepare(std::uint64_t revision, bool mono, bool reverse = false, bool loop = false) {
    const auto source = std::string("import standard as std\nfrom lib.sample import ClassicSampler\nInst.sigstream(std.event.keys.") +
        (mono ? "mono" : "poly") + "()) as key:\n    player = ClassicSampler.make()\n    raw = player(file: \"Samples/test.wav\", trigger: key.trigger, " +
        (reverse ? "reverse: true, " : "") + (loop ? "mode: loop, " : "") + "level: -6db)\n    return raw" + (loop ? " * key.gate" : "") + "\n";
    auto compiled = compile(source, "sample-replay.flows"); require(bool(compiled.program), "Replay test did not compile.");
    auto sample = std::make_shared<SampleData>(); sample->left.assign(16, .5f);
    return std::make_unique<PreparedProgram>(std::move(*compiled.program), source, revision, 48000, SampleBank{sample});
}
// Include the 3 ms playback tail of this deliberately tiny 16-frame sample.
float tick(RealtimeSession& session, std::span<const Event> events = {}, std::size_t count = 256) {
    std::array<float, 256> output{};
    session.process(events, std::span(output).first(count));
    session.reap();
    require(session.status.errors == 0, "A completed held key caused a session event error.");
    float peak = 0;
    for (auto sample : output) peak = std::max(peak, std::abs(sample));
    return peak;
}
Event on(RealtimeSession& session, std::uint64_t id, std::uint64_t offset = 0) { return {session.status.samples + offset, EventType::note_on, id, 48, 1}; }
Event off(RealtimeSession& session, std::uint64_t id) { return {session.status.samples, EventType::note_off, id, 48, 0}; }
void completed_poly() {
    RealtimeSession session; session.submit(prepare(1, false));
    const std::array press{on(session, 1)};
    require(tick(session, press) > .001f && session.status.voices == 0, "One-shot must sound and finish while held.");
    const auto generation = session.status.generation.load();
    session.submit(prepare(2, false, true));
    for (int i = 0; i < 18; ++i) require(tick(session) == 0, "Structural edit restarted an EOF-completed poly sample.");
    require(session.status.generation > generation && session.status.voices == 0, "Rebuild must install without replaying a finished note.");
    const std::array release{off(session, 1)}; tick(session, release);
    const std::array next{on(session, 2)}; require(tick(session, next) > .001f, "New press after suppressed replay must still play.");
    const std::array nextRelease{off(session, 2)}; tick(session, nextRelease);
}
void completed_mono_stack() {
    RealtimeSession session; session.submit(prepare(1, true));
    const std::array press{on(session, 1), on(session, 2, 4)};
    require(tick(session, press) > .001f && session.status.voices == 0, "Mono one-shot stack must reach EOF.");
    session.submit(prepare(2, true, true));
    for (int i = 0; i < 18; ++i) require(tick(session) == 0, "Rebuild revived an older held key from a completed mono kernel.");
    const std::array release{off(session, 2), off(session, 1)}; tick(session, release);
    const std::array next{on(session, 3)}; require(tick(session, next) > .001f, "New mono press must play after a completed stack.");
    const std::array nextRelease{off(session, 3)}; tick(session, nextRelease);
}
void reactivated_mono_priority() {
    RealtimeSession session; session.submit(prepare(1, true));
    const std::array first{on(session, 1)}; tick(session, first);
    require(session.status.voices == 0, "First mono sample should finish.");
    const std::array second{on(session, 2)}; tick(session, second, 4);
    require(session.status.voices == 1, "New mono press should reactivate its resident key stack.");
    session.submit(prepare(2, true, false, true));
    tick(session);
    const std::array releaseSecond{off(session, 2)}; tick(session, releaseSecond);
    for (int i = 0; i < 18; ++i) tick(session);
    require(session.status.voices == 1 && tick(session) > .001f, "Reactivated mono fallback key was lost during rebuild.");
    const std::array releaseFirst{off(session, 1)}; tick(session, releaseFirst);
    require(session.status.voices == 0, "Reactivated mono stack must release normally.");
}
void suppressed_mono_keys_stay_suppressed() {
    RealtimeSession session; session.submit(prepare(1, true));
    const std::array first{on(session, 1)}; tick(session, first);
    session.submit(prepare(2, true, true));
    for (int i = 0; i < 18; ++i) tick(session);
    const std::array second{on(session, 2)}; tick(session, second, 4);
    session.submit(prepare(3, true, false, true)); tick(session);
    const std::array releaseSecond{off(session, 2)}; tick(session, releaseSecond);
    for (int i = 0; i < 18; ++i) tick(session);
    require(session.status.voices == 0 && tick(session) == 0, "A later press resurrected a key excluded from an earlier engine.");
    const std::array releaseFirst{off(session, 1)}; tick(session, releaseFirst);
}
}
int main() {
    try {
        completed_poly(); completed_mono_stack(); reactivated_mono_priority(); suppressed_mono_keys_stay_suppressed();
        std::cout << "Sample replay tests passed: EOF suppression, valid note-off, new presses and mono priority.\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
