#include "host/app/FlowsProcessor.hpp"
#include "host/app/SampleAssets.hpp"
#include <chrono>
#include <cmath>
#include <filesystem>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace {
using namespace juce;
using namespace flows::app;
using Clock=std::chrono::steady_clock;
void check(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
template<class Work> void rejects(Work work,const char* message) { bool failed=false; try { work(); } catch(...) { failed=true; } check(failed,message); }
struct Workspace {
    File root=File::getSpecialLocation(File::tempDirectory).getNonexistentChildFile("flows-sample-host","",false);
    String previous=SystemStats::getEnvironmentVariable("FLOWS_WORKSPACE",{});
    Workspace() { check(root.createDirectory().wasOk(),"Could not create test workspace"); setenv("FLOWS_AI_CONFIG_DIR",root.getChildFile("test-global-ai").getFullPathName().toRawUTF8(),1); setenv("FLOWS_WORKSPACE",root.getFullPathName().toRawUTF8(),1); }
    ~Workspace() { if(previous.isEmpty()) unsetenv("FLOWS_WORKSPACE"); else setenv("FLOWS_WORKSPACE",previous.toRawUTF8(),1); root.deleteRecursively(); }
};
void wav(const File& file,int frames=4800,float left=.25f,float right=-.125f) {
    check(file.getParentDirectory().createDirectory().wasOk(),"Could not create fixture folder");
    auto output=file.createOutputStream(); check(output!=nullptr,"Could not write WAV fixture");
    WavAudioFormat format; std::unique_ptr<AudioFormatWriter> writer(format.createWriterFor(output.get(),48000,2,32,{},0));
    check(writer!=nullptr,"Could not make fixture writer"); output.release();
    AudioBuffer<float> buffer(2,1024);
    for(int offset=0;offset<frames;offset+=1024) {
        const auto count=std::min(1024,frames-offset);
        for(int i=0;i<count;++i) { buffer.setSample(0,i,left); buffer.setSample(1,i,right); }
        check(writer->writeFromAudioSampleBuffer(buffer,0,count),"Could not write WAV samples");
    }
    check(writer->flush(),"Could not finish WAV fixture");
}
MemoryBlock bytes(const File& file) { MemoryBlock result; check(file.loadFileAsData(result),"Could not read fixture bytes"); return result; }
String docId(FlowsProcessor& p) { return str(get(p.snapshot(),"document"),"id"); }
String source(FlowsProcessor& p) { return str(get(p.snapshot(),"document"),"text"); }
void apply(FlowsProcessor& p,const String& text) { const auto d=get(p.snapshot(),"document"); p.request("document.apply",object({{"documentId",get(d,"id")},{"baseRevision",get(d,"revision")},{"text",text}})); }
double audio(FlowsProcessor& p,bool trigger=false) {
    AudioBuffer<float> buffer(2,128); MidiBuffer events; if(trigger) events.addEvent(MidiMessage::noteOn(1,60,.8f),0);
    p.processBlock(buffer,events); double peak=0;
    for(int c=0;c<2;++c) for(int i=0;i<128;++i) { check(std::isfinite(buffer.getSample(c,i)),"Native sampler emitted non-finite audio"); peak=std::max(peak,std::abs(double(buffer.getSample(c,i)))); }
    return peak;
}
template<class Predicate> void until(FlowsProcessor& p,Predicate predicate,const char* message) {
    const auto end=Clock::now()+std::chrono::seconds(15);
    while(!predicate()) { check(Clock::now()<end,message); audio(p); std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
}
void ready(FlowsProcessor& p) { until(p,[&]{ const auto state=p.snapshot(); return str(get(state,"compile"),"status")=="ready" && get(get(state,"audio"),"programRevision")==get(get(state,"document"),"revision"); },"Sampler did not compile/install"); }
String sampler(const String& path) { return "import standard as std\nfrom lib.sample import ClassicSampler\nInst.sigstream(std.event.keys.poly()) as key:\n    sampler = ClassicSampler.make()\n    raw = sampler(file: "+JSON::toString(var(path))+", trigger: key.trigger, pitch: key.pitch, root: C3, level: -6db)\n    p_raw = probe(raw)\n    return raw * key.velocity\n"; }
void assets_and_native() {
    Workspace workspace; const auto input=workspace.root.getChildFile("external/Tone.wav"); wav(input);
    FlowsProcessor processor(true); processor.prepareToPlay(48000,128);
    auto state=processor.importSample(input); const auto imported=str(state,"sampleImportedPath");
    check(imported=="Samples/Tone.wav","Imported asset lost its readable canonical WAV name");
    bool listed=false; auto samples=get(state,"samples");
    for(const auto& item:*samples.getArray()) if(str(item,"path")==imported) listed=static_cast<int>(get(item,"channels"))==2 && std::abs(static_cast<double>(get(item,"duration"))-.1)<1e-6;
    check(listed,"Sample catalogue lost duration or channel metadata");
    check(str(processor.importSample(input),"sampleImportedPath")!=imported,"Repeated import replaced an existing asset");
    processor.request("document.new",object({{"name","Sample instrument"},{"samplePath",imported}})); ready(processor);
    check(source(processor).contains("ClassicSampler") && source(processor).contains(imported),"New sampler instrument did not bind its managed file");
    check(audio(processor,true)>.05,"Imported sample did not reach native audio output");
    rejects([&]{processor.request("document.new",object({{"samplePath","../outside.wav"}}));},"Sampler creation accepted a path outside its catalogue");
    auto package=parseJSONBounded(processor.request("project.exportText",{}).toString(),maximumInstrumentPackageBytes);
    check(get(package,"assets").getArray() && get(package,"assets").getArray()->size()==1,"Portable package did not deduplicate current/playable asset references");
    apply(processor,"invalid sample draft\n"); until(processor,[&]{return str(get(processor.snapshot(),"compile"),"status")=="error";},"Invalid draft did not report a compile error");
    const auto recovered=parseJSONBounded(processor.request("project.exportText",{}).toString(),maximumInstrumentPackageBytes);
    check(get(recovered,"assets").getArray()->size()==1 && str(recovered,"playableSource").contains(imported),"Invalid draft lost its last playable sample dependency");
    apply(processor,sampler("Samples/missing.wav")); until(processor,[&]{return str(get(processor.snapshot(),"compile"),"status")=="error";},"Missing sample was marked ready");
    check(audio(processor,true)<1e-7,"Missing sample draft played the last valid sound");
    check(workspace.root.getChildFile(imported).deleteFile(),"Could not remove the working asset");
    apply(processor,source(processor)+"# newest recoverable draft\n"); MemoryBlock draftState; processor.getStateInformation(draftState);
    MemoryInputStream stateReader(draftState,false); check(stateReader.readInt()==0x464c5731,"Saved sample state lost its header");
    const auto bodySize=stateReader.readInt(); stateReader.setPosition(136);
    const auto draftPackage=parseJSONBounded(stateReader.readString(),maximumInstrumentPackageBytes);
    check(bodySize>0 && str(draftPackage,"source").contains("newest recoverable draft") && get(draftPackage,"assets").getArray()->size()==1,"Unresolved draft or deleted working asset erased the latest recoverable state");
    const auto bad=input.getSiblingFile("bad.wav"); bad.replaceWithText("not audio"); rejects([&]{processor.importSample(bad);},"Malformed audio was imported");
}
void portability() {
    Workspace first; const auto input=first.root.getChildFile("external/Tone.wav"); wav(input,600000);
    FlowsProcessor sender; sender.prepareToPlay(48000,128);
    const auto path=str(sender.importSample(input),"sampleImportedPath"); apply(sender,sampler(path)); ready(sender);
    const auto package=sender.request("project.exportText",{}).toString(); MemoryBlock saved; sender.getStateInformation(saved);
    check(saved.getSize()>3*1024*1024,"Portable state test did not exceed the legacy 3 MiB limit");
    Workspace second; const auto collision=second.root.getChildFile(path); wav(collision,4800,.1f,.2f); const auto original=bytes(collision);
    FlowsProcessor receiver; receiver.prepareToPlay(48000,128);
    receiver.request("project.importText",object({{"text",package}})); ready(receiver);
    check(!source(receiver).contains(JSON::toString(var(path))),"Conflicting embedded sample did not rewrite source to a unique path");
    check(bytes(collision)==original,"Package import overwrote an existing sample");
    check(audio(receiver,true)>.05,"Remapped portable sample did not play");
    FlowsProcessor daw; daw.prepareToPlay(48000,128); daw.setStateInformation(saved.getData(),static_cast<int>(saved.getSize()));
    until(daw,[&]{return source(daw).contains("ClassicSampler");},"Large sample DAW state was rejected"); ready(daw); check(audio(daw,true)>.05,"DAW sample state did not restore audio");
}
void recovery() {
    Workspace workspace; const auto input=workspace.root.getChildFile("external/Recover.wav"); wav(input); String path;
    {
        FlowsProcessor p(true); p.prepareToPlay(48000,128); path=str(p.importSample(input),"sampleImportedPath");
        p.request("document.new",object({{"name","Recover sampler"},{"samplePath",path}})); ready(p);
        p.request("document.save",object({}));
        apply(p,sampler("Samples/missing-recovery.wav"));
        until(p,[&]{return str(get(p.snapshot(),"compile"),"status")=="error";},"Missing recovery draft did not fail");
    }
    check(workspace.root.getChildFile(path).deleteFile(),"Could not remove sample before recovery");
    FlowsProcessor restored(true); restored.prepareToPlay(48000,128);
    until(restored,[&]{const auto state=restored.snapshot(); return str(get(state,"compile"),"status")=="error" && static_cast<int64>(get(get(state,"audio"),"programRevision"))==0;},"Recovery did not mute an invalid sample draft");
    check(workspace.root.getChildFile(path).existsAsFile(),"Workspace recovery did not restore embedded sample bytes");
    check(source(restored).contains("missing-recovery.wav") && audio(restored,true)<1e-7,"Recovery changed the draft or sounded its historical sample");
}
void literals_and_containment() {
    const String text="# 'Samples/ignored.wav'\npath = 'Samples/Bob\\'s.wav'\nother = \"Samples/a\\\"b.wav\"\n";
    const auto refs=referencedSamplePaths({text});
    check(refs.size()==2 && refs.contains("Samples/Bob's.wav") && refs.contains("Samples/a\"b.wav"),"Resource collection did not match compiler string escapes/comments");
    const auto rewritten=remapSamplePaths(text,{{"Samples/Bob's.wav","Samples/A's.wav"},{"Samples/a\"b.wav","Samples/c\"d.wav"}});
    check(rewritten.contains("'Samples/A\\'s.wav'") && rewritten.contains("\"Samples/c\\\"d.wav\""),"Path remapping produced invalid quoted source");
    Workspace workspace; SampleAssets assets; assets.setRoot(workspace.root);
    auto external=workspace.root.getSiblingFile(workspace.root.getFileName()+"-external.wav"); wav(external);
    std::error_code status; std::filesystem::create_directories(workspace.root.getChildFile("Samples").getFullPathName().toStdString(),status);
    std::filesystem::create_symlink(external.getFullPathName().toStdString(),workspace.root.getChildFile("Samples/outside.wav").getFullPathName().toStdString(),status);
    check(!status,"Could not create containment test symlink"); check(!assets.isManaged("Samples/outside.wav"),"Catalogue accepted an escaping symlink");
    auto data=bytes(external); auto entry=object({{"path","../escape.wav"},{"encoding","base64"},{"data",Base64::toBase64(data.getData(),data.getSize())}});
    rejects([&]{ assets.restore(Array<var>{entry}); },"Embedded assets accepted path traversal");
    entry.getDynamicObject()->setProperty("path","Samples/outside.wav"); rejects([&]{assets.restore(Array<var>{entry});},"Embedded assets wrote through an escaping symlink");
    entry.getDynamicObject()->setProperty("path","Samples/valid-first.wav");
    auto malformed=object({{"path","Samples/invalid-second.wav"},{"encoding","base64"},{"data","not base64"}});
    rejects([&]{assets.restore(Array<var>{entry,malformed});},"Invalid later asset did not reject the batch");
    check(!workspace.root.getChildFile("Samples/valid-first.wav").exists(),"Restore published an early asset before validating the complete batch");
    workspace.root.getChildFile("blocked").replaceWithText("file blocks folder");
    auto blocked=object({{"path","blocked/second.wav"},{"encoding","base64"},{"data",Base64::toBase64(data.getData(),data.getSize())}});
    rejects([&]{assets.restore(Array<var>{entry,blocked});},"Blocked second asset unexpectedly restored");
    check(!workspace.root.getChildFile("Samples/valid-first.wav").exists(),"Restore did not roll back earlier files after a later write failed");
    external.deleteFile();
}
}
int main() {
    setenv("FLOWS_DISABLE_LIVE_BRIDGE","1",1);
    ScopedJuceInitialiser_GUI gui;
    try { assets_and_native(); portability(); recovery(); literals_and_containment(); std::cout<<"Native sample import, playback, catalogue, package/DAW portability, collision handling and containment passed.\n"; }
    catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
