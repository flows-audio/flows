#include "app/RealtimeSession.hpp"
#include "app/Analyzer.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace { bool watchHeap = false; std::size_t allocations = 0, deletions = 0; }
void* operator new(std::size_t size) {
    if (watchHeap) ++allocations;
    if (auto* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { if (watchHeap && p) ++deletions; std::free(p); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }

namespace {
using namespace flows;
using namespace flows::app;
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
std::string source(bool saw = false, std::string level = "-6db", bool mono = false) {
    const std::string type = saw ? "ClassicSaw" : "ClassicSine";
    return "import standard as std\nfrom lib.osc import " + type + "\nInst.sigstream(std.event.keys." +
        (mono ? "mono" : "poly") + "()) as key:\n    osc = " + type + ".make()\n    raw = osc(pitch: key.pitch, level: " +
        level + ")\n    return raw * key.gate * key.velocity\n";
}
std::unique_ptr<PreparedProgram> prepare(std::uint64_t revision, bool saw = false,
                                         std::string level = "-6db", bool mono = false) {
    auto text = source(saw, level, mono);
    auto result = compile(text, "session-test.flows");
    check(result.program.has_value(), "session test source did not compile");
    return std::make_unique<PreparedProgram>(std::move(*result.program), text, revision, 48000);
}
ValueId value(const Program& program, std::string_view name) {
    for (const auto& entry : program.named_values) if (entry.name == name) return entry.value;
    throw std::runtime_error("missing named test value");
}
ValueId levelParameter(const Program& program) {
    for (const auto& parameter : program.parameters) if (parameter.name == "level") return parameter.value;
    throw std::runtime_error("missing level parameter");
}
Event on(std::uint64_t sample, std::uint64_t id, double pitch = 69) { return {sample, EventType::note_on, id, pitch, 0.2}; }
Event off(std::uint64_t sample, std::uint64_t id) { return {sample, EventType::note_off, id, 69, 0}; }
void process(RealtimeSession& session, std::span<const Event> events, std::span<float> output,
             std::span<const ParameterUpdate> updates = {}) {
    watchHeap = true; session.process(events, output, updates); watchHeap = false;
    check(allocations == 0 && deletions == 0, "audio session allocated/deleted C++ heap memory");
}
void drain(RealtimeSession& session) { while (session.frames().readSlot()) session.frames().commitRead(); }
void processStereo(RealtimeSession& session,std::span<const Event> events,std::span<float> left,std::span<float> right) {
    watchHeap=true;session.processStereo(events,left,right);watchHeap=false;
    check(allocations==0&&deletions==0,"Stereo audio session allocated/deleted C++ heap memory");
}

void source_updates_and_observation() {
    RealtimeSession session;
    auto initial = prepare(1);
    initial->probes = {1, {value(initial->program, "raw")}, {true}, 1, 0};
    const auto parameter = levelParameter(initial->program);
    Engine reference(initial->program, {48000, 1024, 32, .003});
    session.submit(std::move(initial));
    std::array<float, 64> actual{}, expected{};
    const std::array start{on(0, 1)};
    process(session, start, actual); reference.process(start, expected);
    check(actual == expected && session.status.revision == 1 && session.status.voices == 1, "initial install differs from core");
    const auto firstGeneration = session.status.generation.load();
    const auto* frame = session.frames().readSlot();
    check(frame && frame->voiceGeneration == 1 && frame->voice == 0, "selected voice capture identity missing");
    const auto observation = frame->observationEpoch;
    session.frames().commitRead();

    auto edited = prepare(2, false, "-12db");
    edited->probes = {2, {value(edited->program, "raw")}, {true}, 1, 0};
    session.submit(std::move(edited));
    watchHeap = true; session.beginBlock(); session.beginBlock(); watchHeap = false;
    check(session.targetRevision() == 2 && session.status.revision == 1, "beginBlock must expose target but not ACK source early");
    process(session, {}, actual);
    const std::array changed{ParameterUpdate{64, parameter, -12}};
    reference.process({}, expected, nullptr, changed);
    check(actual == expected && session.status.generation == firstGeneration && session.status.revision == 2,
          "live source edit reset voice/phase or failed to ACK");
    frame = session.frames().readSlot();
    check(frame && frame->parameterEpoch == 1 && frame->transition, "live edit capture version is wrong");
    check(frame->observationEpoch == observation, "unchanged selection falsely changed observation epoch");
    session.frames().commitRead(); session.reap();

    check(!compile("this is not Flows", "bad.flows").program, "invalid draft unexpectedly compiled");
    process(session, {}, actual); reference.process({}, expected);
    check(actual == expected && session.status.revision == 2, "invalid unsubmitted draft changed playing program");
    drain(session);
    ProbeSelection selection{2, {value(prepare(3)->program, "key.pitch")}, {false}, 1, 0};
    check(session.setProbes(selection), "valid observation selection rejected");
    process(session, {}, actual); reference.process({}, expected);
    check(actual == expected, "probe change reset sound");
    frame = session.frames().readSlot();
    check(frame && frame->observationEpoch == observation + 1 && frame->samples[0][0] == 69,
          "observation-only change did not update version/data");
    drain(session);
    session.submit(prepare(1, true));
    process(session, {}, actual); reference.process({}, expected);
    check(actual == expected && session.status.revision == 2, "stale compile completion replaced newer source");
    session.reap();
}

void structural_crossfade_and_release() {
    RealtimeSession session;
    auto first = prepare(1);
    Engine oldReference(first->program, {48000, 1024, 32, .003});
    session.submit(std::move(first));
    std::array<float, 256> actual{}, oldAudio{}, newAudio{};
    const std::array start{on(0, 1)};
    process(session, start, actual); oldReference.process(start, oldAudio);
    auto second = prepare(2, true);
    Engine newReference(second->program, {48000, 1024, 32, .003});
    newReference.restore_held_notes(start);
    session.submit(std::move(second));
    for (std::size_t block = 0; block < 4; ++block) {
        process(session, {}, actual); oldReference.process({}, oldAudio); newReference.process({}, newAudio);
        for (std::size_t i = 0; i < actual.size(); ++i) {
            const auto elapsed = block * actual.size() + i;
            const float oldWeight = elapsed < 960 ? float(960 - elapsed) / 960.0f : 0.0f;
            const auto expected = newAudio[i] * (1 - oldWeight) + oldAudio[i] * oldWeight;
            check(actual[i] == expected, "structural replacement differs from exact 20ms crossfade/replay");
        }
    }
    check(session.status.generation == 2 && session.status.voices == 1, "held note was not restored into new program");
    const std::array release{off(1280, 1)};
    process(session, release, actual);
    check(std::all_of(actual.begin()+144, actual.end(), [](float x) { return x == 0; }), "held replay duplicated or lost note identity after boundary tail");
    check(session.status.errors == 0 && session.status.voices == 0, "release after swap failed");
    session.reap();
}

void rejected_notes_survive_swap_without_revival() {
    RealtimeSession session;
    session.submit(prepare(1));
    std::array<Event, 33> notes{};
    for (std::size_t i = 0; i < notes.size(); ++i) notes[i] = on(0, i + 1, 60 + i % 12);
    std::array<float, 1024> audio{};
    process(session, notes, audio);
    check(session.status.voices == 32, "voice cap not enforced");
    const std::array releaseAccepted{off(1024, 1)};
    process(session, releaseAccepted, audio);
    session.submit(prepare(2, true));
    process(session, {}, audio);
    check(session.status.voices == 31, "previously rejected note was promoted by a swap");
    const std::array releaseRejected{off(3072, 33)};
    process(session, releaseRejected, audio);
    check(session.status.errors == 0 && session.status.voices == 31,
          "old rejected note-off caused a panic in the new program");
    session.reap();
}

void mono_large_held_replay() {
    RealtimeSession session;
    session.submit(prepare(1, false, "-20db", true));
    std::array<Event, 256> notes{};
    std::array<float, 64> audio{};
    for (std::uint64_t group = 0; group < 4; ++group) {
        for (std::size_t i = 0; i < notes.size(); ++i) notes[i] = on(group * 64, group * 256 + i + 1, 60 + i % 12);
        process(session, notes, audio);
    }
    session.submit(prepare(2, true, "-20db", true));
    process(session, {}, audio);
    check(session.status.voices == 1 && session.status.errors == 0, "mono 1024-note replay exceeded ordinary block event capacity");
    for (std::uint64_t group = 0; group < 4; ++group) {
        for (std::size_t i = 0; i < notes.size(); ++i) notes[i] = off(320 + group * 64, group * 256 + i + 1);
        process(session, notes, audio);
    }
    check(session.status.voices == 0 && session.status.errors == 0, "mono replay did not preserve all held identities");
}

void capture_capacity_identity_and_panic() {
    RealtimeSession session;
    auto program = prepare(1);
    program->probes = {1, {value(program->program, "raw")}, {true}, 1, 0};
    session.submit(std::move(program));
    std::array<float, 64> audio{};
    const std::array reused{on(0, 1), off(20, 1), on(30, 2)};
    process(session, reused, audio);
    auto* frame = session.frames().readSlot();
    check(frame && frame->transition && frame->voiceGeneration == 2, "slot generation reuse was spliced into one FFT frame");
    drain(session);
    for (int i = 0; i < 130; ++i) process(session, {}, audio);
    check(session.status.drops == 2 && session.status.errors == 0, "capture queue overflow blocked audio or lost drop accounting");
    drain(session);
    process(session, {}, audio);
    frame = session.frames().readSlot();
    check(frame && frame->sequence == 131 && frame->start == 131 * 64, "capture gap lost sample/sequence identity");
    drain(session);
    ProbeSelection invalid; invalid.count = 9;
    check(!session.setProbes(invalid), "out-of-range probe count accepted");
    ProbeSelection stopped; stopped.revision = 1;
    check(session.setProbes(stopped), "capture stop rejected");
    process(session, {}, audio);
    const auto drops = session.status.drops.load();
    for (int i = 0; i < 130; ++i) process(session, {}, audio);
    check(session.status.drops == drops && !session.frames().readSlot(), "disabled probes still produced frames/drops");
    session.requestPanic(); process(session, {}, audio);
    check(session.status.voices == 0 && std::all_of(audio.begin(), audio.end(), [](float x) { return x == 0; }), "panic failed to silence notes");
}

void zero_blocks_latest_pending_and_parameter_ramps() {
    RealtimeSession session;
    auto program = prepare(1);
    const auto parameter = levelParameter(program->program);
    Engine reference(program->program, {48000, 1024, 32, .003});
    session.submit(std::move(program));
    process(session, {}, {});
    check(session.status.revision == 0, "zero-frame callback prematurely ACKed source");
    std::array<float, 64> actual{}, expected{};
    const std::array start{on(0, 1)};
    process(session, start, actual); reference.process(start, expected);
    const std::array ramp{ParameterUpdate{64, parameter, -24, 100, false, -9.0}};
    process(session, {}, actual, ramp); reference.process({}, expected, nullptr, ramp);
    check(actual == expected, "session lost explicit automation ramp fields");
    process(session, {}, actual); reference.process({}, expected);
    check(actual == expected, "automation ramp did not continue across blocks");
    session.submit(prepare(2, false, "-10db"));
    session.submit(prepare(3, false, "-12db"));
    watchHeap = true; session.process({}, actual, {}, false); watchHeap = false;
    check(session.status.revision == 1, "later sub-block installed a different program under pre-mapped automation");
    process(session, {}, {});
    check(session.status.revision == 1, "zero-frame live edit was not deferred");
    process(session, {}, actual);
    check(session.status.revision == 3 && session.status.generation == 1, "latest pending live source lost or reset state");
    session.reap();
    const std::array invalidTime{ParameterUpdate{0, parameter, -6}};
    process(session, {}, actual, invalidTime);
    check(session.status.errors == 1 && session.status.samples == 384 && session.status.voices == 0,
          "invalid automation timestamp did not panic/advance visible clock");
}

void runtime_overlay_and_initial_held_notes() {
    RealtimeSession session;
    std::array<float, 256> audio{}, expected{};
    const std::array waiting{on(0, 1)};
    process(session, waiting, audio);
    check(session.status.voices == 0 && session.status.errors == 0, "keys before preparation did not remain pending");
    auto first = prepare(1);
    auto parameter = levelParameter(first->program);
    first->initialParameters.push_back({0, parameter, -24, 0, false});
    Engine reference(first->program, {48000, 1024, 32, .003});
    const std::array overlay{ParameterUpdate{0, parameter, -24, 0, false}};
    reference.restore_held_notes(waiting, nullptr, overlay);
    session.submit(std::move(first));
    process(session, {}, audio); reference.process({}, expected);
    check(audio == expected && session.status.voices == 1, "initial overlay was not applied before pending held replay");
    auto second = prepare(2, false, "-3db");
    second->initialParameters.push_back({0, parameter, -24, 0, false});
    session.submit(std::move(second));
    process(session, {}, audio); reference.process({}, expected);
    check(audio == expected && session.status.generation == 1, "runtime overlay did not win over live source edit");
    session.reap();

    auto third = prepare(3, true, "-12db");
    parameter = levelParameter(third->program);
    third->initialParameters.push_back({0, parameter, -30, 0, false});
    Engine replacement(third->program, {48000, 1024, 32, .003});
    const std::array newOverlay{ParameterUpdate{0, parameter, -30, 0, false}};
    replacement.restore_held_notes(waiting, nullptr, newOverlay);
    session.submit(std::move(third));
    for (int i = 0; i < 4; ++i) { process(session, {}, audio); replacement.process({}, expected); }
    check(audio.back() == expected.back() && session.status.generation == 2 && session.status.voices == 1,
          "structural overlay/replay failed");
    session.reap();
}

void probe_audition_and_main_restore() {
    const std::string text = R"(import standard as std
from lib.osc import ClassicSine
Inst.sigstream(std.event.keys.poly()) as key:
    mid_oscillator = ClassicSine.make()
    sub_oscillator = ClassicSine.make()
    mid = mid_oscillator(pitch: key.pitch, octave: 1, level: -12db)
    sub = sub_oscillator(pitch: key.pitch, level: -6db)
    return (mid + sub) * key.gate * key.velocity
)";
    auto compiled = compile(text, "audition.flows");
    check(compiled.program.has_value(), "audition source did not compile");
    RealtimeSession session;
    auto prepared = std::make_unique<PreparedProgram>(*compiled.program, text, 1, 48000);
    const auto mid = value(prepared->program, "mid"), sub = value(prepared->program, "sub");
    const auto pitch = value(prepared->program, "key.pitch");
    ProbeSelection selection{1, {mid}, {true}, 1, -1};
    prepared->probes = selection;
    Engine reference(prepared->program, {48000, 1024, 32, .003});
    session.submit(std::move(prepared));
    std::array<float, 512> actual{}, expected{};
    const std::array notes{on(0, 1)};
    process(session, notes, std::span(actual).first(64));
    reference.process(notes, std::span(expected).first(64));
    drain(session);
    selection.audition = mid;
    check(session.setProbes(selection), "audio audition selection rejected");
    process(session, {}, actual); reference.process({}, expected);
    const auto* frame = session.frames().readSlot();
    check(frame && frame->channels == 1, "audition removed ordinary capture");
    for (std::size_t i = 0; i < actual.size(); ++i) {
        const auto mix = static_cast<float>(std::min(1.0, double(i) / 480.0));
        check(std::abs(actual[i] - (expected[i] * (1 - mix) + frame->samples[0][i] * mix)) < 1e-6,
              "audition differs from the exact 10ms main-to-tap fade");
    }
    check(actual.back() == frame->samples[0][511] && std::abs(actual.back() - expected.back()) > 0.001f,
          "listening to mid still plays the main instrument");
    drain(session);
    selection.audition = invalid_value;
    check(session.setProbes(selection), "audition off rejected");
    process(session, {}, actual); reference.process({}, expected);
    frame = session.frames().readSlot();
    for (std::size_t i = 0; i < actual.size(); ++i) {
        const auto mix = static_cast<float>(std::max(0.0, 1.0 - double(i) / 480.0));
        check(std::abs(actual[i] - (expected[i] * (1 - mix) + frame->samples[0][i] * mix)) < 1e-6,
              "audition off differs from the exact 10ms tap-to-main fade");
    }
    check(actual.back() == expected.back(), "audition off failed to restore main output/phase");
    drain(session);
    // Listening works without visual captures, and control streams cannot reach the speakers.
    selection.count = 0; selection.audition = mid;
    session.setProbes(selection); process(session, {}, actual); reference.process({}, expected);
    check(!session.frames().readSlot() && session.status.drops == 0, "listen-only generated capture frames");
    selection.audition = pitch;
    session.setProbes(selection); process(session, {}, actual); reference.process({}, expected);
    check(actual.back() == expected.back(), "control audition was accepted");
    selection.count = 1; selection.values[0] = mid; selection.audition = mid;
    session.setProbes(selection); process(session, {}, actual); reference.process({}, expected); drain(session);
    selection.values[0] = sub; // Same listen request with a different observation must restore main.
    session.setProbes(selection); process(session, {}, actual); reference.process({}, expected); drain(session);
    check(actual.back() == expected.back(), "switching observed probe did not restore main");

    selection.values[0] = mid; selection.audition = mid;
    session.setProbes(selection); process(session, {}, actual); reference.process({}, expected); drain(session);
    // Deleting the auditioned signal must keep the outgoing tap for the start of the fade.
    const auto nextSample = session.status.samples.load();
    const auto expectedOldTap = static_cast<float>(std::sin(2.0 * 3.14159265358979323846 *
        880.0 * double(nextSample) / 48000.0) * std::pow(10.0, -12.0 / 20.0));
    session.submit(prepare(2, true));
    process(session, {}, actual);
    check(std::abs(actual[0] - expectedOldTap) < 1e-6, "structural swap used stale ValueId or dropped outgoing audition abruptly");
    check(std::all_of(actual.begin(), actual.end(), [](float x) { return std::isfinite(x); }) && session.status.errors == 0,
          "structural swap while listening failed");
}

void live_class_input_probe_preserves_engine() {
    const std::string before = "import standard as std\nfrom lib.osc import ClassicSine\nclass Tap:\n    def signal(input: scalar):\n        return input * 0.5\nInst.sigstream(std.event.keys.poly()) as key:\n    osc = ClassicSine.make()\n    tap = Tap.make()\n    raw = osc(pitch: key.pitch, level: 0.5)\n    shaped = raw * 0.8\n    processed = tap(input: shaped * 2)\n    return processed * key.gate\n";
    auto after=before; after.insert(after.find("        return"),"        observed = probe(input)\n");
    auto original=compile(before,"class-probe.flows"), observed=compile(after,"class-probe.flows");
    check(original.program && observed.program,"Class input probe fixture did not compile");
    check(dsp_structure_key(*original.program)==dsp_structure_key(*observed.program),"Adding a class input probe changed DSP identity");
    auto selection=makeProbeSelection(*observed.program,2);
    check(static_cast<bool>(selection)&&selection.selection.count==1,"Anonymous class argument cannot be captured");
    const auto root=selection.selection.values[0];
    check(std::none_of(observed.program->named_values.begin(),observed.program->named_values.end(),[&](const auto& named){return named.value==root;}),"Class probe regression requires an anonymous DSP root");
    RealtimeSession session; Engine reference(*original.program,{48000,1024,32,.003});
    session.submit(std::make_unique<PreparedProgram>(*original.program,before,1,48000));
    std::array<float,512> actual{}, expected{}; const std::array notes{on(0,1)};
    process(session,notes,actual); reference.process(notes,expected); check(actual==expected,"Initial class output differs");
    const auto generation=session.status.generation.load();
    auto next=std::make_unique<PreparedProgram>(*observed.program,after,2,48000);next->probes=selection.selection;
    session.submit(std::move(next)); process(session,{},actual); reference.process({},expected);
    check(actual==expected&&session.status.generation==generation&&session.status.voices==1,"Live probe addition restarted held oscillator/state");
    const auto* frame=session.frames().readSlot();check(frame&&frame->channels==1,"Class input probe did not publish samples");
    for(std::size_t i=0;i<actual.size();++i)check(std::abs(frame->samples[0][i]-2*actual[i])<1e-6,"Probe captured nearest named ancestor instead of exact class input");
    drain(session);session.reap();
    selection.selection.audition=root;check(session.setProbes(selection.selection),"Class input audition rejected");
    process(session,{},actual);reference.process({},expected);frame=session.frames().readSlot();
    check(frame&&std::abs(actual.back()-frame->samples[0][511])<1e-6&&std::abs(actual.back()-2*expected.back())<1e-6,"Class input audition did not play its exact source");
    drain(session); selection.selection.audition=invalid_value;session.setProbes(selection.selection);
    process(session,{},actual);reference.process({},expected);
    check(actual.back()==expected.back()&&session.status.generation==generation,"Stopping class probe audition failed to restore phase-continuous output");
}

void stereo_projection_audition_and_crossfade() {
    const std::string text="import standard as std\nfrom lib.osc import ClassicSine\nInst.sigstream(std.event.keys.poly()) as key:\n    osc = ClassicSine.make()\n    raw = osc(pitch: key.pitch, level: 0.5)\n    image = stereo(raw, raw * 0.25)\n    p = probe(image)\n    return image * key.gate * 0.5\n";
    auto compiled=compile(text,"stereo-session.flows");check(compiled.program.has_value(),"Stereo session source rejected");
    const auto& program=*compiled.program;const std::array extra{value(program,"raw"),value(program,"key.gate")};
    auto selection=makeProbeSelection(program,1,extra);check(static_cast<bool>(selection),"Stereo probe selection rejected");
    RealtimeSession session;Engine reference(program,{48000,1024,32,.003});
    auto initial=std::make_unique<PreparedProgram>(program,text,1,48000);initial->probes=selection.selection;session.submit(std::move(initial));
    std::array<float,512> left{},right{},expectedLeft{},expectedRight{};const std::array notes{on(0,1)};
    processStereo(session,notes,left,right);reference.process_stereo(notes,expectedLeft,expectedRight);drain(session);
    check(left==expectedLeft&&right==expectedRight&&left!=right,"Native session flattened stereo output");
    for(const auto projection:{ProbeProjection::left,ProbeProjection::right,ProbeProjection::mid,ProbeProjection::side}) {
        selection.selection.projection=projection;check(session.setProbes(selection.selection),"Stereo projection rejected");
        processStereo(session,{},left,right);reference.process_stereo({},expectedLeft,expectedRight);
        check(left==expectedLeft&&right==expectedRight,"Changing observation projection changed playback");
        const auto* frame=session.frames().readSlot();check(frame&&frame->projection==projection&&frame->channels==3,"Actual capture projection identity missing");
        for(std::size_t i=0;i<left.size();++i) {
            const auto expected=projection==ProbeProjection::left ? 2*left[i] : projection==ProbeProjection::right ? 2*right[i] : projection==ProbeProjection::side ? left[i]-right[i] : left[i]+right[i];
            check(std::abs(frame->samples[0][i]-expected)<1e-6,"Stereo capture projection is incorrect");
            if(projection==ProbeProjection::side)check(frame->samples[1][i]==0,"Mono audio has a nonzero Side projection");
            check(frame->samples[2][i]==1,"Audio projection changed a mono control signal");
        }
        drain(session);
    }
    selection.selection.audition=value(program,"image");session.setProbes(selection.selection);
    processStereo(session,{},left,right);reference.process_stereo({},expectedLeft,expectedRight);drain(session);
    check(std::abs(left.back()-2*expectedLeft.back())<1e-6&&std::abs(right.back()-2*expectedRight.back())<1e-6&&left!=right,"Audition flattened stereo or applied the Side display projection");
    auto nextText=text;nextText.replace(nextText.find("stereo(raw, raw * 0.25)"),std::string("stereo(raw, raw * 0.25)").size(),"stereo(raw * 0.25, raw)");
    auto nextProgram=compile(nextText,"stereo-session.flows");check(nextProgram.program.has_value(),"Replacement stereo source rejected");
    // Listen captures the exact DSP tap, before playback boundary protection.
    Engine replacement(*nextProgram.program,{48000,1024,32,0});replacement.restore_held_notes(notes);
    auto next=std::make_unique<PreparedProgram>(*nextProgram.program,nextText,2,48000);
    const std::array nextExtra{value(next->program,"raw"),value(next->program,"key.gate")};
    next->probes=makeProbeSelection(next->program,2,nextExtra,-1,{},true,ProbeProjection::side).selection;next->probes.audition=value(next->program,"image");session.submit(std::move(next));
    std::array<float,512> newLeft{},newRight{};
    for(std::size_t block=0;block<2;++block) {
        processStereo(session,{},left,right);reference.process_stereo({},expectedLeft,expectedRight);replacement.process_stereo({},newLeft,newRight);
        for(std::size_t i=0;i<left.size();++i) {
            const auto elapsed=block*left.size()+i;const float old=elapsed<960 ? float(960-elapsed)/960.0f : 0;
            check(std::abs(left[i]-2*(expectedLeft[i]*old+newLeft[i]*(1-old)))<1e-6&&std::abs(right[i]-2*(expectedRight[i]*old+newRight[i]*(1-old)))<1e-6,"Stereo audition crossfade mixed channels or lost the outgoing tap");
        }
        drain(session);
    }
    check(session.status.voices==1&&session.status.generation==2,"Stereo crossfade lost the held note");session.reap();
}
void transport_boundaries_preserve_state() {
    auto prepared=prepare(1);prepared->probes={1,{value(prepared->program,"raw")},{true},1,-1};
    RealtimeSession session;session.submit(std::move(prepared));session.setTransport({120,4,true,true});
    std::array<float,128> left{},right{};const std::array notes{on(0,1)};processStereo(session,notes,left,right);
    auto epoch=session.frames().readSlot()->observationEpoch;drain(session);
    session.setTransport({120,4+128.0*120/(60*48000),true,true});processStereo(session,{},left,right);
    check(session.frames().readSlot()->observationEpoch==epoch,"Continuous host PPQ reset the analysis window");drain(session);
    session.setTransport({240,4+256.0*120/(60*48000),true,true});processStereo(session,{},left,right);
    check(session.frames().readSlot()->observationEpoch>epoch&&session.status.generation==1&&session.status.voices==1,"Tempo change did not split capture or reset the oscillator");drain(session);
}
void performance_capture_follows_voice_lifetime() {
    {
        auto prepared=prepare(1);const std::array extra{value(prepared->program,"raw")};
        prepared->probes=makeProbeSelection(prepared->program,1,extra,-1,{},false).selection;
        Analyzer analyzer;analyzer.configure(prepared->program,1,{},false);
        RealtimeSession session;session.setContinuousObservation(false);session.submit(std::move(prepared));
        std::array<float,512> output{};process(session,{},output);drain(session);
        const std::array chord{on(529,1),on(640,2,72)};process(session,chord,output);
        const auto* f=session.frames().readSlot();
        check(f&&f->keyEpoch==2&&f->keyStart==529,"Later poly onset overwrote the first attack in a callback.");
        analyzer.consume(*f);check(analyzer.snapshot().start==529,"Mixed capture clipped the earlier poly attack.");
        drain(session);
    }
    for(const bool mono : {false,true}) {
        const std::string text="import standard as std\nfrom lib.osc import ClassicSine\nfrom lib.env import ClassicADSR\n"
            "Inst.sigstream(std.event.keys." + std::string(mono?"mono":"poly") + "()) as key:\n"
            "    osc = ClassicSine.make()\n    env = ClassicADSR.make()\n"
            "    raw = osc(pitch: key.pitch, level: -6db)\n"
            "    amp = env(gate: key.gate, trigger: key.trigger, A: 10ms, D: 50ms, S: 0.6, R: 250ms)\n"
            "    shaped = raw * amp\n    heard = probe(shaped)\n    return shaped\n";
        auto compiled=compile(text,"performance-capture.flows");check(compiled.program.has_value(),"Performance test did not compile.");
        auto prepared=std::make_unique<PreparedProgram>(*compiled.program,text,1,48000);
        const std::vector<std::string> names{"heard"};const std::array extra{value(*compiled.program,"raw"),value(*compiled.program,"key.pitch"),value(*compiled.program,"key.gate")};
        prepared->probes=makeProbeSelection(*compiled.program,1,extra,-1,names).selection;
        Analyzer analyzer;analyzer.configure(*compiled.program,1,names);
        RealtimeSession session;session.setContinuousObservation(false);session.submit(std::move(prepared));
        std::array<float,512> output{};
        auto block=[&](std::span<const Event> events=std::span<const Event>{}) {
            // The DSP clock continues for synced LFOs, even without sequencer playback.
            session.setTransport({120,double(session.status.samples)/24000,true,true});
            process(session,events,output);
            const auto* f=session.frames().readSlot();check(f!=nullptr,"Missing performance capture frame.");
            if(!f->performanceActive)for(std::size_t c=0;c<f->channels;++c)
                for(std::size_t i=0;i<f->count;++i)check(f->samples[c][i]==0,"An inactive mono kernel leaked into the display.");
            analyzer.consume(*f);session.frames().commitRead();
            const auto current=analyzer.snapshot();
            for(const auto* collection:{&current.signals,&current.results})for(const auto& item:*collection)
                check(!item.beatAvailable&&item.beatMin.empty()&&item.beatMax.empty()&&item.beatAge.empty(),"Manual key or release inherited a transport scan.");
        };
        for(int i=0;i<8;++i)block();
        check(analyzer.snapshot().captureIdle,"An unplayed instrument woke the display.");
        const std::array press{on(session.status.samples+17,1)};block(press);
        for(int i=0;i<192;++i)block();
        const auto sustain=analyzer.snapshot();
        check(sustain.ready&&!sustain.captureHeld&&!sustain.captureIdle&&session.status.voices==1,"Sustained key observation expired.");
        auto power=[](const AnalysisSnapshot& snapshot){double sum=0;for(float value:snapshot.results.front().spectrum)sum+=value*value;return sum;};
        const auto sustainedPower=power(sustain);
        const std::array release{off(session.status.samples,1)};block(release);
        for(int i=0;i<8;++i)block();
        const auto releasing=analyzer.snapshot();
        check(session.status.voices==1&&releasing.end>sustain.end&&!releasing.captureIdle,"Note off cut off ADSR release capture.");
        const auto control=[&](std::string_view name)->const AnalysisView&{for(const auto& item:releasing.signals)if(item.name==name)return item;throw std::runtime_error("Missing release control");};
        check(control("key.gate").peak==0&&control("key.pitch").peak==69,"Note off must lower gate while preserving pitch through release.");
        check(power(releasing)<sustainedPower*.9&&power(releasing)>0,"Observed output did not follow the ADSR release.");
        for(int i=0;i<24;++i)block();
        check(session.status.voices==0&&!analyzer.snapshot().captureIdle,"Completion must drain the spectrum before clearing it.");
        for(int i=0;i<160;++i)block();
        check(analyzer.snapshot().captureIdle&&analyzer.snapshot().signals.empty(),"Finished performance did not settle to idle.");
        const std::array next{on(session.status.samples+31,2)};block(next);
        for(int i=0;i<8;++i)block();
        check(analyzer.snapshot().ready&&analyzer.snapshot().keyEpoch==2&&!analyzer.snapshot().captureHeld,"Next key did not restart a live observation.");
    }
}
void unsafe_probe_preserves_main_instrument() {
    auto prepared = prepare(1);
    const auto raw = value(prepared->program, "raw");
    // A deliberately overflowing unused branch is observable but cannot poison the valid main signal.
    auto& program = prepared->program;
    const auto huge = static_cast<ValueId>(program.nodes.size());
    Node literal; literal.op = Op::parameter; literal.initial_value = 1e30; program.nodes.push_back(literal);
    Node multiply; multiply.op = Op::multiply; multiply.inputs = {raw, huge};
    const auto first = static_cast<ValueId>(program.nodes.size()); program.nodes.push_back(multiply);
    multiply.inputs = {first, huge};
    const auto overflow = static_cast<ValueId>(program.nodes.size()); program.nodes.push_back(multiply);
    program.named_values.push_back({"unsafe", overflow, {}});
    auto unsafe = std::make_unique<PreparedProgram>(program, prepared->source, 1, 48000);
    unsafe->probes.audition = overflow;
    Engine reference(program, {48000, 1024, 32, .003});
    RealtimeSession session; session.submit(std::move(unsafe));
    std::array<float, 512> actual{}, expected{};
    const std::array notes{on(0, 1)};
    process(session, notes, actual); reference.process(notes, expected);
    check(actual == expected && session.status.errors == 1 && session.status.voices == 1,
          "nonfinite audition poisoned output or reset valid instrument state");
    process(session, {}, actual); reference.process({}, expected);
    check(actual == expected && session.status.errors == 1, "unsafe audition did not remain disabled");
}

void audition_voice_selection_and_panic() {
    RealtimeSession session;
    auto initial = prepare(1);
    const auto raw = value(initial->program, "raw");
    ProbeSelection selection{1, {raw}, {true}, 1, 1, raw};
    initial->probes = selection;
    session.submit(std::move(initial));
    std::array<float, 512> audio{};
    const std::array notes{on(0, 1, 60), on(0, 2, 72)};
    process(session, notes, audio);
    const auto* frame = session.frames().readSlot();
    check(frame && frame->voice == 1 && audio.back() == frame->samples[0][511], "audition did not isolate the selected voice");
    drain(session);
    session.submit(prepare(2, false, "-6db", true));
    process(session, {}, audio); process(session, {}, audio); session.reap();
    selection.revision = 2; selection.voice = 0;
    session.setProbes(selection); process(session, {}, audio); drain(session);
    check(std::any_of(audio.begin() + 480, audio.end(), [](float x) { return x != 0; }), "mono raw audition did not start");
    session.requestPanic(); process(session, {}, audio);
    check(std::all_of(audio.begin(), audio.end(), [](float x) { return x == 0; }) && session.status.voices == 0,
          "panic left an ungated intermediate oscillator audible");
}

void retained_engine_keeps_runtime_ramps_and_late_panic() {
    const auto make = [](std::uint64_t revision, const std::string& level, const std::string& trim, bool comment = false) {
        auto text = source(false, level);
        text.insert(text.find("    return"), "    trim = " + trim + "\n");
        text.replace(text.find("return raw"), std::string("return raw").size(), "return trim * raw");
        if (comment) text.insert(0, "# observation-only comment\n");
        auto compiled = compile(text, "runtime-ramp.flows");
        check(compiled.program.has_value(), "runtime ramp test source did not compile");
        return std::make_unique<PreparedProgram>(*compiled.program, text, revision, 48000);
    };
    auto initial = make(1, "-6db", "0.5");
    const auto level = levelParameter(initial->program), trim = value(initial->program, "trim");
    Engine reference(initial->program, {48000, 1024, 32, .003});
    RealtimeSession session; session.submit(std::move(initial));
    std::array<float, 128> actual{}, expected{};
    const std::array notes{on(0, 1)};
    process(session, notes, actual); reference.process(notes, expected);
    const std::array ramp{ParameterUpdate{128, level, -24, 768, false, -6.0}};
    process(session, {}, actual, ramp); reference.process({}, expected, nullptr, ramp);
    auto comment = make(2, "-6db", "0.5", true);
    comment->initialParameters.push_back({0, level, -24, 0, false});
    session.submit(std::move(comment));
    process(session, {}, actual); reference.process({}, expected);
    check(actual == expected, "comment compile replaced an in-flight runtime ramp with its target");
    session.reap();
    auto edited = make(3, "-3db", "0.25", true);
    edited->initialParameters.push_back({0, level, -24, 0, false});
    session.submit(std::move(edited));
    const std::array authored{ParameterUpdate{384, trim, 0.25}};
    process(session, {}, actual); reference.process({}, expected, nullptr, authored);
    check(actual == expected && session.status.generation == 1,
          "source edit failed to preserve host ramp while applying unrelated authored value");
    for (int i = 0; i < 5; ++i) {
        process(session, {}, actual); reference.process({}, expected);
        check(actual == expected, "retained runtime ramp lost its remaining duration or endpoint");
    }
    session.reap();
    watchHeap = true; session.beginBlock(); session.requestPanic(); watchHeap = false;
    process(session, {}, actual);
    check(session.status.voices == 0 && std::all_of(actual.begin(), actual.end(), [](float x) { return x == 0; }),
          "panic requested after beginBlock was delayed until the next callback");
}
} // namespace

int main() {
    try {
        source_updates_and_observation();
        structural_crossfade_and_release();
        rejected_notes_survive_swap_without_revival();
        mono_large_held_replay();
        capture_capacity_identity_and_panic();
        zero_blocks_latest_pending_and_parameter_ramps();
        runtime_overlay_and_initial_held_notes();
        probe_audition_and_main_restore();
        live_class_input_probe_preserves_engine();
        stereo_projection_audition_and_crossfade();
        transport_boundaries_preserve_state();
        performance_capture_follows_voice_lifetime();
        unsafe_probe_preserves_main_instrument();
        audition_voice_selection_and_panic();
        retained_engine_keeps_runtime_ramps_and_late_panic();
        std::cout << "Session: 15 groups passed (stereo projections/audition/crossfade, transport boundaries, source/parameter/observation, runtime ramps, held IDs, live class probes, panic, RT heap).\n";
        return 0;
    } catch (const std::exception& error) {
        watchHeap = false;
        std::cerr << "Session test failure: " << error.what() << '\n'; return 1;
    }
}
