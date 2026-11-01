#include "host/app/FlowsProcessor.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <new>
#include <filesystem>

namespace { thread_local bool watchSequencerHeap=false;thread_local std::size_t sequencerHeapTouches=0; }
void* operator new(std::size_t size){if(watchSequencerHeap)++sequencerHeapTouches;if(auto* value=std::malloc(size?size:1))return value;throw std::bad_alloc();}
void* operator new[](std::size_t size){return ::operator new(size);}
void operator delete(void* value) noexcept {if(watchSequencerHeap&&value)++sequencerHeapTouches;std::free(value);}
void operator delete[](void* value) noexcept {::operator delete(value);}
void operator delete(void* value,std::size_t) noexcept {::operator delete(value);}
void operator delete[](void* value,std::size_t) noexcept {::operator delete(value);}

namespace {
using namespace juce;
using namespace flows::app;
using Clock = std::chrono::steady_clock;
constexpr int blockSize = 128;
constexpr double sampleRate = 48000;

void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
template<class Work> void rejects(Work work, const std::string& message) {
    bool rejected = false;
    try { work(); } catch (const std::exception&) { rejected = true; }
    check(rejected, message);
}

// These tests always own a new temporary workspace, even when the caller has
// FLOWS_WORKSPACE set to a real project. No existing instrument is read or saved.
struct TestWorkspace {
    const char* previousPointer = std::getenv("FLOWS_WORKSPACE");
    bool hadPrevious = previousPointer != nullptr;
    String previous = hadPrevious ? String(previousPointer) : String();
    File directory = File::getSpecialLocation(File::tempDirectory)
        .getChildFile("flows-sequencer-host-" + Uuid().toString());
    TestWorkspace() {
        check(directory.createDirectory().wasOk(), "Could not create temporary test workspace.");
        setenv("FLOWS_AI_CONFIG_DIR", directory.getChildFile("test-global-ai").getFullPathName().toRawUTF8(), 1);
        check(setenv("FLOWS_WORKSPACE", directory.getFullPathName().toRawUTF8(), 1) == 0,
              "Could not isolate the test workspace.");
    }
    ~TestWorkspace() {
        if (hadPrevious) setenv("FLOWS_WORKSPACE", previous.toRawUTF8(), 1);
        else unsetenv("FLOWS_WORKSPACE");
        directory.deleteRecursively();
    }
};

var document(FlowsProcessor& p) { return get(p.snapshot(), "document"); }
String documentId(FlowsProcessor& p) { return str(document(p), "id"); }
var sequencer(FlowsProcessor& p) { return get(p.snapshot(), "sequencer"); }
bool playing(FlowsProcessor& p) { return static_cast<bool>(get(sequencer(p), "playing")); }

// A gate produces an unambiguous onset and release; tests never send this
// buffer to an audio device. This separates scheduler timing from oscillator
// phase, envelopes and the analyser's asynchronous display cadence.
const String gateSource =
    "import standard as std\n"
    "Inst.sigstream(std.event.keys.poly()) as key:\n"
    "    return key.gate * key.velocity * 0.25\n";

double audio(FlowsProcessor& p, std::vector<float>* capture = nullptr) {
    AudioBuffer<float> buffer(2, blockSize);
    for (int channel = 0; channel < 2; ++channel)
        for (int i = 0; i < blockSize; ++i)
            buffer.setSample(channel, i, std::numeric_limits<float>::quiet_NaN());
    MidiBuffer midi;
    p.processBlock(buffer, midi);
    double peak = 0;
    for (int i = 0; i < blockSize; ++i) {
        for (int channel = 0; channel < 2; ++channel) {
            const auto value = buffer.getSample(channel, i);
            check(std::isfinite(value), "Sequencer audio was not finite on every channel.");
            peak = std::max(peak, std::abs(static_cast<double>(value)));
        }
        if (capture) capture->push_back(buffer.getSample(0, i));
    }
    return peak;
}
template<class Predicate> void until(FlowsProcessor& p, Predicate predicate, const std::string& message) {
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    while (!predicate()) {
        check(Clock::now() < deadline, message);
        audio(p);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}
void ready(FlowsProcessor& p) {
    until(p, [&] {
        const auto state = p.snapshot();
        return str(get(state, "compile"), "status") == "ready"
            && get(get(state, "audio"), "programRevision") == get(get(state, "document"), "revision");
    }, "Instrument did not compile and install.");
    // Drain structural crossfades before onset or silence measurements.
    for (int i = 0; i < 12; ++i) audio(p);
}
void applyGateSource(FlowsProcessor& p) {
    const auto d = document(p);
    p.request("document.apply", object({{"documentId", str(d, "id")},
        {"baseRevision", get(d, "revision")}, {"text", gateSource}}));
    ready(p);
}

var pattern(int rowCount = 1, int note = 60) {
    Array<var> rows;
    for (int row = 0; row < rowCount; ++row) {
        Array<var> steps;
        for (int step = 0; step < 32; ++step)
            steps.add(object({{"active", true}, {"velocity", .8}, {"gate", .25}}));
        rows.add(object({{"note", note + row}, {"mute", false}, {"volume", 0.0}, {"steps", steps}}));
    }
    return object({{"length", 16}, {"division", 16}, {"swing", 0.0},
        {"direction", "forward"}, {"root", 0}, {"scale", "chromatic"},
        {"transpose", 0}, {"rows", rows}});
}
void setPattern(FlowsProcessor& p, const var& value) {
    p.request("sequencer.set", object({{"documentId", documentId(p)},
        {"baseRevision", get(sequencer(p), "revision")}, {"pattern", value}}));
}
void setPlaying(FlowsProcessor& p, bool enabled) {
    p.request("sequencer.play", object({{"documentId", documentId(p)}, {"playing", enabled}}));
    check(playing(p) == enabled, "Play/stop request was not reflected in the snapshot.");
}
void matchesPattern(FlowsProcessor& p, const var& expected, const std::string& context) {
    const auto actual = sequencer(p);
    for (const auto* field : {"length", "division", "swing", "direction", "root", "scale", "transpose", "rows"})
        check(JSON::toString(get(actual, field)) == JSON::toString(get(expected, field)),
              context + ": lost field " + field);
}
void stoppedAndSilent(FlowsProcessor& p, const std::string& context) {
    check(!playing(p), context + ": playback unexpectedly active.");
    for (int i = 0; i < 12; ++i) audio(p);
    check(audio(p) < 1e-7, context + ": audio remained after note release.");
    check(static_cast<int>(get(get(p.snapshot(), "audio"), "voices")) == 0,
          context + ": a sequencer voice remained held.");
}

void validationTests(FlowsProcessor& p) {
    const auto initial = pattern();
    setPattern(p, initial);
    const auto rejectField = [&](const char* field, const var& value) {
        auto bad = pattern(); bad.getDynamicObject()->setProperty(field, value);
        rejects([&] { setPattern(p, bad); }, std::string("Accepted invalid sequencer field: ") + field);
        matchesPattern(p, initial, "Rejected edit mutated the pattern");
    };
    rejectField("length", 17); rejectField("length", "16");
    rejectField("division", 12); rejectField("division", 16.5);
    rejectField("swing", -.01); rejectField("swing", .61);
    rejectField("swing", std::numeric_limits<double>::quiet_NaN());
    rejectField("root", -1); rejectField("root", 12); rejectField("root", true);
    rejectField("transpose", -25); rejectField("transpose", 25);
    rejectField("direction", "backward"); rejectField("scale", "dorian");
    rejectField("rows", Array<var>());
    rejectField("rows", get(pattern(9), "rows"));
    rejects([&] { setPattern(p, var()); }, "Accepted an absent pattern.");
    for (const auto* field : {"note", "mute", "steps"}) {
        auto bad = pattern();
        auto row = (*get(bad, "rows").getArray())[0];
        row.getDynamicObject()->setProperty(field, String("invalid"));
        rejects([&] { setPattern(p, bad); }, std::string("Accepted malformed row field: ") + field);
    }
    for (const auto value : {-1, 128}) {
        auto bad = pattern(); (*get(bad, "rows").getArray())[0].getDynamicObject()->setProperty("note", value);
        rejects([&] { setPattern(p, bad); }, "Accepted an out-of-range MIDI note.");
    }
    for (const auto count : {31, 33}) {
        auto bad = pattern(); auto row = (*get(bad, "rows").getArray())[0];
        auto steps = get(row, "steps");
        if (count == 31) steps.getArray()->removeLast(); else steps.getArray()->add((*steps.getArray())[0]);
        rejects([&] { setPattern(p, bad); }, "Accepted a row without exactly 32 stored steps.");
    }
    const auto rejectStep = [&](const char* field, const var& value) {
        auto bad = pattern();
        auto row = (*get(bad, "rows").getArray())[0];
        (*get(row, "steps").getArray())[0].getDynamicObject()->setProperty(field, value);
        rejects([&] { setPattern(p, bad); }, std::string("Accepted malformed step: ") + field);
    };
    rejectStep("active", 1); rejectStep("velocity", -.01); rejectStep("velocity", 1.01);
    rejectStep("gate", .049); rejectStep("gate", 1.01); rejectStep("gate", "0.5");
    rejects([&] { p.request("sequencer.play", object({{"documentId", documentId(p)}, {"playing", "yes"}})); },
            "Accepted a non-boolean play state.");
    const auto stale = get(sequencer(p), "revision");
    auto changed = pattern(); changed.getDynamicObject()->setProperty("swing", .2); setPattern(p, changed);
    rejects([&] { p.request("sequencer.set", object({{"documentId", documentId(p)},
        {"baseRevision", stale}, {"pattern", initial}})); }, "A stale pattern revision overwrote a newer edit.");
    matchesPattern(p, changed, "Stale revision rejection");
}

void metadataFailureTests(FlowsProcessor& p) {
    const auto workspace = File(SystemStats::getEnvironmentVariable("FLOWS_WORKSPACE", {}));
    const auto id = documentId(p);
    const auto expected = sequencer(p);
    const auto oldRevision = get(expected, "revision");
    const auto metadata = workspace.getChildFile(".sequencers").getChildFile(id + ".json");
    const auto backup = metadata.getSiblingFile(metadata.getFileName() + ".test-backup");
    check(metadata.moveFileTo(backup) && metadata.createDirectory().wasOk(),
          "Could not obstruct the current pattern's metadata path.");
    rejects([&] { setPattern(p, pattern(3)); }, "Pattern save accepted a directory as the target file.");
    matchesPattern(p, expected, "Failed metadata save must retain the current pattern");
    check(get(sequencer(p), "revision") == oldRevision, "Failed metadata save advanced its revision.");
    check(metadata.deleteRecursively() && backup.moveFileTo(metadata), "Could not restore the test metadata file.");

    const String newName = "blocked-rename.flows";
    const auto obstruction = workspace.getChildFile(".sequencers").getChildFile(id.upToLastOccurrenceOf("/",false,false)+"/"+newName + ".json");
    check(obstruction.createDirectory().wasOk(), "Could not obstruct the renamed pattern's metadata path.");
    rejects([&] { p.request("document.rename", object({{"documentId", id}, {"name", newName}})); },
            "Rename accepted a directory as its metadata destination.");
    check(documentId(p) == id, "Failed metadata preflight changed the active document path.");
    check(workspace.getChildFile(id).existsAsFile() && !workspace.getChildFile(newName).exists(),
          "Failed metadata preflight moved the source file.");
    bool originalOpen = false;
    const auto documents = get(p.snapshot(), "documents");
    for (const auto& d : *documents.getArray()) {
        originalOpen = originalOpen || str(d, "id") == id;
        check(str(d, "id") != newName, "Failed rename changed the open document list.");
    }
    check(originalOpen, "Failed rename removed the original document from the tab list.");
    matchesPattern(p, expected, "Failed rename must preserve the active pattern");
    check(obstruction.deleteRecursively(), "Could not remove the metadata test obstruction.");
}

std::vector<std::size_t> timedOnsets(FlowsProcessor& p, double bpm) {
    setPlaying(p, false); stoppedAndSilent(p, "Before timing capture");
    setPattern(p, pattern());
    p.request("transport.set", object({{"bpm", bpm}}));
    check(static_cast<double>(get(get(p.snapshot(), "audio"), "bpm")) == bpm,
          "BPM request did not reach the active internal transport.");
    setPlaying(p, true);
    std::vector<float> samples;
    for (int block = 0; block < 600; ++block) audio(p, &samples);
    setPlaying(p, false); stoppedAndSilent(p, "After timing capture");
    std::vector<std::size_t> onsets;
    bool wasHigh = false;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const bool high = samples[i] > .1f;
        if (high && !wasHigh) onsets.push_back(i);
        wasHigh = high;
    }
    check(onsets.size() >= 6, "A running pattern did not generate repeated audible notes.");
    return onsets;
}
void playbackTests(FlowsProcessor& p) {
    for (const double bpm : {60.0, 120.0}) {
        const auto onsets = timedOnsets(p, bpm);
        const auto interval = static_cast<std::size_t>(sampleRate * 60 / bpm / 4);
        for (std::size_t i = 1; i < onsets.size(); ++i)
            check(std::abs(static_cast<double>(onsets[i] - onsets[i - 1]) - interval) <= 1,
                  "Step timing did not follow BPM at audio-sample precision.");
    }
    for (const var bad : {var(0), var(301), var("138"), var(true)})
        rejects([&] { p.request("transport.set", object({{"bpm", bad}})); }, "Accepted invalid BPM.");
    auto dense = pattern(8);
    dense.getDynamicObject()->setProperty("length", 32);
    dense.getDynamicObject()->setProperty("division", 32);
    dense.getDynamicObject()->setProperty("swing", .6);
    dense.getDynamicObject()->setProperty("root", 11);
    dense.getDynamicObject()->setProperty("transpose", -24);
    for (const auto* direction : {"forward", "reverse", "pingpong", "random"}) {
        dense.getDynamicObject()->setProperty("direction", direction);
        setPattern(p, dense); setPlaying(p, true);
        double peak = 0;
        for (int i = 0; i < 240; ++i) peak = std::max(peak, audio(p));
        check(peak > .1, std::string("No finite notes generated in direction ") + direction);
        setPlaying(p, false); stoppedAndSilent(p, direction);
    }
    for (const auto* scale : {"chromatic", "major", "minor", "pentatonic"}) {
        auto muted = pattern(); muted.getDynamicObject()->setProperty("scale", scale);
        (*get(muted, "rows").getArray())[0].getDynamicObject()->setProperty("mute", true);
        setPattern(p, muted); setPlaying(p, true);
        for (int i = 0; i < 100; ++i) check(audio(p) < 1e-7, "Muted row still generated notes.");
        setPlaying(p, false);
    }
    setPattern(p, pattern());
    p.request("audio.playDemo", object({{"documentId", documentId(p)}}));
    check(playing(p), "Legacy demo toggle did not start the editable sequencer.");
    for (int i = 0; i < 10; ++i) audio(p);
    p.request("audio.playDemo", object({{"documentId", documentId(p)}}));
    stoppedAndSilent(p, "Legacy toggle stop");

    p.request("keyboard.noteOn", object({{"documentId", documentId(p)},
        {"noteId", 991}, {"pitch", 72}, {"velocity", .4}}));
    audio(p); // Let the 3 ms playback onset settle before testing the held level.
    check(std::abs(audio(p) - .1) < 1e-6, "Manual held-note fixture did not play.");
    setPlaying(p, true);
    for (int i = 0; i < 10; ++i) audio(p);
    setPlaying(p, false);
    for (int i = 0; i < 12; ++i) audio(p);
    check(std::abs(audio(p) - .1) < 1e-6, "Stopping the sequencer also cut the manual held note.");
    p.request("keyboard.noteOff", object({{"documentId", documentId(p)}, {"noteId", 991}, {"pitch", 72}}));
    stoppedAndSilent(p, "Independent manual-note release");
}

void documentAndPersistenceTests(FlowsProcessor& p) {
    auto idA = documentId(p);
    auto patternA = pattern(2, 36);
    patternA.getDynamicObject()->setProperty("direction", "reverse");
    patternA.getDynamicObject()->setProperty("scale", "minor");
    patternA.getDynamicObject()->setProperty("swing", .32);
    setPattern(p, patternA); setPlaying(p, true); audio(p);
    p.request("document.new", object({{"name", "sequence-b.flows"}}));
    const auto idB = documentId(p); check(idB != idA, "Second instrument was not created.");
    applyGateSource(p); stoppedAndSilent(p, "Document focus change");
    const auto patternB = pattern(1, 72); setPattern(p, patternB);
    rejects([&] { p.request("sequencer.set", object({{"documentId", idA},
        {"baseRevision", get(sequencer(p), "revision")}, {"pattern", patternA}})); },
        "An inactive document's pattern edit was applied to the active instrument.");
    rejects([&] { p.request("sequencer.play", object({{"documentId", idA}, {"playing", true}})); },
            "An inactive document started the active sequencer.");
    matchesPattern(p, patternB, "Inactive document rejection");
    p.request("document.activate", object({{"documentId", idA}})); ready(p);
    matchesPattern(p, patternA, "First instrument reactivation"); stoppedAndSilent(p, "First instrument reactivation");
    p.request("document.activate", object({{"documentId", idB}})); ready(p);
    matchesPattern(p, patternB, "Second instrument reactivation");
    p.request("document.activate", object({{"documentId", idA}})); ready(p);

    p.request("document.close", object({{"documentId", idA}, {"save", true}})); ready(p);
    p.request("document.open", object({{"path", idA}})); ready(p);
    matchesPattern(p, patternA, "Closed instrument reopened from disk");
    stoppedAndSilent(p, "Closed instrument reopened from disk");
    p.request("document.rename", object({{"documentId", idA}, {"name", "sequence-a-renamed.flows"}}));
    idA = documentId(p);
    p.request("document.close", object({{"documentId", idA}, {"save", true}})); ready(p);
    p.request("document.open", object({{"path", idA}})); ready(p);
    matchesPattern(p, patternA, "Renamed instrument reopened from disk");

    p.request("transport.set", object({{"bpm", 173}}));
    setPlaying(p, true); audio(p);
    const auto portable = p.request("project.exportText", object({})).toString();
    MemoryBlock state; p.getStateInformation(state);
    check(state.getSize() > 0, "Sequencer DAW state was empty.");
    setPlaying(p, false);
    p.request("document.close", object({{"documentId", idA}, {"save", true}})); ready(p);
    p.request("project.importText", object({{"text", portable}})); ready(p);
    const auto importedId = documentId(p);
    check(importedId != idA, "Package import reused an existing closed instrument's path.");
    setPattern(p, patternB);
    p.request("document.close", object({{"documentId", importedId}, {"discard", true}})); ready(p);
    p.request("document.open", object({{"path", idA}})); ready(p);
    matchesPattern(p, patternA, "Discarded package draft must not overwrite the original pattern");
    p.request("project.importText", object({{"text", portable}})); ready(p);
    matchesPattern(p, patternA, "Package round trip"); stoppedAndSilent(p, "Package import");
    check(static_cast<double>(get(get(p.snapshot(), "preferences"), "bpm")) == 173,
          "Package import lost the authored BPM.");

    // A fresh plugin processor must recall pattern and BPM without starting it.
    FlowsProcessor restored;
    restored.prepareToPlay(sampleRate, blockSize);
    restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    until(restored, [&] {
        const auto seq = sequencer(restored);
        return str(seq, "direction") == "reverse" && str(seq, "scale") == "minor"
            && static_cast<double>(get(seq, "swing")) == .32;
    }, "DAW recall did not restore the saved pattern.");
    ready(restored);
    matchesPattern(restored, patternA, "DAW state round trip"); stoppedAndSilent(restored, "DAW state recall");
    check(static_cast<double>(get(get(restored.snapshot(), "preferences"), "bpm")) == 173,
          "DAW state recall lost the authored BPM.");
    restored.releaseResources();
}

void selectSound(FlowsProcessor& processor,const String& kind,const String& path={}) {
    processor.request("sequencer.sound",object({{"documentId",documentId(processor)},
        {"baseRevision",get(sequencer(processor),"revision")},{"sound",object({{"kind",kind},{"path",path}})}}));
}
void soundReady(FlowsProcessor& processor) {
    until(processor,[&]{return str(sequencer(processor),"soundStatus")=="ready";},"Selected sequencer sound did not prepare/install");
    for(int i=0;i<12;++i)audio(processor);
}
void applySource(FlowsProcessor& processor,const String& source) {
    auto d=document(processor);processor.request("document.apply",object({{"documentId",str(d,"id")},{"baseRevision",get(d,"revision")},{"text",source}}));ready(processor);
}
void constantSample(const File& file,float left=.25f,float right=-.125f,int frames=48000) {
    check(file.getParentDirectory().createDirectory().wasOk(),"Could not create sample fixture folder");
    auto stream=file.createOutputStream();check(stream!=nullptr,"Could not open sample fixture");
    WavAudioFormat format;std::unique_ptr<AudioFormatWriter> writer(format.createWriterFor(stream.get(),sampleRate,2,32,{},0));
    check(writer!=nullptr,"Could not write sample fixture");stream.release();
    AudioBuffer<float> buffer(2,frames);for(int i=0;i<frames;++i){buffer.setSample(0,i,left);buffer.setSample(1,i,right);}
    check(writer->writeFromAudioSampleBuffer(buffer,0,frames)&&writer->flush(),"Could not finish sample fixture");
}
void guardAudio(FlowsProcessor& processor) {
    AudioBuffer<float> buffer(2,128);MidiBuffer midi;
    sequencerHeapTouches=0;watchSequencerHeap=true;processor.processBlock(buffer,midi);watchSequencerHeap=false;
    check(sequencerHeapTouches==0,"Independent sequencer audio touched the heap");
}
void explicitUnitTests() {
    TestWorkspace workspace;FlowsProcessor processor(true);processor.prepareToPlay(sampleRate,blockSize);applyGateSource(processor);
    const auto owner=documentId(processor);const auto unit=workspace.directory.getChildFile("unit.flows");
    check(unit.replaceWithText(gateSource.replace("0.25","0.5")),"Could not create instrument unit fixture");
    for(const auto sound: {object({{"kind","unknown"}}),object({{"kind","instrument"},{"path","../outside.flows"}}),object({{"kind","sample"},{"path","Samples/missing.wav"}}),object({{"kind","instrument"},{"path","unit.wav"}})}) {
        rejects([&]{auto value=pattern();value.getDynamicObject()->setProperty("sound",sound);setPattern(processor,value);},"Accepted invalid sound selector");
        check(str(get(sequencer(processor),"sound"),"kind")=="active","Rejected sound mutated selection");
    }
    auto value=pattern();for(auto& step:*get((*get(value,"rows").getArray())[0],"steps").getArray())step.getDynamicObject()->setProperty("gate",1.0);
    setPattern(processor,value);
    processor.request("keyboard.noteOn",object({{"documentId",owner},{"noteId",900},{"pitch",72},{"velocity",1.0}}));
    setPlaying(processor,true);check(audio(processor)>.4,"Follow-active sound did not mix with a held keyboard note");
    selectSound(processor,"instrument","unit.flows");soundReady(processor);check(playing(processor),"Selecting a sound stopped transport");
    // The former active-instrument step must be released without cancelling the
    // physical keyboard note. Only .25 keyboard + .4 selected-unit may remain.
    auto level=audio(processor);check(level>.64&&level<.66,"Follow-active to pinned swap left a held step or lost the keyboard note");
    processor.request("keyboard.noteOff",object({{"documentId",owner},{"noteId",900},{"pitch",72}}));audio(processor);check(audio(processor)>.39&&audio(processor)<.41,"Pinned sound was routed through the keyboard engine");
    processor.request("document.new",object({{"name","Cursor instrument"}}));applyGateSource(processor);
    check(playing(processor)&&str(get(sequencer(processor),"sound"),"path")=="unit.flows"&&str(sequencer(processor),"ownerDocumentId")==owner,"Editor focus lost pinned source/owner/playback");
    processor.request("keyboard.noteOn",object({{"documentId",documentId(processor)},{"noteId",901},{"pitch",72},{"velocity",1.0}}));audio(processor);level=audio(processor);check(level>.64&&level<.66,"Cursor-active keyboard did not mix with independent unit");
    processor.request("keyboard.noteOff",object({{"documentId",documentId(processor)},{"noteId",901},{"pitch",72}}));audio(processor);
    for(int i=0;i<16;++i)guardAudio(processor);
    setPlaying(processor,false);stoppedAndSilent(processor,"Pinned instrument stop");
    // Ungated mono code must not bypass Play/Stop merely because its DSP is ready.
    const auto mono=workspace.directory.getChildFile("continuous.flows");check(mono.replaceWithText("import standard as std\nInst.sigstream(std.event.keys.mono()) as key:\n    return 0.25\n"),"Could not create mono fixture");
    selectSound(processor,"instrument","continuous.flows");soundReady(processor);check(audio(processor)<1e-7,"Stopped ungated mono unit was audible");
    setPlaying(processor,true);std::vector<float> routeOn;audio(processor,&routeOn);
    check(routeOn.front()==0&&routeOn.back()>.2,"Ungated mono unit did not fade into playback");
    setPlaying(processor,false);std::vector<float> routeOff;audio(processor,&routeOff);audio(processor,&routeOff);
    check(routeOff.front()==routeOn.back()&&routeOff[144]==0,"Sequencer Stop cut the route or left an unbounded tail");
    stoppedAndSilent(processor,"Ungated mono unit stop");
    selectSound(processor,"instrument","unit.flows");soundReady(processor);setPlaying(processor,true);
    processor.request("document.open",object({{"path","unit.flows"}}));ready(processor);
    applySource(processor,gateSource.replace("0.25","0.125"));soundReady(processor);for(int i=0;i<16;++i)guardAudio(processor);
    check(audio(processor)>.09&&audio(processor)<.11,"Valid selected-instrument draft did not update sequencer sound");
    auto d=document(processor);processor.request("document.apply",object({{"documentId",str(d,"id")},{"baseRevision",get(d,"revision")},{"text","invalid source\n"}}));
    until(processor,[&]{return str(get(processor.snapshot(),"compile"),"status")=="error";},"Invalid unit draft was not diagnosed");
    for(int i=0;i<4;++i)audio(processor);
    check(audio(processor)<1e-7,"Invalid selected-instrument draft left historical sequencer audio audible");
    processor.request("document.save",object({}));
    check(audio(processor)<1e-7,"Saving an invalid selected sound resumed old audio");
    applySource(processor,gateSource.replace("0.25","0.125"));soundReady(processor);
    for(int i=0;i<4;++i)audio(processor);
    check(audio(processor)>.09&&audio(processor)<.11,"Repair did not resume the selected sequencer sound");
    const auto ownerMetadata=workspace.directory.getChildFile(".sequencers").getChildFile(owner+".json");
    const auto metadataBackup=ownerMetadata.getSiblingFile("owner-pattern.backup");
    check(ownerMetadata.moveFileTo(metadataBackup)&&ownerMetadata.createDirectory().wasOk(),"Could not obstruct selected-unit owner metadata");
    rejects([&]{processor.request("document.rename",object({{"documentId","unit.flows"},{"name","renamed-unit.flows"}}));},"Source rename ignored failed owner metadata persistence");
    check(documentId(processor)=="unit.flows"&&unit.existsAsFile()&&!workspace.directory.getChildFile("renamed-unit.flows").exists(),"Failed owner metadata update moved selected source");
    check(ownerMetadata.deleteRecursively()&&metadataBackup.moveFileTo(ownerMetadata),"Could not restore owner metadata");
    processor.request("document.rename",object({{"documentId","unit.flows"},{"name","renamed-unit.flows"}}));
    check(str(get(sequencer(processor),"sound"),"path")=="renamed-unit.flows","Renaming selected source lost global sound identity");
    const auto metadata=parseJSONBounded(ownerMetadata.loadFileAsString(),8*1024*1024);
    check(str(get(metadata,"sound"),"path")=="renamed-unit.flows","Renaming selected source did not persist its owner's descriptor");
    processor.request("document.open",object({{"path",owner}}));ready(processor);
    applySource(processor,workspace.directory.getChildFile("examples/sine-reference.flows").loadFileAsString());
    check(audio(processor)>.09,"Pinned source stopped after changing the observed editor");
    processor.request("probe.listen",object({{"documentId",documentId(processor)},{"signal","raw"},{"enabled",true}}));
    for(int i=0;i<12;++i)audio(processor);
    check(audio(processor)<1e-7&&playing(processor),"Probe audition leaked independent sequencer audio or stopped its transport");
    processor.request("probe.listen",object({{"documentId",documentId(processor)},{"enabled",false}}));
    check(audio(processor)>.09,"Disabling probe audition did not restore independent unit sound");
    setPlaying(processor,false);stoppedAndSilent(processor,"After live source updates");
    processor.releaseResources();
}
void sampleUnitAndPortabilityTests() {
    TestWorkspace original;FlowsProcessor sender;sender.prepareToPlay(sampleRate,blockSize);applyGateSource(sender);
    const auto input=original.directory.getChildFile("external/Step.wav");constantSample(input);
    const auto sample=str(sender.importSample(input),"sampleImportedPath");
    auto value=pattern(1,48);for(auto& step:*get((*get(value,"rows").getArray())[0],"steps").getArray())step.getDynamicObject()->setProperty("velocity",1.0);
    setPattern(sender,value);sender.request("sequencer.sound",object({{"documentId",documentId(sender)},{"baseRevision",get(sequencer(sender),"revision")},{"sound",object({{"kind","sample"},{"path",sample},{"playback","gated"}})}}));soundReady(sender);check(audio(sender)<1e-7,"Selecting sample started playback");
    sender.request("transport.set",object({{"bpm",120}}));setPlaying(sender,true);
    std::vector<float> samples;for(int i=0;i<120;++i)audio(sender,&samples);
    std::vector<std::size_t> onsets;bool high=false;for(std::size_t i=0;i<samples.size();++i){bool now=samples[i]>.01;if(now&&!high)onsets.push_back(i);high=now;}
    check(onsets.size()>=3&&onsets.front()>0&&onsets.front()<144,"Sample did not cross the threshold within its 3 ms onset correction");
    for(std::size_t i=1;i<onsets.size();++i)check(onsets[i]-onsets[i-1]==6000,"Sample step timing drifted from BPM");
    check(samples[0]==0&&samples[1]>0&&samples[144]>.12&&samples[144]<.13&&samples[2544]==0,"Sample root C3 or step gate/release semantics are wrong");
    AudioBuffer<float> output(2,128);MidiBuffer midi;sender.processBlock(output,midi);check(output.getSample(0,0)>=0&&output.getSample(1,0)<=0,"Stereo sample lost channel polarity");
    for(int i=0;i<16;++i)guardAudio(sender);
    setPlaying(sender,false);stoppedAndSilent(sender,"Sample stop");
    const auto package=sender.request("project.exportText",object({})).toString();MemoryBlock saved;sender.getStateInformation(saved);
    auto decoded=parseJSONBounded(package,maximumInstrumentPackageBytes);check(get(decoded,"assets").getArray()->size()==1&&str(get(get(decoded,"sequencer"),"sound"),"source").contains("ClassicSampler"),"Portable sequence did not embed sample source/assets");
    {
        TestWorkspace fresh;const auto collision=fresh.directory.getChildFile(sample);constantSample(collision,.1f,.2f,4800);
        FlowsProcessor receiver;receiver.prepareToPlay(sampleRate,blockSize);receiver.request("project.importText",object({{"text",package}}));ready(receiver);soundReady(receiver);
        const auto restoredPath=str(get(sequencer(receiver),"sound"),"path");check(restoredPath!=sample&&fresh.directory.getChildFile(restoredPath).existsAsFile(),"Portable sample collision did not remap selector and source");
        // Editing a whole snapshot preserves its captured source, independent of
        // any original instrument/sample filename being available in this workspace.
        auto edited=sequencer(receiver).clone();edited.getDynamicObject()->setProperty("swing",.1);setPattern(receiver,edited);
        setPlaying(receiver,true);check(audio(receiver)>.05,"Portable sample unit did not play");setPlaying(receiver,false);stoppedAndSilent(receiver,"Portable sample stop");
        FlowsProcessor daw;daw.prepareToPlay(sampleRate,blockSize);daw.setStateInformation(saved.getData(),static_cast<int>(saved.getSize()));
        until(daw,[&]{return str(get(sequencer(daw),"sound"),"kind")=="sample";},"DAW recall lost selected sample unit");ready(daw);soundReady(daw);check(!playing(daw),"DAW recall restarted transport");setPlaying(daw,true);check(audio(daw)>.05,"DAW recalled sample unit did not play");setPlaying(daw,false);daw.releaseResources();receiver.releaseResources();
    }
    // A .flows unit source remains portable even after its original file is gone.
    const auto external=original.directory.getChildFile("independent.flows");check(external.replaceWithText(gateSource),"Could not create portable instrument unit");
    selectSound(sender,"instrument","independent.flows");soundReady(sender);const auto instrumentPackage=sender.request("project.exportText",object({})).toString();check(external.deleteFile(),"Could not delete original instrument unit");
    {
        TestWorkspace fresh;FlowsProcessor receiver;receiver.prepareToPlay(sampleRate,blockSize);receiver.request("project.importText",object({{"text",instrumentPackage}}));ready(receiver);soundReady(receiver);
        check(!fresh.directory.getChildFile("independent.flows").exists(),"Test unexpectedly recreated original unit file");
        auto edited=sequencer(receiver).clone();edited.getDynamicObject()->setProperty("transpose",3);setPattern(receiver,edited);setPlaying(receiver,true);check(audio(receiver)>.1,"Portable source snapshot could not play/edit without original file");setPlaying(receiver,false);receiver.releaseResources();
    }
    sender.releaseResources();
}

void workspacePinnedRecoveryTests() {
    TestWorkspace workspace;String owner,cursor,sample;
    {
        FlowsProcessor processor(true);processor.prepareToPlay(sampleRate,blockSize);applyGateSource(processor);owner=documentId(processor);
        processor.request("document.save",object({}));
        const auto input=workspace.directory.getChildFile("external/RecoveryUnit.wav");constantSample(input);
        sample=str(processor.importSample(input),"sampleImportedPath");selectSound(processor,"sample",sample);soundReady(processor);
        auto value=pattern(1,48);value.getDynamicObject()->setProperty("swing",.2);setPattern(processor,value);
        processor.request("document.new",object({{"name","Recovery cursor"}}));applyGateSource(processor);cursor=documentId(processor);processor.request("document.save",object({}));
        setPlaying(processor,true);audio(processor);processor.releaseResources();
    }
    check(workspace.directory.getChildFile(sample).deleteFile(),"Could not remove recoverable unit sample");
    FlowsProcessor restored(true);restored.prepareToPlay(sampleRate,blockSize);ready(restored);soundReady(restored);
    check(documentId(restored)==cursor&&str(sequencer(restored),"ownerDocumentId")==owner&&str(get(sequencer(restored),"sound"),"kind")=="sample"&&static_cast<double>(get(sequencer(restored),"swing"))==.2,"Workspace recovery lost focused editor versus pinned sequence identity");
    check(workspace.directory.getChildFile(sample).existsAsFile()&&!playing(restored),"Workspace recovery lost sample bytes or restarted transport");
    setPlaying(restored,true);check(audio(restored)>.02,"Recovered independent sample did not play");setPlaying(restored,false);stoppedAndSilent(restored,"Workspace pinned recovery stop");restored.releaseResources();
}
void metadataEscapeAndMixedOverflowTests() {
    TestWorkspace workspace;FlowsProcessor processor;processor.prepareToPlay(sampleRate,blockSize);applyGateSource(processor);processor.request("document.save",object({}));
    const auto owner=documentId(processor),path=String("escaped.flows");
    const auto source=gateSource+"\n#"+String::repeatedString("\"\\",460000)+"\n";
    check(source.getNumBytesAsUTF8()<1024*1024&&workspace.directory.getChildFile(path).replaceWithText(source),"Could not create escaped near-budget source");
    selectSound(processor,"instrument",path);soundReady(processor);
    const auto metadata=workspace.directory.getChildFile(".sequencers").getChildFile(owner+".json");check(metadata.getSize()>1024*1024+65536,"Escaped metadata fixture did not exceed former read budget");
    {FlowsProcessor reopened;reopened.prepareToPlay(sampleRate,blockSize);ready(reopened);soundReady(reopened);check(str(get(sequencer(reopened),"sound"),"path")==path,"Saved escaped source did not reload its pinned sound");reopened.releaseResources();}
    const auto huge=gateSource.replace("0.25","2e38");const auto file=workspace.directory.getChildFile("large-gain.flows");check(file.replaceWithText(huge),"Could not create finite large-gain fixture");
    applySource(processor,huge);selectSound(processor,"instrument","large-gain.flows");soundReady(processor);
    auto value=pattern();for(auto& step:*get((*get(value,"rows").getArray())[0],"steps").getArray())step.getDynamicObject()->setProperty("velocity",1.0);setPattern(processor,value);
    processor.request("keyboard.noteOn",object({{"documentId",owner},{"noteId",700},{"pitch",60},{"velocity",1.0}}));setPlaying(processor,true);check(audio(processor)<1e-7,"Overflowing independent-session sum was not silenced");
    until(processor,[&]{return static_cast<int64>(get(processor.telemetry(),"processErrors"))>0;},"Mixer overflow did not report an engine error");
    setPlaying(processor,false);selectSound(processor,"active");processor.releaseResources();
}
void independentLaneTests() {
    TestWorkspace workspace;FlowsProcessor processor;processor.prepareToPlay(sampleRate,blockSize);applyGateSource(processor);
    check(workspace.directory.getChildFile("lane-a.flows").replaceWithText(gateSource),"Could not write lane A");
    check(workspace.directory.getChildFile("lane-b.flows").replaceWithText(gateSource.replace("0.25","0.5")),"Could not write lane B");
    auto value=pattern(2);
    Array<var> sounds{object({{"kind","instrument"},{"path","lane-a.flows"}}),object({{"kind","instrument"},{"path","lane-b.flows"}})};
    value.getDynamicObject()->setProperty("sound",object({{"kind","lanes"},{"lanes",sounds}}));
    for(auto& row:*get(value,"rows").getArray())for(auto& step:*get(row,"steps").getArray())step.getDynamicObject()->setProperty("gate",1.0);
    setPattern(processor,value);soundReady(processor);setPlaying(processor,true);
    for(int i=0;i<12;++i)audio(processor);
    check(std::abs(audio(processor)-.6)<1e-5,"Independent lane sources did not sum .2 + .4");guardAudio(processor);
    (*get(value,"rows").getArray())[1].getDynamicObject()->setProperty("mute",true);setPattern(processor,value);
    for(int i=0;i<12;++i)audio(processor);
    check(std::abs(audio(processor)-.2)<1e-5,"Muting lane B changed lane A or left B playing");
    const auto package=processor.request("project.exportText",{}).toString();
    check(package.contains("lane-b.flows")&&package.contains("lanes"),"Portable instrument lost lane sound configuration");
    setPlaying(processor,false);stoppedAndSilent(processor,"Independent lane stop");
    auto bad=pattern(2);bad.getDynamicObject()->setProperty("sound",object({{"kind","lanes"},{"lanes",Array<var>{sounds[0]}}}));
    rejects([&]{setPattern(processor,bad);},"Mismatched lane source count was accepted");
    processor.releaseResources();
}
void mixerAndOneShotTests() {
    TestWorkspace workspace;FlowsProcessor p(true);p.prepareToPlay(sampleRate,blockSize);
    applySource(p,gateSource.replace(" * key.velocity",""));
    auto value=pattern(2);for(auto& row:*get(value,"rows").getArray())for(auto& step:*get(row,"steps").getArray())step.getDynamicObject()->setProperty("gate",1.0);
    setPattern(p,value);setPlaying(p,true);for(int i=0;i<12;++i)audio(p);
    check(std::abs(audio(p)-.5)<1e-5,"Shared instrument lanes did not start together");
    auto rows=get(value,"rows");rows[0].getDynamicObject()->setProperty("volume",-6.0);setPattern(p,value);
    for(int i=0;i<12;++i)audio(p);
    check(std::abs(audio(p)-(.25+.25*std::pow(10.,-.3)))<1e-5,"Lane fader incorrectly depended on key.velocity");
    p.request("keyboard.noteOn",object({{"noteId",999},{"pitch",72},{"velocity",1.0}}));
    rows[0].getDynamicObject()->setProperty("mute",true);rows[1].getDynamicObject()->setProperty("mute",true);setPattern(p,value);
    for(int i=0;i<12;++i)audio(p);
    check(std::abs(audio(p)-.25)<1e-5&&playing(p),"Muting sequencer lanes changed the keyboard or stopped transport");
    p.request("keyboard.noteOff",object({{"noteId",999},{"pitch",72}}));for(int i=0;i<12;++i)audio(p);
    check(audio(p)<1e-7&&playing(p),"All-muted transport was not silent");
    value.getDynamicObject()->setProperty("metronome",object({{"mute",false},{"division",8},{"volume",-12}}));setPattern(p,value);
    double peak=0;for(int i=0;i<220;++i)peak=std::max(peak,audio(p));check(peak>.05,"Metronome did not sound independently of muted note lanes");
    get(value,"metronome").getDynamicObject()->setProperty("mute",true);setPattern(p,value);for(int i=0;i<12;++i)audio(p);check(audio(p)<1e-7&&playing(p),"Muting metronome stopped the silent clock or leaked audio");
    setPlaying(p,false);stoppedAndSilent(p,"Mixer stop");
    const auto input=workspace.directory.getChildFile("external/one-shot.wav");constantSample(input,.25f,-.125f,4800);
    const auto path=str(p.importSample(input),"sampleImportedPath");value=pattern(1,48);
    auto steps=get(get(value,"rows")[0],"steps");for(int i=0;i<32;++i){steps[i].getDynamicObject()->setProperty("active",i==0);steps[i].getDynamicObject()->setProperty("gate",.05);steps[i].getDynamicObject()->setProperty("velocity",1.0);}
    setPattern(p,value);selectSound(p,"sample",path);soundReady(p);setPlaying(p,true);
    std::vector<float> samples;for(int i=0;i<45;++i)audio(p,&samples);
    check(samples[3000]>.12&&samples[4700]>.12&&samples[5000]==0,"One-shot did not preserve its tail at original C3 speed or stop at EOF");
    setPlaying(p,false);stoppedAndSilent(p,"One-shot stop");
    auto bad=value.clone();get(bad,"rows")[0].getDynamicObject()->setProperty("volume",std::numeric_limits<double>::infinity());rejects([&]{setPattern(p,bad);},"Nonfinite lane volume was accepted");
    p.releaseResources();
}
void workspaceImportTests() {
    TestWorkspace workspace;
    auto tutorial=workspace.directory.getChildFile("ADCxGather/01-signals/first.flows");tutorial.getParentDirectory().createDirectory();tutorial.replaceWithText(gateSource);
    workspace.directory.getChildFile("mine.flows").replaceWithText(gateSource);
    FlowsProcessor processor;processor.prepareToPlay(sampleRate,blockSize);ready(processor);
    check(workspace.directory.getChildFile("tutorial/01-signals/first.flows").existsAsFile(),"Tutorial migration failed");
    check(workspace.directory.getChildFile("examples/mine.flows").existsAsFile()&&workspace.directory.getChildFile(".workspace-layout-backup/mine.flows").existsAsFile(),"Workspace migration did not preserve source backup");
    const auto encoded=Base64::toBase64(gateSource.toRawUTF8(),gateSource.getNumBytesAsUTF8());
    auto imported=processor.request("workspace.import",object({{"folder","AI-gen"},{"name","dropped.flows"},{"data",encoded}}));
    check(str(imported,"importedPath")=="AI-gen/dropped.flows","Drop did not target selected folder");
    imported=processor.request("workspace.import",object({{"folder","AI-gen"},{"name","dropped.flows"},{"data",encoded}}));
    check(str(imported,"importedPath")!="AI-gen/dropped.flows","Duplicate drop overwrote an instrument");
    rejects([&]{processor.request("workspace.import",object({{"folder","../escape"},{"name","bad.flows"},{"data",encoded}}));},"Drop allowed traversal outside workspace");
    rejects([&]{processor.request("workspace.import",object({{"folder","AI-gen"},{"name","bad.exe"},{"data",encoded}}));},"Drop accepted unsupported file type");
    const auto source=workspace.directory.getChildFile("external/drop.wav");constantSample(source);
    MemoryBlock bytes;source.loadFileAsData(bytes);
    imported=processor.request("workspace.import",object({{"folder","tutorial/01-signals"},{"name","drop.wav"},{"data",Base64::toBase64(bytes.getData(),bytes.getSize())}}));
    const auto samplePath=str(imported,"importedPath");
    check(samplePath=="tutorial/01-signals/drop.wav","Audio drop lost destination folder");
    selectSound(processor,"sample",samplePath);soundReady(processor);
    processor.releaseResources();
}

} // namespace

int main() {
    try {
        setenv("FLOWS_DISABLE_LIVE_BRIDGE","1",1);
        TestWorkspace workspace;
        ScopedJuceInitialiser_GUI initialise;
        {
            FlowsProcessor processor(true);
            processor.prepareToPlay(sampleRate, blockSize);
            applyGateSource(processor);
            validationTests(processor);
            metadataFailureTests(processor);
            playbackTests(processor);
            documentAndPersistenceTests(processor);
            processor.releaseResources();
        }
        independentLaneTests();mixerAndOneShotTests();workspaceImportTests();explicitUnitTests();sampleUnitAndPortabilityTests();workspacePinnedRecoveryTests();metadataEscapeAndMixedOverflowTests();
        std::cout << "Sequencer host integration passed: validation, stale edits, sample-accurate BPM, "
                     "multi-row notes, directions, mute/stop release, document isolation, portable and DAW recall.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Sequencer host integration failure: " << error.what() << '\n';
        return 1;
    }
}
