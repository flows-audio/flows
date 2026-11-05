#include "host/app/FlowsProcessor.hpp"
#include "app/AIEndpoint.hpp"
#include "app/BundledExamples.hpp"
#include "app/Text.hpp"

#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
using namespace juce;
using namespace flows::app;
using Clock = std::chrono::steady_clock;
void check(bool condition,const std::string& message){if(!condition)throw std::runtime_error(message);}
AISecrets memorySecrets() { return memoryAISecrets(true); }
std::string sha(const MemoryBlock& bytes) { return flows::text::sha256Hex(std::string_view(static_cast<const char*>(bytes.getData()), bytes.getSize())); }
std::filesystem::path aiConfigDirectory() { return SystemStats::getEnvironmentVariable("FLOWS_AI_CONFIG_DIR",{}).toStdString(); }
void global_model_tests() {
    auto secrets=memorySecrets();AIModels first(aiConfigDirectory(),secrets),second(aiConfigDirectory(),secrets);
    first.save(flows::json::object({{"name","First"},{"endpoint","https://one.invalid/v1"},{"model","one"},{"apiKey","model-test-key"}}));
    const auto id=flows::json::str(first.snapshot(),"selectedId");
    check(second.connection().key=="model-test-key","Global models did not survive a store recreation.");
    second.save(flows::json::object({{"endpoint","https://two.invalid/v1"},{"model","two"},{"apiKey","other-test-key"}}));
    check(first.snapshot()["profiles"].size()==2,"Adding a model overwrote the first profile.");
    first.select(id);check(second.connection().model=="one","Selection was not shared between instances.");
    first.save(flows::json::object({{"id",id},{"endpoint","https://one.invalid/v1"},{"model","renamed"}}));
    check(second.connection().key=="model-test-key","Editing a model lost its saved key.");
    first.save(flows::json::object({{"id",id},{"endpoint","https://different.invalid/v1"},{"model","renamed"}}));
    check(second.connection().key.empty(),"Changing endpoints reused another provider's key.");
    check(flows::json::stringify(first.snapshot()).find("test-key")==std::string::npos,"Public model metadata exposed a key.");
    const auto file=File(SystemStats::getEnvironmentVariable("FLOWS_AI_CONFIG_DIR",{})).getChildFile("ai-models.json");
    check(!file.loadFileAsString().contains("test-key"),"Plaintext key appeared in model configuration.");
    AISecrets denied;denied.read=[](const std::string&){return std::string();};denied.write=[](const std::string&,const std::string&){throw std::runtime_error("Keychain denied");};denied.remove=[](const std::string&){};
    AIModels broken(aiConfigDirectory(),denied);const auto before=file.loadFileAsString();bool failed=false;
    try{broken.save(flows::json::object({{"endpoint","https://denied.invalid"},{"model","denied"},{"apiKey","new-test-key"}}));}catch(...){failed=true;}
    check(failed && file.loadFileAsString()==before,"Credential failure damaged saved model settings.");
    first.remove(id);check(second.snapshot()["profiles"].size()==1,"Removing a model was not global.");
    second.remove(flows::json::str(second.snapshot(),"selectedId"));check(first.connection().model.empty(),"Last model removal retained a stale connection.");
}
const String code=String::fromUTF8("# 音色 🎹 é\nimport standard as std\nfrom lib.osc import ClassicSine\nInst.sigstream(std.event.keys.poly()) as key:\n    osc = ClassicSine.make()\n    raw = osc(pitch: key.pitch, level: -6db)\n    return raw * key.gate * key.velocity\n");
var document(FlowsProcessor& p){return get(p.snapshot(),"document");}
std::uint64_t revision(FlowsProcessor& p){return static_cast<std::uint64_t>(static_cast<int64>(get(document(p),"revision")));}
String text(FlowsProcessor& p){return str(document(p),"text");}
var apply(FlowsProcessor& p,const String& source,const String& gesture={},const String& phase={}){
    auto d=document(p);return p.request("document.apply",object({{"documentId",str(d,"id")},{"baseRevision",get(d,"revision")},{"text",source},{"gestureId",gesture},{"gesturePhase",phase}}));
}
double audio(FlowsProcessor& p,MidiBuffer midi={}){
    AudioBuffer<float> buffer(2,128);for(int c=0;c<2;++c)for(int i=0;i<128;++i)buffer.setSample(c,i,std::numeric_limits<float>::quiet_NaN());
    p.processBlock(buffer,midi);double peak=0;
    for(int c=0;c<2;++c)for(int i=0;i<128;++i){auto value=buffer.getSample(c,i);check(std::isfinite(value),"Processor audio must be finite on every channel.");check(value==buffer.getSample(0,i),"Mono engine must feed both stereo channels consistently.");peak=std::max(peak,std::abs(static_cast<double>(value)));}
    return peak;
}
template<class Predicate> void until(FlowsProcessor& p,Predicate predicate,const std::string& message,double timeout=10){
    const auto deadline=Clock::now()+std::chrono::duration<double>(timeout);
    while(!predicate()){if(Clock::now()>deadline)throw std::runtime_error(message);audio(p);std::this_thread::sleep_for(std::chrono::milliseconds(5));}
}
void ready(FlowsProcessor& p){auto wanted=revision(p);until(p,[&]{auto snapshot=p.snapshot();return str(get(snapshot,"compile"),"status")=="ready"&&static_cast<std::uint64_t>(static_cast<int64>(get(get(snapshot,"audio"),"programRevision")))==wanted;},"Source did not compile/install.");}
template<class Work> void rejects(Work work,const std::string& message){bool rejected=false;try{work();}catch(const std::exception&){rejected=true;}check(rejected,message);}
var findGraphNode(const var& graph,const String& signal) {
    auto nodes=get(graph,"nodes");
    if(auto* values=nodes.getArray()) for(const auto& node:*values) if(str(node,"signal")==signal)return node;
    throw std::runtime_error("Missing graph signal: "+signal.toStdString());
}
bool graphHasEdge(const var& graph,const String& source,const String& target,const String& label={}) {
    auto edges=get(graph,"edges");
    if(auto* values=edges.getArray()) for(const auto& edge:*values)
        if(str(edge,"source")==source&&str(edge,"target")==target&&(label.isEmpty()||str(edge,"label")==label))return true;
    return false;
}
void graphIsDAG(const var& graph) {
    std::map<String,int> incoming;std::map<String,std::vector<String>> outgoing;
    auto nodes=get(graph,"nodes"),edges=get(graph,"edges");
    check(nodes.isArray()&&edges.isArray(),"Graph must provide node and edge arrays.");
    for(const auto& node:*nodes.getArray())check(incoming.emplace(str(node,"id"),0).second,"Graph node IDs must be unique.");
    std::set<String> edgeIds;
    for(const auto& edge:*edges.getArray()) {
        const auto source=str(edge,"source"),target=str(edge,"target");
        check(incoming.contains(source)&&incoming.contains(target)&&source!=target,"Graph contains a dangling edge or self-loop.");
        check(edgeIds.insert(str(edge,"id")).second,"Graph edge IDs must be unique.");
        ++incoming[target];outgoing[source].push_back(target);
    }
    std::vector<String> ready;for(const auto& [id,count]:incoming)if(count==0)ready.push_back(id);
    std::size_t visited=0;while(!ready.empty()){auto id=ready.back();ready.pop_back();++visited;for(const auto& target:outgoing[id])if(--incoming[target]==0)ready.push_back(target);}
    check(visited==incoming.size(),"Signal graph has a cycle.");
}
void graph_tests(FlowsProcessor& p) {
    apply(p,code);ready(p);
    auto compiled=get(p.snapshot(),"compile"),graph=get(compiled,"graph");graphIsDAG(graph);
    const auto raw=findGraphNode(graph,"raw");check(str(raw,"op")=="sine"&&str(raw,"kind")=="audio","Stateful output was not identified from typed IR.");
    check(str(graph,"output")=="signal:$output"&&graphHasEdge(graph,"signal:raw","signal:$output"),"Output is not connected to the signal.");
    const std::u16string utf16(reinterpret_cast<const char16_t*>(code.toUTF16().getAddress()));
    const auto begin=static_cast<std::size_t>(static_cast<int>(get(raw,"from"))),end=static_cast<std::size_t>(static_cast<int>(get(raw,"to")));
    check(end<=utf16.size()&&utf16.substr(begin,end-begin)==u"raw","Graph spans are not UTF-16 aligned after Chinese and emoji.");
    auto rawParameters=get(raw,"parameterIds");check(rawParameters.isArray()&&rawParameters.getArray()->size()==1,"Sine level knob ownership is incorrect.");
    auto parameters=get(compiled,"parameters");bool level=false;
    for(const auto& parameter:*parameters.getArray())if(get(parameter,"id")==(*rawParameters.getArray())[0])level=str(parameter,"name")=="level";
    check(level,"Parameter metadata is missing the registered knob name.");
    const auto rawId=str(raw,"id");apply(p,code.replace("-6db","-12db"));ready(p);
    check(str(findGraphNode(get(get(p.snapshot(),"compile"),"graph"),"raw"),"id")==rawId,"A parameter edit changed semantic node identity.");

    const auto rolling=File(__FILE__).getParentDirectory().getParentDirectory().getChildFile("presets/trance-rolling-bass.flows").loadFileAsString();
    check(rolling.isNotEmpty(),"Rolling bass fixture unavailable.");
    const auto observed=rolling.replace("    return (mid + sub)","    p_mid = probe(mid_filtered)\n    p_raw = probe(mid_raw)\n    difference = p_mid - p_raw\n    energy = power(difference)\n    return (mid + sub)");
    apply(p,observed);ready(p);compiled=get(p.snapshot(),"compile");graph=get(compiled,"graph");graphIsDAG(graph);
    check(graphHasEdge(graph,"signal:mid_raw","signal:mid_high","input")&&graphHasEdge(graph,"signal:mid_high","signal:mid_filtered","input"),"Rolling bass audio stages were flattened.");
    check(graphHasEdge(graph,"signal:filter_env","signal:cutoff")&&graphHasEdge(graph,"signal:cutoff","signal:mid_filtered","cutoff"),"Filter cutoff modulation is absent.");
    check(!graphHasEdge(graph,"signal:filter_env","signal:mid_filtered"),"Graph connected a transitive ancestor instead of the closest named signal.");
    check(graphHasEdge(graph,"signal:mid","signal:$output")&&graphHasEdge(graph,"signal:sub","signal:$output"),"Mid and sub layers do not reach output.");
    check(str(findGraphNode(graph,"mid_filtered"),"op")=="filter_lowpass","Filter operation metadata is incorrect.");
    auto direct=get(findGraphNode(graph,"mid_filtered"),"parameterIds");check(direct.isArray()&&direct.getArray()->size()==1,"Filter borrowed parameters from cutoff or upstream audio.");
    parameters=get(compiled,"parameters");bool resonance=false;
    for(const auto& parameter:*parameters.getArray())if(get(parameter,"id")==(*direct.getArray())[0])resonance=str(parameter,"name")=="resonance";
    check(resonance,"The filter must own only its directly authored resonance knob.");
    check(get(findGraphNode(graph,"cutoff"),"parameterIds").getArray()->size()==2,"Cutoff must own both frequency literals.");
    check(get(findGraphNode(graph,"mid_raw"),"parameterIds").getArray()->size()==3,"Oscillator lost its compile-time unison or octave literal.");
    check(get(findGraphNode(graph,"mid"),"parameterIds").getArray()->isEmpty(),"Mix expression borrowed upstream knobs.");
    check(str(findGraphNode(graph,"difference"),"kind")=="analysis"&&graphHasEdge(graph,"analysis:p_mid","analysis:difference")&&graphHasEdge(graph,"analysis:difference","analysis:energy"),"Analysis graph dependencies were not preserved.");
    check(graphHasEdge(graph,"signal:mid_filtered","analysis:p_mid","probe"),"Probe is disconnected from its measured DSP signal.");

    const auto aliases=code.replace("    return raw", "    copied = raw\n    again = copied\n    return again");
    apply(p,aliases);ready(p);graph=get(get(p.snapshot(),"compile"),"graph");graphIsDAG(graph);
    check(str(findGraphNode(graph,"copied"),"op")=="alias"&&graphHasEdge(graph,"signal:raw","signal:copied","alias"),"Shared ValueId alias lost its canonical computation.");
    check(get(findGraphNode(graph,"again"),"parameterIds").getArray()->isEmpty(),"Alias duplicated its source parameter knobs.");
    const String classProbe="import standard as std\nfrom lib.osc import ClassicSine\nclass Tap:\n    def signal(input: scalar):\n        observed = probe(input)\n        return input * 0.5\nInst.sigstream(std.event.keys.poly()) as key:\n    osc = ClassicSine.make()\n    tap = Tap.make()\n    raw = osc(pitch: key.pitch)\n    shaped = raw * 0.8\n    processed = tap(input: shaped * 2)\n    return processed * key.gate\n";
    apply(p,classProbe);ready(p);graph=get(get(p.snapshot(),"compile"),"graph");graphIsDAG(graph);
    check(graphHasEdge(graph,"signal:shaped","analysis:tap.observed","probe"),"Probe on a class port lost its unnamed caller expression.");
    check(!graphHasEdge(graph,"signal:raw","analysis:tap.observed"),"Class port probe ignored the nearest named DSP ancestor.");
    check(get(findGraphNode(graph,"tap.observed"),"parameterIds").getArray()->isEmpty(),"Analysis probe borrowed DSP source knobs.");
    apply(p,"invalid graph draft\n");until(p,[&]{return str(get(p.snapshot(),"compile"),"status")=="error";},"Bad graph draft was not diagnosed.");
    graph=get(get(p.snapshot(),"compile"),"graph");check(get(graph,"nodes").getArray()->isEmpty()&&get(graph,"edges").getArray()->isEmpty()&&str(graph,"output").isEmpty(),"Failed compile exposed stale graph source bindings.");
    apply(p,code);ready(p);
}
void choice_tests(FlowsProcessor& p) {
    const String source=String::fromUTF8(
        "# Dropdown 音色 🎛️ é\nimport standard as std\nfrom lib.osc import ClassicSine\nfrom lib.filt import ClassicFilt\nfrom lib.mod import ClassicLFO\n"
        "class Tone:\n    def signal(input: audio):\n        filt = ClassicFilt.make()\n        filtered = filt(input, mode: lowpass, cutoff: 900hz)\n        return filtered\n"
        "class Layer:\n    def signal(input: audio):\n        tone = Tone.make()\n        shaped = tone(input: input)\n        return shaped\n"
        "Inst.sigstream(std.event.keys.poly()) as key:\n    osc = ClassicSine.make()\n    lfo = ClassicLFO.make()\n    one = Layer.make()\n    two = Layer.make()\n"
        "    raw = osc(pitch: key.pitch)\n    motion = lfo(rate: 2hz)\n    first = one(input: raw)\n    second = two(input: raw)\n    mixed = (first + second) * (0.5 + motion * 0.1)\n    return mixed * key.gate\n");
    auto utf16=[](const String& value){return std::u16string(reinterpret_cast<const char16_t*>(value.toUTF16().getAddress()));};
    auto options=[](const var& choice,const String& value) {
        const auto entries=get(choice,"options");
        if(const auto* list=entries.getArray())for(const auto& item:*list)if(str(item,"value")==value)return item;
        throw std::runtime_error("Missing choice option: "+value.toStdString());
    };
    auto forNode=[](const var& compiled,const String& signal,const String& name) {
        const auto ids=get(findGraphNode(get(compiled,"graph"),signal),"choiceIds"),choices=get(compiled,"choices");
        if(const auto* list=choices.getArray())for(const auto& item:*list)
            if(str(item,"name")==name&&ids.getArray()&&ids.getArray()->contains(get(item,"id")))return item;
        throw std::runtime_error("Missing choice for "+signal.toStdString()+"."+name.toStdString());
    };
    auto replaceChoice=[&](const var& choice,const String& value) {
        auto edited=utf16(text(p));const auto from=static_cast<std::size_t>(static_cast<int>(get(choice,"from"))),to=static_cast<std::size_t>(static_cast<int>(get(choice,"to")));
        check(from<=to&&to<=edited.size(),"Choice edit has invalid UTF-16 source bounds");
        edited.replace(from,to-from,utf16(str(choice,"prefix")+value+str(choice,"suffix")));
        return String(CharPointer_UTF16(reinterpret_cast<const CharPointer_UTF16::CharType*>(edited.c_str())));
    };
    apply(p,source);ready(p);
    auto compiled=get(p.snapshot(),"compile"),choices=get(compiled,"choices");
    check(choices.isArray()&&!choices.getArray()->isEmpty(),"Compiler choices were not exposed by the native bridge");
    std::set<String> ids;const auto originalUtf16=utf16(source);
    for(const auto& item:*choices.getArray()) {
        check(str(item,"id").isNotEmpty()&&ids.insert(str(item,"id")).second,"Choice binding IDs must be nonempty and unique");
        const auto from=static_cast<int>(get(item,"from")),to=static_cast<int>(get(item,"to"));
        check(from>=0&&to>=from&&static_cast<std::size_t>(to)<=originalUtf16.size(),"Choice spans are not valid UTF-16 offsets");
        check(get(item,"implicit").isBool()&&get(item,"prefix").isString()&&get(item,"suffix").isString(),"Choice insertion recipe schema is incomplete");
        if(static_cast<bool>(get(item,"implicit")))check(from==to,"Implicit choice must insert at an empty source range");
        const auto entries=get(item,"options");check(entries.isArray()&&!entries.getArray()->isEmpty(),"Choice has no selectable options");
        for(const auto& option:*entries.getArray())check(get(option,"enabled").isBool()&&get(option,"reason").isString()&&str(option,"value").isNotEmpty(),"Choice dependency schema is incomplete");
    }
    const auto graph=get(compiled,"graph");std::vector<String> filterNames;
    for(const auto& node:*get(graph,"nodes").getArray())if(str(node,"signal").endsWith(".filtered"))filterNames.push_back(str(node,"signal"));
    check(filterNames.size()==2,"Nested class signal nodes were not expanded independently");
    const auto firstMode=forNode(compiled,filterNames[0],"mode"),secondMode=forNode(compiled,filterNames[1],"mode");
    check(str(firstMode,"id")!=str(secondMode,"id")&&get(firstMode,"from")==get(secondMode,"from")&&get(firstMode,"to")==get(secondMode,"to"),"Shared class source must have a distinct binding on each owning graph node");
    const auto modeFrom=static_cast<int>(get(firstMode,"from")),modeTo=static_cast<int>(get(firstMode,"to"));
    check(originalUtf16.substr(modeFrom,modeTo-modeFrom)==u"lowpass","Choice UTF-8 span was not mapped past Chinese/emoji/combining characters");
    for(const auto* name:{"first","second","mixed","$output"})check(get(findGraphNode(graph,name),"choiceIds").getArray()->isEmpty(),"Downstream signal borrowed a nested class choice");
    check(str(forNode(compiled,"key.pitch","mode"),"value")=="poly","Key policy choice does not belong to key.pitch");
    const auto mode=forNode(compiled,"motion","mode");
    for(const auto* value:{"retrigger","one_shot","transport"}) {
        const auto option=options(mode,value);check(!static_cast<bool>(get(option,"enabled"))&&str(option,"reason").isNotEmpty(),"LFO dependency was not disabled with an explanation");
    }
    check(static_cast<bool>(get(options(firstMode,"highpass"),"enabled")),"Legal filter choice was disabled");
    const auto base=revision(p);apply(p,replaceChoice(firstMode,"highpass"));ready(p);
    check(revision(p)==base+1,"One choice replacement generated multiple document revisions");
    compiled=get(p.snapshot(),"compile");for(const auto& name:filterNames)check(str(forNode(compiled,name,"mode"),"value")=="highpass","Shared class choice edit did not reach both instances");
    p.request("document.undo",object({}));ready(p);check(text(p)==source,"Choice replacement was not a single undo step");
    auto shape=forNode(get(p.snapshot(),"compile"),"motion","shape");check(static_cast<bool>(get(shape,"implicit")),"Omitted LFO shape lacks its default insertion recipe");
    apply(p,replaceChoice(shape,"triangle"));ready(p);shape=forNode(get(p.snapshot(),"compile"),"motion","shape");
    check(str(shape,"value")=="triangle"&&!static_cast<bool>(get(shape,"implicit")),"Implicit choice did not insert a valid explicit argument");
    p.request("document.undo",object({}));ready(p);check(text(p)==source,"Implicit choice insertion was not a single undo step");
    apply(p,replaceChoice(forNode(get(p.snapshot(),"compile"),"motion","mode"),"transport"));
    until(p,[&]{return str(get(p.snapshot(),"compile"),"status")=="error";},"Invalid dependent choice did not receive a compiler diagnostic");
    compiled=get(p.snapshot(),"compile");check(get(compiled,"choices").getArray()->isEmpty()&&get(get(compiled,"graph"),"nodes").getArray()->isEmpty(),"Invalid draft retained stale clickable choice locations");
    p.request("document.undo",object({}));ready(p);check(text(p)==source,"Undo did not restore choices after an invalid draft");
    const auto direct=code.replace("from lib.osc import ClassicSine\n","from lib.osc import ClassicSine\nfrom lib.filt import ClassicFilt\n").replace("    raw = osc(","    filt = ClassicFilt.make()\n    raw = osc(").replace("    return raw *","    return filt(raw, mode: lowpass) *");
    apply(p,direct);ready(p);compiled=get(p.snapshot(),"compile");
    check(str(forNode(compiled,"$output","mode"),"value")=="lowpass","A return expression choice was not attached to the Output graph node");
    check(get(findGraphNode(get(compiled,"graph"),"raw"),"choiceIds").getArray()->isEmpty(),"Output choice was copied upstream to its oscillator");
    apply(p,code);ready(p);
}
void silentTransport(FlowsProcessor& p) {
    auto state=p.snapshot();auto pattern=get(state,"sequencer").clone();
    for(auto& row:*get(pattern,"rows").getArray())row.getDynamicObject()->setProperty("mute",true);
    p.request("sequencer.set",object({{"baseRevision",get(pattern,"revision")},{"pattern",pattern}}));
    p.request("sequencer.play",object({{"playing",true}}));
}
void probe_tests(FlowsProcessor& p) {
    const auto previousPattern=get(p.snapshot(),"sequencer").clone();
    auto signal=[](const var& values,const String& name) {
        if (const auto* list=values.getArray()) for (const auto& entry:*list) if(str(entry,"name")==name)return entry;
        return var{};
    };
    String source="import standard as std\nfrom lib.osc import ClassicSine\nclass Tap:\n    def signal(input: scalar):\n        observed = probe(input)\n        return input * 0.5\nInst.sigstream(std.event.keys.poly()) as key:\n    osc = ClassicSine.make()\n    tap = Tap.make()\n    raw = osc(pitch: key.pitch, level: 0.5)\n    processed = tap(input: raw * 2)\n";
    Array<var> aliases, roots;
    for(int i=0;i<9;++i) {
        source+="    p"+String(i)+" = probe(raw)\n    s"+String(i)+" = raw * 0.5\n";
        aliases.add("p"+String(i));roots.add("s"+String(i));
    }
    source+="    copied = raw\n    difference = p0 - p1\n    energy = power(p0)\n    control = probe(key.gate)\n    control_energy = power(control)\n    wide = p0";
    for(int i=0;i<65;++i)source+=" + 1";
    source+="\n    return processed * key.gate\n";
    apply(p,source);ready(p);
    const auto metadata=get(get(p.snapshot(),"compile"),"signals");
    const auto raw=signal(metadata,"raw"), alias=signal(metadata,"p0"), port=signal(metadata,"tap.observed");
    check(get(raw,"captureRoots")==get(alias,"captureRoots")&&static_cast<bool>(get(alias,"auditionable")),"Audio probe alias metadata does not share its source capture/audition.");
    check(static_cast<bool>(get(port,"auditionable"))&&get(port,"captureRoots")!=get(raw,"captureRoots"),"Class input probe metadata lost its exact anonymous root.");
    check(!static_cast<bool>(get(signal(metadata,"difference"),"auditionable"))&&str(signal(metadata,"energy"),"shape")=="scalar","Derived analysis metadata confused audition/shape.");
    check(static_cast<int>(get(get(p.snapshot(),"compile"),"voiceLimit"))==32,"Poly voice limit missing.");
    auto observe=[&](const Array<var>& names,var voice=var("mixed")){return p.request("probe.set",object({{"signals",names},{"voice",voice}}));};
    observe(aliases); const auto validSelection=JSON::toString(get(p.snapshot(),"observation"));
    check(get(get(p.snapshot(),"observation"),"signals").getArray()->size()==9,"Shared probe aliases incorrectly consumed nine capture roots.");
    rejects([&]{observe(roots);},"Nine real roots did not reject synchronously.");
    rejects([&]{observe(Array<var>{"missing"});},"Unknown probe silently accepted.");
    rejects([&]{observe(Array<var>{"wide"});},"Expanded analysis budget did not reject synchronously.");
    rejects([&]{observe(Array<var>{"p0"},var("latest"));},"Unknown voice string silently selected mixed capture.");
    check(JSON::toString(get(p.snapshot(),"observation"))==validSelection,"Rejected probe selection changed the current observation.");
    observe(Array<var>{"tap.observed","p0","energy","control_energy"},0);
    p.request("keyboard.panic",object({}));audio(p);silentTransport(p);
    p.request("keyboard.noteOn",object({{"noteId",9876},{"pitch",69},{"velocity",1.0}}));
    until(p,[&]{return static_cast<bool>(get(signal(get(p.telemetry(),"signals"),"energy"),"valid"));},"Selected probe analysis never acquired a continuous window.");
    const auto telemetry=p.telemetry();
    check(static_cast<int>(get(telemetry,"captureVoice"))==0,"Telemetry voice identity missing.");
    const auto observed=signal(get(telemetry,"signals"),"tap.observed");
    check(static_cast<bool>(get(observed,"valid"))&&get(observed,"spectrum").getArray()->size()>0,"Anonymous class probe did not capture waveform/spectrum.");
    check(std::abs(static_cast<double>(get(signal(get(telemetry,"signals"),"control_energy"),"scalar"))-1.0)<1e-6,"Selected voice control power is not valid gate power.");
    p.request("probe.listen",object({{"signal","tap.observed"},{"enabled",true}}));
    check(str(get(p.snapshot(),"audio"),"audition")=="tap.observed","Direct class audio probe could not be auditioned.");
    rejects([&]{p.request("probe.listen",object({{"signal","difference"},{"enabled",true}}));},"Derived analysis unexpectedly entered realtime audio.");
    check(str(get(p.snapshot(),"audio"),"audition")=="tap.observed","Rejected derived audition changed current listening.");
    observe(Array<var>{"raw"});
    until(p,[&]{const auto signals=get(p.telemetry(),"signals");return signals.isArray()&&signals.getArray()->size()==1&&str(signals.getArray()->getReference(0),"name")=="raw";},"Raw-only observation silently retained default authored probes.");
    observe(Array<var>{"copied"});
    until(p,[&]{return static_cast<bool>(get(signal(get(p.telemetry(),"signals"),"copied"),"valid"));},"Subscribed DSP alias never received its shared capture.");
    const auto shared=get(p.telemetry(),"signals");
    check(get(signal(shared,"copied"),"values")==get(signal(shared,"raw"),"values"),"DSP alias waveform differs from its captured source.");
    check(get(get(p.snapshot(),"observation"),"signals").getArray()->size()==1,"Alias selection grew into duplicate subscriptions.");
    p.request("keyboard.panic",object({}));audio(p);
    p.request("sequencer.set",object({{"baseRevision",get(get(p.snapshot(),"sequencer"),"revision")},{"pattern",previousPattern}}));
    apply(p,code);ready(p);
}
void stereo_host_tests(FlowsProcessor& p) {
    auto signal=[](const var& values,const String& name) { if(const auto* list=values.getArray())for(const auto& value:*list)if(str(value,"name")==name)return value;return var{}; };
    const auto fixture=File(__FILE__).getParentDirectory().getParentDirectory().getChildFile("presets/stereo-motion.flows").loadFileAsString();
    check(fixture.isNotEmpty(),"Stereo motion fixture missing");apply(p,fixture);ready(p);
    auto metadata=get(get(p.snapshot(),"compile"),"signals");
    check(static_cast<int>(get(get(p.snapshot(),"compile"),"outputChannels"))==2&&static_cast<int>(get(signal(metadata,"raw"),"channels"))==2,"Stereo output/unison channel metadata missing");
    check(str(signal(metadata,"motion"),"kind")=="control"&&!static_cast<bool>(get(signal(metadata,"motion"),"auditionable")),"LFO was exposed as an audio oscillator");
    const auto graph=get(get(p.snapshot(),"compile"),"graph");
    check(str(findGraphNode(graph,"raw"),"op")=="unison_saw"&&str(findGraphNode(graph,"shaped"),"op")=="distortion"&&str(findGraphNode(graph,"motion"),"kind")=="control","Signal flow lost new DSP kinds");
    Array<var> observed{"p_after","motion","amp"};
    p.request("probe.set",object({{"signals",observed},{"voice",0},{"projection","side"}}));
    rejects([&]{p.request("probe.set",object({{"signals",observed},{"voice",0},{"projection","surround"}}));},"Unknown audio projection accepted");
    p.request("keyboard.noteOn",object({{"noteId",731},{"pitch",57},{"velocity",1.0}}));
    AudioBuffer<float> buffer(2,128);MidiBuffer midi;
    double difference=0;
    for(int block=0;block<80;++block) {
        p.processBlock(buffer,midi);
        for(int i=0;i<128;++i){check(std::isfinite(buffer.getSample(0,i))&&std::isfinite(buffer.getSample(1,i)),"Stereo host emitted nonfinite audio");difference+=std::abs(buffer.getSample(0,i)-buffer.getSample(1,i));}
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    check(difference>.01,"Native host flattened unison/filter/distortion stereo output");
    check(str(p.telemetry(),"captureProjection")=="side"&&static_cast<bool>(get(signal(get(p.telemetry(),"signals"),"amp"),"valid"))&&static_cast<double>(get(signal(get(p.telemetry(),"signals"),"amp"),"rms"))>.1,"Side observation corrupted the mono envelope control");
    p.request("probe.listen",object({{"signal","p_after"},{"enabled",true}}));
    for(int block=0;block<8;++block){p.processBlock(buffer,midi);std::this_thread::sleep_for(std::chrono::milliseconds(2));}
    check(str(get(p.snapshot(),"audio"),"audition")=="p_after","Stereo probe alias could not be auditioned");
    p.request("keyboard.panic",object({}));p.processBlock(buffer,midi);
    const String clockSource="import standard as std\nfrom lib.mod import ClassicLFO\nInst.sigstream(std.event.keys.poly()) as key:\n    lfo = ClassicLFO.make()\n    motion = lfo(shape: sine, sync: 1/4, mode: transport, bipolar: true)\n    return stereo(motion, motion * 0.5) * key.gate\n";
    apply(p,clockSource);ready(p);
    struct PlayHead final:AudioPlayHead {
        double bpm=240,ppq=.25;int64 sample=0;bool playing=false;
        Optional<PositionInfo> getPosition()const override { PositionInfo info;info.setBpm(bpm);info.setPpqPosition(ppq);info.setTimeInSamples(sample);info.setIsPlaying(playing);return info; }
    } playHead;
    p.setPlayHead(&playHead);
    struct Detach { FlowsProcessor& processor;~Detach(){processor.setPlayHead(nullptr);} } detach{p};
    // ready() confirms installation; drain the documented 20 ms structural
    // crossfade in silence before measuring the host's exact PPQ waveform.
    buffer.setSize(2,2048);p.processBlock(buffer,midi);
    p.request("keyboard.noteOn",object({{"noteId",732},{"pitch",69},{"velocity",1.0}}));
    p.processBlock(buffer,midi);
    for(int i=48;i<2048;++i)check(std::abs(buffer.getSample(0,i)-1)<2e-5&&std::abs(buffer.getSample(1,i)-.5)<2e-5,"Stopped host PPQ did not freeze transport LFO on both channels");
    playHead.playing=true;playHead.ppq=.125;playHead.sample=2048;p.processBlock(buffer,midi);
    for(int i=0;i<2048;++i){const auto expected=std::sin(2*3.14159265358979323846*(.125+i*240.0/(60*48000)));check(std::abs(buffer.getSample(0,i)-expected)<2e-5&&std::abs(buffer.getSample(1,i)-expected*.5)<2e-5,"Host PPQ/tempo lost its sample offset across native 1024-frame chunks");}
    check(str(get(p.snapshot(),"audio"),"tempoSource")=="host"&&static_cast<double>(get(get(p.snapshot(),"audio"),"bpm"))==240,"Host tempo source metadata missing");
    p.request("transport.set",object({{"bpm",150}}));check(static_cast<double>(get(get(p.snapshot(),"preferences"),"bpm"))==150,"Local tempo did not persist under host override");
    rejects([&]{p.request("transport.set",object({{"bpm",0}}));},"Invalid tempo accepted");
    p.setPlayHead(nullptr);p.request("keyboard.panic",object({}));p.processBlock(buffer,midi);
    check(str(get(p.snapshot(),"audio"),"tempoSource")=="internal"&&static_cast<double>(get(get(p.snapshot(),"audio"),"bpm"))==150,"Native tempo did not resume after host detach");
    p.request("transport.set",object({{"bpm",138}}));apply(p,code);ready(p);
}
void standalone_transport_tests() {
    FlowsProcessor p(true);
    p.prepareToPlay(48000,128);
    apply(p,code);ready(p);
    const auto probeFixture = File(__FILE__).getParentDirectory().getParentDirectory().getParentDirectory()
        .getChildFile("examples/ADCxGather/04-probe/01-filter-before-after.flows");
    check(probeFixture.existsAsFile(), "Missing filter/probe idle fixture.");
    apply(p, probeFixture.loadFileAsString()); ready(p);
    // A stopped standalone must send exact digital silence, including when
    // its metronome is armed. Check multiple beats and callback boundaries;
    // a single quiet block would miss a periodic click.
    auto sequence = get(p.snapshot(), "sequencer").clone();
    sequence.getDynamicObject()->setProperty("metronome", object({{"mute", false}, {"division", 16}, {"volume", 0.0}}));
    p.request("sequencer.set", object({{"documentId", str(document(p), "id")},
        {"baseRevision", get(sequence, "revision")}, {"pattern", sequence}}));
    for (const int frames : {64, 512, 2048}) {
        AudioBuffer<float> idle(2, frames); MidiBuffer noNotes;
        for (int block = 0; block < 48000 * 4 / frames; ++block) {
            for (int channel = 0; channel < 2; ++channel)
                std::fill_n(idle.getWritePointer(channel), frames, std::numeric_limits<float>::quiet_NaN());
            p.processBlock(idle, noNotes);
            for (int channel = 0; channel < 2; ++channel)
                for (int sample = 0; sample < frames; ++sample)
                    check(idle.getSample(channel, sample) == 0.0f, "Stopped standalone emitted audio with no key held.");
        }
    }
    sequence.getDynamicObject()->setProperty("metronome", object({{"mute", true}, {"division", 4}, {"volume", -12.0}}));
    sequence.getDynamicObject()->setProperty("revision", get(get(p.snapshot(), "sequencer"), "revision"));
    p.request("sequencer.set", object({{"documentId", str(document(p), "id")},
        {"baseRevision", get(sequence, "revision")}, {"pattern", sequence}}));
    apply(p, code); ready(p);
    // Match the minimal device-clock playhead injected by JUCE's
    // AudioProcessorPlayer: times are present, transport fields are absent.
    struct DevicePlayHead final:AudioPlayHead {
        int64 sample=0;
        Optional<PositionInfo> getPosition()const override {
            PositionInfo info;info.setTimeInSamples(sample);info.setTimeInSeconds(double(sample)/48000);return info;
        }
    } playHead;
    p.setPlayHead(&playHead);
    struct Detach { FlowsProcessor& processor;~Detach(){processor.setPlayHead(nullptr);} } detach{p};
    p.request("transport.set",object({{"bpm",138}}));
    const auto started=p.request("audio.playDemo",object({}));
    check(static_cast<bool>(get(get(started,"audio"),"demoRunning")),"Standalone pattern toggle was not acknowledged");
    double peak=0;
    for(int block=0;block<200;++block){peak=std::max(peak,audio(p));playHead.sample+=128;}
    check(peak>.01,"Standalone device-clock playhead stopped the demo pattern");
    check(str(get(p.snapshot(),"audio"),"tempoSource")=="internal","Standalone device clock was mistaken for DAW tempo");
    const auto stopped=p.request("audio.playDemo",object({}));
    check(!static_cast<bool>(get(get(stopped,"audio"),"demoRunning")),"Standalone pattern did not stop");
    for(int block=0;block<8;++block){audio(p);playHead.sample+=128;}
    check(static_cast<int>(get(get(p.snapshot(),"audio"),"voices"))==0,"Stopping standalone pattern left a held note");

    const String clockSource="import standard as std\nfrom lib.mod import ClassicLFO\nInst.sigstream(std.event.keys.poly()) as key:\n    lfo = ClassicLFO.make()\n    motion = lfo(shape: sine, sync: 1/4, mode: transport, bipolar: true)\n    return motion * key.gate\n";
    apply(p,clockSource);ready(p);
    // Drain the structural crossfade before measuring a sustained control.
    for(int block=0;block<10;++block){audio(p);playHead.sample+=128;}
    p.request("keyboard.noteOn",object({{"noteId",733},{"pitch",69},{"velocity",1.0}}));
    AudioBuffer<float> buffer(2,128);MidiBuffer midi;
    double low=1,high=-1;
    for(int block=0;block<80;++block) {
        p.processBlock(buffer,midi);playHead.sample+=128;
        for(int i=0;i<128;++i) {
            const auto value=buffer.getSample(0,i);check(std::isfinite(value),"Standalone transport LFO emitted nonfinite audio");
            low=std::min(low,double(value));high=std::max(high,double(value));
        }
    }
    check(high-low>.5,"Standalone transport LFO did not advance under the device-clock playhead");
    p.request("keyboard.panic",object({}));audio(p);p.releaseResources();
}
void preference_tests(FlowsProcessor& p) {
    auto prefs=get(p.snapshot(),"preferences");
    const auto visible = [](const var& preferences, const char* key) {
        const auto value=get(preferences,key);
        check(value.isBool(),std::string("Panel visibility must be boolean: ")+key);
        return static_cast<bool>(value);
    };
    const auto allVisible = [&](const var& preferences) {
        for (const auto* key : {"showExplorer","showAssistant","showDataflow","showKeyboard"})
            check(visible(preferences,key),std::string("Missing visibility must default to shown: ")+key);
    };
    allVisible(prefs);
    check(static_cast<int>(get(prefs,"fontSize"))==16&&static_cast<int>(get(prefs,"layoutVersion"))==2,"New workspace did not receive the readable version 2 defaults.");
    check(static_cast<int>(get(prefs,"leftWidth"))==240&&static_cast<int>(get(prefs,"flowHeight"))==320,"Native layout defaults disagree with the editor.");
    p.request("preferences.set",object({{"fontSize",28},{"theme","paper"},{"leftWidth",300},{"rightWidth",420},{"flowHeight",400},{"splitRatio",.65},
        {"showExplorer",false},{"showAssistant",false},{"showDataflow",false},{"showKeyboard",false}}));
    p.request("preferences.set",object({{"opacity",.8}}));prefs=get(p.snapshot(),"preferences");
    check(static_cast<int>(get(prefs,"fontSize"))==28&&str(prefs,"theme")=="paper"&&static_cast<int>(get(prefs,"leftWidth"))==300&&static_cast<int>(get(prefs,"flowHeight"))==400,"Partial preferences update reset saved typography or layout.");
    check(std::abs(static_cast<double>(get(prefs,"splitRatio"))-.65)<1e-9,"Partial preferences update reset the split ratio.");
    for (const auto* key : {"showExplorer","showAssistant","showDataflow","showKeyboard"})
        check(!visible(prefs,key),std::string("Partial preferences update lost hidden panel: ")+key);
    p.request("preferences.set",object({{"showExplorer",true}}));
    p.request("preferences.set",object({{"showExplorer",0},{"showAssistant","true"},{"showDataflow",1},{"showKeyboard",var()}}));
    prefs=get(p.snapshot(),"preferences");
    check(visible(prefs,"showExplorer")&&!visible(prefs,"showAssistant")&&!visible(prefs,"showDataflow")&&!visible(prefs,"showKeyboard"),
        "Invalid visibility values overwrote boolean preferences or changed other panels.");
    auto settings=parseJSONBounded(File(SystemStats::getEnvironmentVariable("FLOWS_WORKSPACE",{})).getChildFile(".flows-settings.json").loadFileAsString());
    check(JSON::toString(get(settings,"preferences"))==JSON::toString(prefs),"Layout was not persisted to workspace settings.");
    {
        FlowsProcessor restored;
        check(JSON::toString(get(restored.snapshot(),"preferences"))==JSON::toString(prefs),"Reopening a workspace lost saved panel visibility.");
    }
    p.request("preferences.set",object({{"fontSize",99},{"leftWidth",9999},{"rightWidth",9999},{"flowHeight",9999},{"splitRatio",0}}));prefs=get(p.snapshot(),"preferences");
    check(static_cast<int>(get(prefs,"fontSize"))==32&&static_cast<int>(get(prefs,"leftWidth"))==520&&static_cast<int>(get(prefs,"rightWidth"))==600&&static_cast<int>(get(prefs,"flowHeight"))==800&&static_cast<double>(get(prefs,"splitRatio"))==.2,"Preferences bounds were not applied.");
    p.request("preferences.set",object({{"leftWidth",120},{"rightWidth",180},{"flowHeight",80}}));
    p.request("preferences.set",object({{"theme","graphite"}}));prefs=get(p.snapshot(),"preferences");
    check(static_cast<int>(get(prefs,"leftWidth"))==120&&static_cast<int>(get(prefs,"rightWidth"))==180&&static_cast<int>(get(prefs,"flowHeight"))==80,"Small visible panels snapped back to larger native defaults.");
    auto narrowed=parseJSONBounded(p.request("project.exportText",object({})).toString());
    check(visible(get(narrowed,"preferences"),"showExplorer")&&!visible(get(narrowed,"preferences"),"showDataflow"),"Instrument export lost panel visibility.");
    p.request("preferences.set",object({{"showExplorer",false},{"showAssistant",true},{"showDataflow",true},{"showKeyboard",true}}));
    p.request("project.importText",object({{"text",JSON::toString(narrowed)}}));ready(p);prefs=get(p.snapshot(),"preferences");
    check(static_cast<int>(get(prefs,"leftWidth"))==120&&static_cast<int>(get(prefs,"rightWidth"))==180&&static_cast<int>(get(prefs,"flowHeight"))==80,"Restoring an instrument changed saved small-panel dimensions.");
    check(visible(prefs,"showExplorer")&&!visible(prefs,"showAssistant")&&!visible(prefs,"showDataflow")&&!visible(prefs,"showKeyboard"),"Instrument import did not restore panel visibility.");
    auto portable=parseJSONBounded(p.request("project.exportText",object({})).toString());portable.getDynamicObject()->setProperty("preferences",object({{"fontSize",12},{"theme","graphite"}}));
    p.request("project.importText",object({{"text",JSON::toString(portable)}}));ready(p);prefs=get(p.snapshot(),"preferences");
    check(static_cast<int>(get(prefs,"layoutVersion"))==1&&static_cast<int>(get(prefs,"fontSize"))==12,"Legacy preferences lost their migration marker.");
    allVisible(prefs);
    p.request("preferences.set",object({{"fontSize",16},{"layoutVersion",2}}));p.request("preferences.set",object({{"rightWidth",360}}));prefs=get(p.snapshot(),"preferences");
    check(static_cast<int>(get(prefs,"fontSize"))==16&&static_cast<int>(get(prefs,"layoutVersion"))==2,"Layout migration did not survive a later partial update.");
}
void document_tests(FlowsProcessor& p){
    apply(p,code);ready(p);const auto base=revision(p);
    rejects([&]{p.request("document.apply",object({{"documentId",str(document(p),"id")},{"baseRevision",static_cast<int64>(base-1)},{"text",code.replace("-6db","-99db")}}));},"Stale document edit was accepted.");check(text(p)==code,"Conflicting edit changed the source.");
    const std::u16string utf16(reinterpret_cast<const char16_t*>(code.toUTF16().getAddress()));bool mapped=false;
    auto snapshot=p.snapshot();auto parameters=get(get(snapshot,"compile"),"parameters");if(auto* list=parameters.getArray())for(const auto& parameter:*list){if(str(parameter,"unit")!="decibels")continue;auto start=static_cast<std::size_t>(static_cast<int>(get(parameter,"from"))),end=static_cast<std::size_t>(static_cast<int>(get(parameter,"to")));check(start<=end&&end<=utf16.size(),"Native parameter UTF-16 bounds are invalid.");check(utf16.substr(start,end-start)==u"-6db","UTF-8 compiler spans were not converted around Chinese/emoji/combining text.");mapped=true;}check(mapped,"Expected draggable dB source binding.");
    apply(p,code.replace("-6db","-9db"),"drag-a");apply(p,code.replace("-6db","-12db"),"drag-a");apply(p,code.replace("-6db","-12db"),"drag-a","end");p.request("document.undo",object({}));check(text(p)==code,"One drag must be one undo step.");p.request("document.redo",object({}));check(text(p)==code.replace("-6db","-12db"),"Redo lost the drag result.");
    apply(p,code.replace("-6db","-15db"),"drag-cancel");apply(p,code.replace("-6db","-15db"),"drag-cancel","cancel");check(text(p)==code.replace("-6db","-12db"),"Esc did not restore the gesture start.");p.request("document.undo",object({}));check(text(p)==code,"Cancelled drag polluted undo history.");ready(p);
    p.request("document.save",object({}));auto saved=File(SystemStats::getEnvironmentVariable("FLOWS_WORKSPACE",{})).getChildFile(str(document(p),"path"));check(saved.loadFileAsString()==code,"Save did not persist actual source.");
}
void midi_tests(FlowsProcessor& p){
    apply(p,code);ready(p);
    p.request("keyboard.panic",object({}));audio(p);
    MidiBuffer on;on.addEvent(MidiMessage::noteOn(1,60,.5f),0);on.addEvent(MidiMessage::noteOn(1,60,.5f),16);audio(p,on);
    check(static_cast<int>(get(get(p.snapshot(),"audio"),"voices"))==2,"Same-pitch MIDI presses must retain two identities.");
    MidiBuffer off;off.addEvent(MidiMessage::noteOff(1,60),0);check(audio(p,off)>0,"Releasing one same-pitch note silenced both voices.");check(static_cast<int>(get(get(p.snapshot(),"audio"),"voices"))==1,"First note-off must release exactly one identity.");
    audio(p,off);check(static_cast<int>(get(get(p.snapshot(),"audio"),"voices"))==0,"Second note-off must release the remaining identity.");audio(p); /* Drain the 3 ms playback tail. */ for(int i=0;i<16;++i)check(audio(p)<1e-7,"Gated sine remained audible after matching note-offs.");
    auto voices=[&]{return static_cast<int>(get(get(p.snapshot(),"audio"),"voices"));};
    MidiBuffer channels;channels.addEvent(MidiMessage::noteOn(1,60,.5f),0);channels.addEvent(MidiMessage::noteOn(2,60,.5f),16);audio(p,channels);
    audio(p,off);check(voices()==1,"A note-off released another MIDI channel's note");
    MidiBuffer zero;zero.addEvent(MidiMessage::noteOn(2,60,static_cast<uint8>(0)),0);audio(p,zero);
    check(voices()==0,"Velocity-zero note-on must release its note");
    MidiBuffer wheels;wheels.addEvent(MidiMessage::pitchWheel(1,12000),0);wheels.addEvent(MidiMessage::controllerEvent(1,1,127),0);
    audio(p,wheels);check(voices()==0,"Unsupported MIDI controls must not start voices");
    auto peakAt=[&](float velocity){
        p.request("keyboard.panic",object({}));audio(p);
        MidiBuffer note;note.addEvent(MidiMessage::noteOn(1,69,velocity),0);
        double peak=audio(p,note);for(int i=0;i<8;++i)peak=std::max(peak,audio(p));
        p.releaseMidiNotes();audio(p);return peak;
    };
    const auto soft=peakAt(.25f),hard=peakAt(.75f);
    check(soft>0 && hard>soft*2.8 && hard<soft*3.2,"MIDI velocity must reach key.velocity");
    for(int i=0;i<8;++i)audio(p);
    p.request("keyboard.noteOn",object({{"noteId",9001},{"pitch",67},{"velocity",.6}}));audio(p);
    audio(p,channels);check(voices()==3,"Expected independent UI and MIDI voices");
    p.releaseMidiNotes();audio(p);check(voices()==1,"Disconnect must release MIDI notes without silencing UI keys");
    p.request("keyboard.noteOff",object({{"noteId",9001}}));audio(p);check(voices()==0,"UI release after MIDI disconnect failed");
    // A collected note-on racing with disconnect must not leave a stuck voice.
    p.releaseMidiNotes();audio(p,channels);check(voices()==0,"Queued MIDI note-on survived device disconnect");
}
void stopped_audio_macro_mapping_tests(FlowsProcessor& p){
    apply(p,code);ready(p);
    auto levelBinding=[&]{
        auto parameters=get(get(p.snapshot(),"compile"),"parameters");
        if(auto* list=parameters.getArray())for(const auto& parameter:*list)
            if(str(parameter,"unit")=="decibels")return str(parameter,"id");
        throw std::runtime_error("Expected an exportable level binding.");
    };
    const auto originalBinding=levelBinding();
    p.request("parameter.export",object({{"slot",1},{"bindingId",originalBinding}}));audio(p);
    auto* macro=p.getParameters()[0];
    auto* identified=dynamic_cast<AudioProcessorParameterWithID*>(macro);
    check(identified&&identified->paramID=="macro01","Host macro ID must remain fixed.");
    const auto playingRevision=get(get(p.snapshot(),"audio"),"programRevision");
    p.releaseResources();
    // Wait for every worker compile without calling processBlock. This deliberately
    // exceeds the old 16-map ring while keeping all publications unconsumed.
    for(int edit=0;edit<24;++edit){
        apply(p,code+"# stopped audio compile "+String(edit)+"\n");
        const auto wanted=revision(p);
        const auto deadline=Clock::now()+std::chrono::seconds(10);
        for(;;){
            const auto compile=get(p.snapshot(),"compile");
            if(static_cast<std::uint64_t>(static_cast<int64>(get(compile,"revision")))==wanted){
                check(str(compile,"status")=="ready","Stopped-device edit failed compilation.");break;
            }
            check(Clock::now()<deadline,"Stopped-device compile did not finish.");
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    check(get(get(p.snapshot(),"audio"),"programRevision")==playingRevision,
          "The stopped-device test accidentally consumed a prepared program.");
    p.prepareToPlay(48000,128);ready(p);
    // The saved source remains -6 dB. An actual -12 dB result proves that the
    // latest map routes fresh automation after resuming, not just its old overlay.
    macro->setValueNotifyingHost(.75f); // -120 + .75 * 144 = -12 dB.
    MidiBuffer note;note.addEvent(MidiMessage::noteOn(1,69,1.0f),0);
    double peak=audio(p,note);for(int i=0;i<12;++i)peak=std::max(peak,audio(p));
    check(std::abs(peak-std::pow(10.0,-12.0/20.0))<.0001,
          "Latest macro mapping was lost after more than 16 stopped-device compiles.");

    // Inserting an argument reuses raw.literal0 for octave. The old macro must
    // become unbound instead of silently driving that different semantic target.
    const auto changed=code.replace("pitch: key.pitch, level:","pitch: key.pitch, octave: 1, level:");
    apply(p,changed);ready(p);for(int i=0;i<8;++i)audio(p);
    auto exported=get(p.snapshot(),"exportedParameters");auto* slots=exported.getArray();
    check(slots&&!slots->isEmpty()&&str((*slots)[0],"bindingId")==originalBinding&&
          !static_cast<bool>(get((*slots)[0],"bound")),"A structure change silently rebound an old macro ID.");
    check(p.getParameters()[0]==macro&&identified->paramID=="macro01","Unbinding replaced the DAW's fixed macro parameter.");
    p.request("keyboard.panic",object({}));audio(p);macro->setValueNotifyingHost(.25f);
    AudioBuffer<float> samples(2,128);MidiBuffer fresh;fresh.addEvent(MidiMessage::noteOn(1,69,1.0f),0);
    p.processBlock(samples,fresh);
    for(int sample=48;sample<128;++sample){
        const auto expected=std::sin(2*3.14159265358979323846*880.0*sample/48000.0)*std::pow(10.0,-6.0/20.0);
        check(std::isfinite(samples.getSample(0,sample))&&std::abs(samples.getSample(0,sample)-expected)<.00002,
              "Unbound automation modified the new octave/level or lost the fresh voice.");
    }
    const auto newBinding=levelBinding();check(newBinding!=originalBinding,"Structure fixture did not move the level binding.");
    p.request("parameter.export",object({{"slot",1},{"bindingId",newBinding}}));macro->setValueNotifyingHost(.75f);
    peak=0;for(int i=0;i<12;++i)peak=std::max(peak,audio(p));
    check(std::abs(peak-std::pow(10.0,-12.0/20.0))<.0001,"Explicit re-export did not restore macro control.");
    p.request("parameter.unexport",object({{"slot",1}}));p.request("keyboard.panic",object({}));audio(p);
    apply(p,code);ready(p);
}
void current_source_audio_tests(FlowsProcessor& p) {
    const auto original=str(document(p),"id");
    p.request("document.new",object({{"name","Current Source Test"}}));
    const auto id=str(document(p),"id");
    const String gate="import standard as std\nInst.sigstream(std.event.keys.poly()) as key:\n    return key.gate * 0.25\n";
    apply(p,gate);ready(p);p.request("document.save",object({}));
    p.request("keyboard.noteOn",object({{"noteId",9800},{"pitch",60},{"velocity",1.0}}));
    until(p,[&]{return std::abs(audio(p)-.25)<1e-6;},"Saved fixture did not play");
    apply(p,gate.replace("0.25","0.5"));ready(p);
    for(int i=0;i<3;++i)audio(p);
    check(std::abs(audio(p)-.5)<1e-6,"Unsaved source did not replace the live sound");
    check(File(SystemStats::getEnvironmentVariable("FLOWS_WORKSPACE",{})).getChildFile(id).loadFileAsString()==gate,"Valid live edit was silently saved");
    const auto beforeSave=revision(p);p.request("document.save",object({}));
    check(revision(p)==beforeSave&&std::abs(audio(p)-.5)<1e-6,"Saving changed the program or interrupted its held voice");
    apply(p,"invalid current source\n");until(p,[&]{return str(get(p.snapshot(),"compile"),"status")=="error";},"Invalid current source was not diagnosed");
    check(audio(p)<1e-7,"Compile failure left a held old voice audible");
    p.request("document.save",object({}));
    check(File(SystemStats::getEnvironmentVariable("FLOWS_WORKSPACE",{})).getChildFile(id).loadFileAsString()=="invalid current source\n","An invalid draft could not be saved");
    p.request("keyboard.noteOn",object({{"noteId",9801},{"pitch",67},{"velocity",1.0}}));
    MidiBuffer midi;midi.addEvent(MidiMessage::noteOn(1,69,1.0f),0);
    check(audio(p,midi)<1e-7,"Keyboard or MIDI played historical source after saving an invalid draft");
    for(int i=0;i<8;++i)check(audio(p)<1e-7,"Invalid draft resumed old audio");
    apply(p,gate);ready(p);check(audio(p)<1e-7,"Keys pressed during failure replayed into the repaired program");
    p.request("keyboard.noteOn",object({{"noteId",9802},{"pitch",72},{"velocity",1.0}}));audio(p);
    check(std::abs(audio(p)-.25)<1e-6,"Repair did not resume sound from current source");
    p.request("document.close",object({{"documentId",id},{"discard",true}}));
    p.request("document.activate",object({{"documentId",original}}));ready(p);
}
void developer_host_tests(FlowsProcessor& p) {
    const auto fixture=File(__FILE__).getParentDirectory().getParentDirectory().getParentDirectory().getChildFile("examples/ADCxGather/07-developer/01-formula-oscillator.flows");
    const auto source=fixture.loadFileAsString();check(source.contains("from lib.dsp import Phasor"),"Developer demo fixture is missing.");
    apply(p,source);ready(p);
    p.request("keyboard.noteOn",object({{"noteId",47001},{"pitch",57},{"velocity",.8}}));
    double peak=0;for(int i=0;i<20;++i)peak=std::max(peak,audio(p));
    check(peak>0.01,"Native host compiled the developer example but produced no sound.");
    p.request("keyboard.panic",object({}));apply(p,code);ready(p);
}
void package_tests(FlowsProcessor& p){
    ready(p);auto portable=p.request("project.exportText",object({})).toString();auto decoded=JSON::parse(portable);check(str(decoded,"source")==code&&str(decoded,"playableSource")==code,"Package did not preserve source and playable snapshot.");
    MemoryBlock validState;p.getStateInformation(validState);check(validState.getSize()>0,"DAW state is empty.");
    apply(p,"this is an invalid draft\n");until(p,[&]{return str(get(p.snapshot(),"compile"),"status")=="error";},"Invalid draft was not diagnosed.");MemoryBlock draftState;p.getStateInformation(draftState);auto draft=JSON::parse(p.request("project.exportText",object({})).toString());check(str(draft,"source")=="this is an invalid draft\n"&&str(draft,"playableSource")==code,"Invalid draft destroyed last playable snapshot.");
    const auto oldWorkspace=SystemStats::getEnvironmentVariable("FLOWS_WORKSPACE",{});auto imported=File(oldWorkspace).getChildFile("portable-import");imported.createDirectory();setenv("FLOWS_WORKSPACE",imported.getFullPathName().toRawUTF8(),1);
    {
        FlowsProcessor restored;restored.setStateInformation(draftState.getData(),static_cast<int>(draftState.getSize()));restored.prepareToPlay(48000,128);
        until(restored,[&]{auto s=restored.snapshot();return text(restored)=="this is an invalid draft\n"&&str(get(s,"compile"),"status")=="error"&&static_cast<int64>(get(get(s,"audio"),"programRevision"))==0;},"Portable invalid-draft restore did not stay muted.");
        MidiBuffer notes;notes.addEvent(MidiMessage::noteOn(1,69,.7f),0);double peak=audio(restored,notes);for(int i=0;i<20;++i)peak=std::max(peak,audio(restored));check(peak<1e-7,"Restored invalid draft played historical source.");check(text(restored)=="this is an invalid draft\n","Restoring audio overwrote the invalid draft.");
        restored.setStateInformation(validState.getData(),static_cast<int>(validState.getSize()));until(restored,[&]{return text(restored)==code;},"Valid package did not restore source.");ready(restored);restored.releaseResources();
    }
    setenv("FLOWS_WORKSPACE",oldWorkspace.toRawUTF8(),1);p.request("project.importText",object({{"text",portable}}));ready(p);check(text(p)==code,"Portable package import failed.");
}
var openDocument(FlowsProcessor& p,const String& id) {
    const auto documents=get(p.snapshot(),"documents");
    check(documents.isArray(),"Snapshot must list all open documents.");
    for(const auto& entry:*documents.getArray())if(str(entry,"id")==id)return entry;
    throw std::runtime_error("Missing open document: "+id.toStdString());
}
void assertPitch(FlowsProcessor& p,double expected) {
    p.request("keyboard.panic",object({}));audio(p);
    // A measured period checks which instrument actually renders, independently
    // of the active tab label or compiler metadata.
    double previous=0,firstCrossing=-1,lastCrossing=-1,peak=0;int crossings=0;
    for(int block=0;block<40;++block){
        AudioBuffer<float> samples(2,128);MidiBuffer notes;
        if(block==0)notes.addEvent(MidiMessage::noteOn(1,69,1.0f),0);
        p.processBlock(samples,notes);
        for(int i=0;i<128;++i){
            const auto value=static_cast<double>(samples.getSample(0,i));
            check(std::isfinite(value),"Switched instrument produced non-finite audio.");
            peak=std::max(peak,std::abs(value));
            if(previous<0&&value>=0){
                const auto crossing=block*128+i-1-previous/(value-previous);
                if(firstCrossing<0)firstCrossing=crossing;
                lastCrossing=crossing;++crossings;
            }
            previous=value;
        }
    }
    check(peak>.05&&crossings>2,"Active instrument did not produce a measurable note.");
    const auto measured=48000.0*(crossings-1)/(lastCrossing-firstCrossing);
    check(std::abs(measured-expected)<1,"Focus selected the wrong instrument audio: expected "+std::to_string(expected)+" Hz, got "+std::to_string(measured)+" Hz.");
    p.request("keyboard.panic",object({}));audio(p);
}
void multi_document_tests(FlowsProcessor& p) {
    p.request("document.save",object({}));
    const auto originalPath=str(document(p),"path");
    const auto workspace=File(SystemStats::getEnvironmentVariable("FLOWS_WORKSPACE",{}));
    const auto fileA=workspace.getChildFile("focus-a.flows"),fileB=workspace.getChildFile("focus-b.flows"),fileBad=workspace.getChildFile("focus-invalid.flows");
    const auto sourceA=code.replace("raw","bassA"),sourceB=code.replace("raw","bassB").replace("pitch: key.pitch,","pitch: key.pitch, octave: 1,");
    const auto draftA=sourceA.replace("-6db","-9db"),draftB=sourceB.replace("-6db","-12db"),savedB=sourceB.replace("-6db","-15db");
    check(fileA.replaceWithText(sourceA,false,false,"\n")&&fileB.replaceWithText(sourceB,false,false,"\n")&&fileBad.replaceWithText("invalid new instrument\n",false,false,"\n"),"Could not create multi-document fixtures.");
    p.request("document.open",object({{"path","focus-a.flows"}}));ready(p);
    const auto idA=str(document(p),"id");const auto initialRevisionA=revision(p);
    apply(p,draftA);ready(p);
    const auto choicesA=JSON::toString(get(get(p.snapshot(),"compile"),"choices"));
    check(get(get(p.snapshot(),"compile"),"choices").getArray()->size()>0,"Active instrument has no key policy choice metadata");
    p.request("document.open",object({{"path","focus-b.flows"}}));ready(p);
    const auto idB=str(document(p),"id");apply(p,draftB);ready(p);
    check(str(openDocument(p,idA),"text")==draftA&&static_cast<bool>(get(openDocument(p,idA),"dirty")),"Opening another tab discarded an unsaved draft.");
    check(fileA.loadFileAsString()==sourceA&&fileB.loadFileAsString()==sourceB,"Switching tabs silently wrote a draft to disk.");
    check(str(get(openDocument(p,idA),"compile"),"status")=="ready","Inactive document lost its own compiled metadata.");
    check(JSON::toString(get(get(openDocument(p,idA),"compile"),"choices"))==choicesA,"Inactive document lost its own choice bindings");

    p.request("document.activate",object({{"documentId",idA}}));ready(p);
    check(text(p)==draftA&&revision(p)>initialRevisionA,"Reactivation did not preserve the draft and advance the session revision.");
    p.request("document.undo",object({}));ready(p);check(text(p)==sourceA,"Document A lost its independent undo history.");
    check(str(openDocument(p,idB),"text")==draftB,"Undo in document A changed document B.");
    p.request("document.redo",object({}));ready(p);check(text(p)==draftA,"Document A lost its independent redo history.");
    const auto count=get(p.snapshot(),"documents").getArray()->size();
    p.request("document.open",object({{"path","focus-a.flows"}}));ready(p);
    check(text(p)==draftA&&get(p.snapshot(),"documents").getArray()->size()==count,"Reopening an existing tab reread disk or duplicated the document.");
    rejects([&]{p.request("document.apply",object({{"documentId",idA},{"baseRevision",static_cast<int64>(initialRevisionA)},{"text",sourceA}}));},"A stale pre-switch edit overwrote the current draft.");
    rejects([&]{p.request("document.apply",object({{"documentId",idB},{"baseRevision",get(document(p),"revision")},{"text",sourceB}}));},"An edit for an inactive document changed the focused instrument.");
    check(text(p)==draftA&&str(openDocument(p,idB),"text")==draftB,"Rejected cross-document edits changed a source.");

    MidiBuffer held;held.addEvent(MidiMessage::noteOn(1,69,1.0f),0);check(audio(p,held)>.05,"Document A did not sound before inactive save.");
    const auto playingRevision=get(get(p.snapshot(),"audio"),"programRevision");
    p.request("document.save",object({{"documentId",idB}}));
    check(str(document(p),"id")==idA&&fileB.loadFileAsString()==draftB&&!static_cast<bool>(get(openDocument(p,idB),"dirty")),"Saving an inactive tab changed focus or saved the wrong source.");
    check(get(get(p.snapshot(),"audio"),"programRevision")==playingRevision&&audio(p)>.05,"Saving an inactive tab restarted or silenced the active instrument.");
    p.request("document.close",object({{"documentId",idB}}));
    check(str(document(p),"id")==idA&&get(get(p.snapshot(),"audio"),"programRevision")==playingRevision&&audio(p)>.05,"Closing an inactive tab changed the sounding instrument.");

    // Switching instruments must clear both resident notes and queued UI notes.
    p.request("keyboard.noteOn",object({{"documentId",idA},{"noteId",987},{"pitch",72},{"velocity",1.0}}));
    p.request("document.open",object({{"path","focus-b.flows"}}));ready(p);
    check(text(p)==draftB,"Reopening a closed tab did not load its saved source.");
    p.request("keyboard.noteOff",object({{"documentId",idA},{"noteId",987},{"pitch",72}}));
    rejects([&]{p.request("keyboard.noteOn",object({{"documentId",idA},{"noteId",988},{"pitch",72},{"velocity",1.0}}));},"A delayed note-on from an inactive editor reached the focused instrument.");
    for(int block=0;block<12;++block)check(audio(p)<1e-7,"A held or queued key from the previous instrument replayed after focus changed.");
    assertPitch(p,880);
    apply(p,savedB);ready(p);
    p.request("document.activate",object({{"documentId",idA}}));ready(p);
    rejects([&]{p.request("document.close",object({{"documentId",idB}}));},"Closing an unsaved inactive tab bypassed the save/discard guard.");
    check(str(document(p),"id")==idA&&str(openDocument(p,idB),"text")==savedB&&fileB.loadFileAsString()==draftB,"Rejected close lost a draft, focus, or saved contents.");
    p.request("document.close",object({{"documentId",idB},{"save",true}}));
    check(str(document(p),"id")==idA&&fileB.loadFileAsString()==savedB,"Save-and-close did not save the selected inactive document.");
    assertPitch(p,440);

    p.request("document.open",object({{"path","focus-b.flows"}}));ready(p);
    for(int i=0;i<16;++i){
        p.request("document.activate",object({{"documentId",i%2==0?idA:idB}}));
        audio(p);std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const auto state=p.snapshot();const auto programRevision=static_cast<int64>(get(get(state,"audio"),"programRevision"));
        check(programRevision==0||programRevision==static_cast<int64>(get(get(state,"document"),"revision")),"A late worker result installed audio for a different active revision.");
    }
    p.request("document.activate",object({{"documentId",idA}}));ready(p);
    check(text(p)==draftA&&str(findGraphNode(get(get(p.snapshot(),"compile"),"graph"),"bassA"),"op")=="sine","Rapid focus changes attached another document's graph or source.");
    assertPitch(p,440);

    // Historical source survives for recovery, but no invalid tab may sound it.
    p.request("document.activate",object({{"documentId",idB}}));ready(p);
    apply(p,"invalid edited B draft\n");
    until(p,[&]{return str(get(p.snapshot(),"compile"),"status")=="error";},"Invalid document B was not diagnosed.");
    p.request("document.activate",object({{"documentId",idA}}));ready(p);
    p.request("document.activate",object({{"documentId",idB}}));
    until(p,[&]{const auto state=p.snapshot();return str(get(state,"compile"),"status")=="error"&&static_cast<int64>(get(get(state,"audio"),"programRevision"))==0;},"Reactivating an invalid draft did not stay silent.");
    MidiBuffer invalidKey;invalidKey.addEvent(MidiMessage::noteOn(1,69,1.0f),0);
    check(audio(p,invalidKey)<1e-7,"Reactivated invalid draft sounded historical source.");check(text(p)=="invalid edited B draft\n","Invalid draft was overwritten.");
    p.request("document.open",object({{"path","focus-invalid.flows"}}));
    until(p,[&]{return str(get(p.snapshot(),"compile"),"status")=="error";},"New malformed instrument was not diagnosed.");
    MidiBuffer on;on.addEvent(MidiMessage::noteOn(1,69,1.0f),0);
    check(audio(p,on)<1e-7,"A new malformed instrument sounded the previous document.");
    for(int block=0;block<12;++block)check(audio(p)<1e-7,"Previous instrument remained audible after opening a malformed file.");

    const auto remaining=get(p.snapshot(),"documents");
    std::vector<String> toClose;for(const auto& entry:*remaining.getArray())toClose.push_back(str(entry,"id"));
    for(const auto& id:toClose)p.request("document.close",object({{"documentId",id},{"discard",true}}));
    check(get(p.snapshot(),"documents").getArray()->isEmpty()&&str(document(p),"id").isEmpty(),"Closing the last tab did not enter an empty workspace.");
    check(str(get(p.snapshot(),"compile"),"status")=="idle","Empty workspace still reports an active compile.");
    on.clear();on.addEvent(MidiMessage::noteOn(1,69,1.0f),0);check(audio(p,on)<1e-7,"An empty workspace still plays an instrument.");
    check(static_cast<int64>(get(get(p.snapshot(),"audio"),"programRevision"))==0,"Empty workspace exposes an active audio revision.");
    check(fileA.loadFileAsString()==sourceA&&fileB.loadFileAsString()==savedB,"Discarding tabs changed their saved files.");
    const auto emptyPackage=p.request("project.exportText",object({})).toString();
    MemoryBlock emptyState;p.getStateInformation(emptyState);
    p.request("document.open",object({{"path","focus-b.flows"}}));ready(p);
    check(text(p)==savedB&&!static_cast<bool>(get(document(p),"dirty")),"Reopening after all tabs closed resurrected a discarded draft.");
    for(int block=0;block<12;++block)check(audio(p)<1e-7,"A note played with no open instrument leaked into a reopened tab.");
    assertPitch(p,880);
    rejects([&]{p.request("project.importText",object({{"text",emptyPackage}}));},"An empty workspace was accepted as an instrument import.");
    check(text(p)==savedB,"Rejecting an empty instrument import changed the active document.");
    p.setStateInformation(emptyState.getData(),static_cast<int>(emptyState.getSize()));
    until(p,[&]{const auto state=p.snapshot();return get(state,"documents").getArray()->isEmpty()&&str(get(state,"document"),"id").isEmpty()&&str(get(state,"compile"),"status")=="idle";},"DAW recall of an empty workspace created a spurious instrument.");
    on.clear();on.addEvent(MidiMessage::noteOn(1,69,1.0f),0);check(audio(p,on)<1e-7,"DAW recall of an empty workspace left an instrument audible.");
    p.request("document.open",object({{"path",originalPath}}));ready(p);apply(p,code);ready(p);
}
void bundled_example_migration_tests() {
    const auto workspace=File(SystemStats::getEnvironmentVariable("FLOWS_WORKSPACE",{})).getChildFile("example-migration");
    check(workspace.createDirectory().wasOk(),"Could not create example migration directory.");
    const auto fixtures=File(__FILE__).getParentDirectory().getParentDirectory().getChildFile("fixtures/instruments");
    for(const auto& example:retiredBundledExamples) {
        MemoryBlock bytes;check(fixtures.getChildFile(example.name).loadFileAsData(bytes),"Missing original example migration fixture.");
        check(sha(bytes)==example.sha256,"Retired factory example hash is stale.");
        check(workspace.getChildFile(example.name).replaceWithData(bytes.getData(),bytes.getSize()),"Could not seed factory example.");
    }
    const auto edited=workspace.getChildFile(retiredBundledExamples[1].name);
    const auto changed=edited.loadFileAsString()+"\n# My edited instrument\n";
    check(edited.replaceWithText(changed),"Could not write edited example.");
    MemoryBlock editedBytes;check(edited.loadFileAsData(editedBytes),"Could not read edited example bytes.");
    const std::vector<std::string> opened{retiredBundledExamples[0].name};
    const std::filesystem::path workspacePath=workspace.getFullPathName().toStdString();
    check(retireBundledExamples(workspacePath,opened)==1,"Migration must retire only closed, byte-identical factory examples.");
    MemoryBlock retained,openedBytes;
    check(edited.loadFileAsData(retained)&&retained==editedBytes,"Migration modified edited instrument bytes.");
    check(workspace.getChildFile(retiredBundledExamples[0].name).loadFileAsData(openedBytes)&&sha(openedBytes)==retiredBundledExamples[0].sha256,"Migration modified an open instrument.");
    const auto backup=workspace.getChildFile(".example-backups").getChildFile(String(retiredBundledExamples[2].name)+".bak");
    MemoryBlock saved;check(backup.loadFileAsData(saved)&&sha(saved)==retiredBundledExamples[2].sha256,"Factory backup lost original bytes.");
    check(!workspace.getChildFile(retiredBundledExamples[2].name).exists()&&retireBundledExamples(workspacePath,opened)==0,"Repeated example migration changed the workspace.");
}
void workspace_recovery_tests() {
    const auto previousWorkspace=SystemStats::getEnvironmentVariable("FLOWS_WORKSPACE",{});
    const auto workspace=File(previousWorkspace).getChildFile("multi-document-recovery");
    check(workspace.createDirectory().wasOk(),"Could not create isolated workspace recovery directory.");
    struct RestoreEnvironment {
        String previous;
        ~RestoreEnvironment(){setenv("FLOWS_WORKSPACE",previous.toRawUTF8(),1);}
    } restore{previousWorkspace};
    setenv("FLOWS_WORKSPACE",workspace.getFullPathName().toRawUTF8(),1);
    const auto sourceB=code.replace("pitch: key.pitch,","pitch: key.pitch, octave: 1,");
    const auto draftA=code.replace("-6db","-9db");
    workspace.getChildFile("examples").createDirectory();
    const auto fileA=workspace.getChildFile("examples/recovery-a.flows"),fileB=workspace.getChildFile("examples/recovery-b.flows"),fileC=workspace.getChildFile("examples/recovery-clean.flows"),fileD=workspace.getChildFile("examples/recovery-deleted.flows");
    check(fileA.replaceWithText(code,false,false,"\n")&&fileB.replaceWithText(sourceB,false,false,"\n")&&fileC.replaceWithText(code,false,false,"\n")&&fileD.replaceWithText(code,false,false,"\n"),"Could not create workspace recovery fixtures.");
    String idA,idB,idC,idD;
    {
        FlowsProcessor saved(true);saved.prepareToPlay(48000,128);
        saved.request("document.close",object({{"documentId",str(document(saved),"id")}}));
        saved.request("document.open",object({{"path","examples/recovery-a.flows"}}));ready(saved);idA=str(document(saved),"id");
        apply(saved,draftA);ready(saved);
        saved.request("document.open",object({{"path","examples/recovery-clean.flows"}}));ready(saved);idC=str(document(saved),"id");
        saved.request("document.open",object({{"path","examples/recovery-deleted.flows"}}));ready(saved);idD=str(document(saved),"id");
        saved.request("document.open",object({{"path","examples/recovery-b.flows"}}));ready(saved);idB=str(document(saved),"id");
        apply(saved,"invalid recovered B draft\n");
        until(saved,[&]{return str(get(saved.snapshot(),"compile"),"status")=="error";},"Recovery fixture did not retain an invalid draft.");
        saved.releaseResources();
    }
    check(fileA.loadFileAsString()==code&&fileB.loadFileAsString()==sourceB,"Workspace recovery overwrote saved files with drafts.");
    const auto externalSource=code.replace("-6db","-21db")+"# changed outside Flows while it was closed\n";
    check(fileC.replaceWithText(externalSource,false,false,"\n")&&fileD.deleteFile(),"Could not update/delete clean recovery fixtures externally.");
    {
        FlowsProcessor restored(true);restored.prepareToPlay(48000,128);
        check(get(restored.snapshot(),"documents").getArray()->size()==4&&str(document(restored),"id")==idB,"Workspace restart lost open tabs or active focus.");
        check(text(restored)=="invalid recovered B draft\n"&&static_cast<bool>(get(document(restored),"dirty")),"Restart lost the active unsaved draft or dirty state.");
        check(str(openDocument(restored,idA),"text")==draftA&&static_cast<bool>(get(openDocument(restored,idA),"dirty")),"Restart lost an inactive unsaved draft or dirty state.");
        check(str(openDocument(restored,idC),"text")==externalSource&&!static_cast<bool>(get(openDocument(restored,idC),"dirty")),"Restart replaced an externally updated clean file with stale recovered source.");
        restored.request("document.save",object({{"documentId",idC}}));
        check(fileC.loadFileAsString()==externalSource,"Saving a recovered clean tab overwrote external changes.");
        check(str(openDocument(restored,idD),"text")==code&&static_cast<bool>(get(openDocument(restored,idD),"dirty")),"Recovery lost a deleted file or incorrectly marked its unsaved source clean.");
        restored.request("document.save",object({{"documentId",idD}}));
        check(fileD.loadFileAsString()==code&&!static_cast<bool>(get(openDocument(restored,idD),"dirty")),"Saving a recovered deleted file did not recreate it and clear its dirty state.");
        until(restored,[&]{const auto state=restored.snapshot();return str(get(state,"compile"),"status")=="error"&&static_cast<int64>(get(get(state,"audio"),"programRevision"))==0;},"Restart did not mute the active invalid draft.");
        MidiBuffer invalidKey;invalidKey.addEvent(MidiMessage::noteOn(1,69,1.0f),0);check(audio(restored,invalidKey)<1e-7,"Recovered invalid draft sounded historical source.");
        restored.request("document.activate",object({{"documentId",idA}}));ready(restored);assertPitch(restored,440);
        check(text(restored)==draftA&&fileA.loadFileAsString()==code,"Activating a recovered draft reread disk or saved it silently.");
        restored.request("document.close",object({{"documentId",idD}}));
        restored.request("document.close",object({{"documentId",idC}}));
        restored.request("document.close",object({{"documentId",idB},{"discard",true}}));
        restored.request("document.close",object({{"documentId",idA},{"discard",true}}));
        restored.releaseResources();
    }
    {
        FlowsProcessor empty(true);empty.prepareToPlay(48000,128);
        check(get(empty.snapshot(),"documents").getArray()->isEmpty()&&str(document(empty),"id").isEmpty(),"An intentionally empty workspace reopened a discarded tab on restart.");
        check(audio(empty)<1e-7,"Restored empty workspace was audible.");
        empty.releaseResources();
    }
}
var wait_agent(FlowsProcessor& p,const String& requestId,double timeout=10,Array<var>* activityEvents=nullptr){
    var final;until(p,[&]{for(auto& event:p.takeAgentEvents())if(str(event,"requestId")==requestId){if(activityEvents && get(event,"activity").isObject())activityEvents->add(event);if(str(event,"status")!="working")final=event;}return !final.isVoid();},"AI request did not complete: "+requestId.toStdString(),timeout);until(p,[&]{return !static_cast<bool>(get(get(p.snapshot(),"ai"),"busy"));},"AI worker did not clear busy after completion.");return final;
}
void send(FlowsProcessor& p,const String& id,const String& prompt){auto d=document(p);auto response=p.request("ai.send",object({{"requestId",id},{"prompt",prompt},{"documentId",str(d,"id")},{"baseRevision",get(d,"revision")}}));check(str(response,"status")=="working","AI send must acknowledge background work.");}
void json_budget_tests(){
    auto quoted=object({{"source",String::repeatedString("[\\\"{",10000)}});
    auto encoded=JSON::toString(quoted,true);check(str(parseJSONBounded(encoded),"source")==str(quoted,"source"),"JSON budget scanner counted escaped code punctuation as nesting.");
    rejects([]{parseJSONBounded(String::repeatedString("[",10000)+"0"+String::repeatedString("]",10000));},"Deep JSON reached recursive parser.");
    rejects([]{parseJSONBounded("{\"x\":1}",3);},"JSON byte budget was ignored.");
    rejects([]{parseJSONBounded("["+String::repeatedString("0,",4098)+"0]");},"Unbounded JSON arrays were accepted.");
    auto depth128=String::repeatedString("[",128)+"0"+String::repeatedString("]",128);check(parseJSONBounded(depth128).isArray(),"The documented JSON depth boundary must remain accepted.");
}
void endpoint_tests(){
    for(const auto& address:{"https://api.openai.com/v1","https://localhost:8443/v1","http://localhost:1234/v1","http://127.0.0.1:8000/v1"})check(isAllowedAIEndpoint(address),"Valid endpoint rejected.");
    for(const auto& address:{"http://127.0.0.1:80@example.invalid/v1","http://localhost:80@remote.invalid/v1","http://127.0.0.1:80%40remote.invalid/v1","http://localhost:8000\\@remote.invalid/v1","http://remote.invalid/v1","https://user:password@remote.invalid/v1","http://localhost:80000/v1","http://localhost:\n8000/v1"})check(!isAllowedAIEndpoint(address),"Unsafe or ambiguous endpoint accepted: "+std::string(address));
}
var historyMessage(FlowsProcessor& p,const String& conversationId,const String& requestId){
    auto response=p.request("ai.conversations.select",object({{"conversationId",conversationId}}));
    const auto messages=get(get(response,"conversation"),"messages");
    for(const auto& message:*messages.getArray())if(str(message,"id")==requestId)return message;
    throw std::runtime_error("Saved AI message missing: "+requestId.toStdString());
}
void ai_tests(FlowsProcessor& p,const String& endpoint){
    const auto created=p.request("ai.conversations.create",object({{"title","DSP experiments"}}));
    const auto firstConversation=str(created,"activeConversationId");
    p.request("ai.configure",object({{"endpoint",endpoint},{"model","flows-fake"},{"apiKey","secret-test-session"}}));auto before=text(p);auto base=revision(p);send(p,"test-render","TEST_RENDER");Array<var> activityEvents;auto completed=wait_agent(p,"test-render",10,&activityEvents);check(str(completed,"status")=="done","AI tool flow failed: "+str(completed,"error").toStdString());check(text(p)==before&&revision(p)==base,"AI silently modified source before apply.");auto proposal=str(completed,"proposedSource");check(proposal==code.replace("-6db","-12db"),"Validated source proposal was not returned intact.");
    bool sawThinking=false,sawTool=false,sawFinishedTool=false;
    std::set<String> startedSteps;
    for(const auto& event:activityEvents){
        const auto step=get(event,"activity");
        check(str(event,"conversationId")==firstConversation && str(event,"status")=="working","Activity lost its request/conversation identity.");
        check(str(event,"text").isEmpty() && !JSON::toString(event).contains("secret-test-session"),"Activity overwrote answer text or exposed credentials.");
        if(str(step,"state")=="running")startedSteps.insert(str(step,"id"));
        else check(startedSteps.contains(str(step,"id")) && static_cast<int64>(get(step,"durationMs"))>=0,"Activity completion lacked its start or elapsed time.");
        sawThinking|=str(step,"kind")=="thinking" && str(step,"state")=="running";
        sawTool|=str(step,"kind")=="tool" && str(step,"state")=="running";
        sawFinishedTool|=str(step,"kind")=="tool" && str(step,"state")=="done";
    }
    check(sawThinking && sawTool && sawFinishedTool,"Native AI request did not report thinking and actual tool lifecycle events.");
    check(str(completed,"conversationId")==firstConversation&&str(completed,"documentId")==str(document(p),"id"),"Agent completion lacks original conversation/document identity.");
    auto saved=historyMessage(p,firstConversation,"test-render");check(str(saved,"status")=="done"&&str(saved,"text")==str(completed,"text")&&static_cast<bool>(get(saved,"proposalAvailable")),"Saved completed response or proposal availability is wrong.");
    {FlowsProcessor secondInstance;
        secondInstance.request("ai.conversations.archive",object({{"conversationId",firstConversation}}));
        rejects([&]{p.request("ai.apply",object({{"requestId","test-render"},{"documentId",str(document(p),"id")},{"baseRevision",get(completed,"baseRevision")},{"text",proposal}}));},"A proposal applied after another instance archived its conversation.");
        secondInstance.request("ai.conversations.restore",object({{"conversationId",firstConversation}}));
    }
    p.request("ai.apply",object({{"requestId","test-render"},{"documentId",str(document(p),"id")},{"baseRevision",get(completed,"baseRevision")},{"text",proposal}}));check(text(p)==proposal,"Explicit proposal apply failed.");ready(p);
    const auto offlineSource=text(p);const auto offlineRevision=revision(p);send(p,"test-stereo-render","TEST_STEREO_RENDER");auto stereo=wait_agent(p,"test-stereo-render");check(str(stereo,"status")=="done","AI stereo/tempo tool flow failed: "+str(stereo,"error").toStdString());check(text(p)==offlineSource&&revision(p)==offlineRevision&&str(stereo,"proposedSource").isEmpty(),"Offline stereo/tempo checks changed the live document.");
    apply(p,code);ready(p);send(p,"test-stale","TEST_STALE");apply(p,code.replace("-6db","-15db"));auto stale=wait_agent(p,"test-stale");check(str(stale,"status")=="done"&&str(stale,"proposedSource").isNotEmpty(),"Expected a validated stale proposal.");rejects([&]{p.request("ai.apply",object({{"requestId","test-stale"},{"documentId",str(document(p),"id")},{"baseRevision",get(stale,"baseRevision")},{"text",get(stale,"proposedSource")}}));},"Stale AI proposal overwrote newer source.");check(text(p)==code.replace("-6db","-15db"),"Stale AI apply changed the document.");
    check(!static_cast<bool>(get(historyMessage(p,firstConversation,"test-render"),"proposalAvailable")),"Applied proposal remained available in history.");
    check(!static_cast<bool>(get(historyMessage(p,firstConversation,"test-stale"),"proposalAvailable")),"Stale proposal remained available in history.");
    send(p,"test-cancel","TEST_CANCEL");std::this_thread::sleep_for(std::chrono::milliseconds(150));
    const auto secondCreated=p.request("ai.conversations.create",object({{"title","Second conversation"}}));
    const auto secondConversation=str(secondCreated,"activeConversationId");
    rejects([&]{p.request("ai.conversations.archive",object({{"conversationId",firstConversation}}));},"Archived conversation while its worker was live.");
    rejects([&]{p.request("ai.cancel",object({{"requestId","wrong-request"}}));},"A mismatched cancel ID stopped the active request.");
    const auto start=Clock::now();p.request("ai.cancel",object({{"requestId","test-cancel"}}));check(std::chrono::duration<double>(Clock::now()-start).count()<.1,"Cancel blocked the caller.");auto cancelled=wait_agent(p,"test-cancel");check(str(cancelled,"status")=="cancelled","Cancelled request was not reported.");
    check(str(cancelled,"conversationId")==firstConversation,"Completion was routed to the newly selected conversation.");
    check(str(p.request("ai.conversations.list",object({})),"activeConversationId")==secondConversation,"Completion changed the user's conversation selection.");
    check(str(historyMessage(p,firstConversation,"test-cancel"),"status")=="cancelled","Cancelled message did not persist.");
    p.request("ai.conversations.select",object({{"conversationId",secondConversation}}));
    send(p,"test-malformed","TEST_MALFORMED");auto malformed=wait_agent(p,"test-malformed");check(str(malformed,"status")=="failed"&&str(malformed,"proposedSource").isEmpty(),"Invalid provider response must fail without a proposal.");
    send(p,"test-deep","TEST_DEEP");auto deep=wait_agent(p,"test-deep");check(str(deep,"status")=="failed"&&str(deep,"error").contains("JSON"),"Deep provider JSON did not fail safely.");
    send(p,"test-secret-error","TEST_SECRET_ERROR");auto secret=wait_agent(p,"test-secret-error");check(str(secret,"status")=="failed"&&!str(secret,"error").contains("secret-test-session")&&str(secret,"error").contains("401"),"Provider error leaked credentials or lost HTTP status.");
    send(p,"test-reply","TEST_REPLY");auto reply=wait_agent(p,"test-reply");check(str(reply,"status")=="done","AI did not recover after cancellation/failure.");
    send(p,"test-context","TEST_CONTEXT");auto contextual=wait_agent(p,"test-context");check(str(contextual,"status")=="done","Multi-turn context did not reach the provider.");
    check(str(historyMessage(p,secondConversation,"test-malformed"),"status")=="failed","Failed request did not persist.");
    auto renamed=p.request("ai.conversations.rename",object({{"conversationId",secondConversation},{"title","Recovered dialogue"}}));check(str(get(renamed,"conversation"),"title")=="Recovered dialogue","Native conversation rename failed.");
    auto archived=p.request("ai.conversations.archive",object({{"conversationId",secondConversation}}));check(static_cast<bool>(get(get(archived,"conversation"),"archived")),"Native archive failed.");
    auto trashed=p.request("ai.conversations.delete",object({{"conversationId",secondConversation}}));check(static_cast<bool>(get(get(trashed,"conversation"),"deleted")),"Native Trash failed.");
    auto restored=p.request("ai.conversations.restore",object({{"conversationId",secondConversation}}));check(!static_cast<bool>(get(get(restored,"conversation"),"deleted"))&&!static_cast<bool>(get(get(restored,"conversation"),"archived")),"Native restore failed.");
    {FlowsProcessor reopened;auto restoredMessage=historyMessage(reopened,firstConversation,"test-stale");check(str(restoredMessage,"proposedSource").isNotEmpty()&&!static_cast<bool>(get(restoredMessage,"proposalAvailable")),"Reloaded history resurrected actionable proposal.");}
    const auto historyFile=File(SystemStats::getEnvironmentVariable("FLOWS_WORKSPACE",{})).getChildFile(".assistant-history.json");check(!historyFile.loadFileAsString().contains("secret-test-session"),"Session API key leaked into assistant history.");
    apply(p,code);ready(p);const auto originalId=str(document(p),"id");
    send(p,"test-create-instrument","TEST_RENDER");const auto generated=wait_agent(p,"test-create-instrument");
    check(str(generated,"status")=="done","AI standalone source generation failed");
    p.request("ai.create",object({{"requestId","test-create-instrument"},{"documentId",originalId},{"baseRevision",get(generated,"baseRevision")},{"text",str(generated,"proposedSource")}}));ready(p);
    check(str(document(p),"path").startsWith("AI-gen/")&&text(p)==str(generated,"proposedSource"),"Standalone AI source was not saved in AI-gen");
    p.request("document.open",object({{"path",originalId}}));ready(p);check(text(p)==code,"Saving standalone AI source changed the original instrument");
    auto package=p.request("project.exportText",object({})).toString();check(!package.contains("TEST_CONTEXT")&&!package.contains("assistant-history"),"Assistant transcript leaked into instrument package.");check(!package.contains("secret-test-session"),"API key leaked into an instrument package.");MemoryBlock checkpoint;p.getStateInformation(checkpoint);check(std::string_view(static_cast<const char*>(checkpoint.getData()),checkpoint.getSize()).find("secret-test-session")==std::string_view::npos,"API key leaked into DAW state.");check(!JSON::toString(p.snapshot()).contains("secret-test-session"),"Snapshot exposed the session API key.");
    // Workspace requests have no dependency on the focused instrument.
    const auto root=File(SystemStats::getEnvironmentVariable("FLOWS_WORKSPACE",{}));
    const String target="examples/workspace-target.flows";
    root.getChildFile(target).replaceWithText(code);
    p.request("document.open",object({{"path",target}}));
    const auto draft=code+"# unsaved-workspace-marker\n";apply(p,draft);
    p.request("document.open",object({{"path",originalId}}));
    auto workspaceSend=[&](const String& id,const String& marker,const String& reference=String()) {
        p.request("ai.conversations.create",object({{"title","Workspace tools"}}));
        p.request("ai.send",object({{"requestId",id},{"prompt",marker},{"referencePath",reference}}));
        const auto event=wait_agent(p,id);check(str(event,"status")=="done","Workspace AI failed: "+str(event,"error").toStdString());return event;
    };
    auto cross=workspaceSend("workspace-cross","TEST_WORKSPACE");
    check(str(cross,"documentId")==target && static_cast<bool>(get(cross,"proposalAvailable")),"Workspace proposal lost its target.");
    check(str(document(p),"id")==originalId && text(p)==code,"Workspace reads changed the active draft.");
    rejects([&]{p.request("ai.apply",object({{"requestId","workspace-cross"},{"text",get(cross,"proposedSource")}}));},"Cross-file proposal applied to the focused instrument.");
    p.request("document.open",object({{"path",target}}));
    p.request("ai.apply",object({{"requestId","workspace-cross"},{"text",get(cross,"proposedSource")}}));
    check(text(p)==draft.replace("-6db","-12db"),"Targeted workspace edit failed.");
    apply(p,draft);p.request("document.open",object({{"path",originalId}}));
    auto referenced=workspaceSend("workspace-reference","TEST_WORKSPACE_REFERENCE",target);
    const auto persisted=historyMessage(p,str(referenced,"conversationId"),"workspace-reference");
    check(str(persisted,"documentId")==target && static_cast<bool>(get(persisted,"proposalAvailable")),"History lost the cross-file target.");
    p.request("document.open",object({{"path",target}}));apply(p,draft+"# new edit\n");
    rejects([&]{p.request("ai.apply",object({{"requestId","workspace-reference"},{"text",get(referenced,"proposedSource")}}));},"A workspace proposal overwrote a newer target draft.");
    auto open=get(p.snapshot(),"documents");for(const auto& d:*open.getArray())p.request("document.close",object({{"documentId",str(d,"id")},{"discard",true}}));
    check(str(document(p),"id").isEmpty(),"Expected no active instrument.");
    auto newInstrument=workspaceSend("workspace-new","TEST_WORKSPACE_NEW");
    check(str(newInstrument,"documentId").isEmpty(),"New instrument proposal inherited an old target.");
    p.request("ai.create",object({{"requestId","workspace-new"},{"text",get(newInstrument,"proposedSource")}}));ready(p);
    check(str(document(p),"path").startsWith("AI-gen/"),"Workspace generation without an active instrument failed.");

}
}
int main(int argc,char** argv){try{setenv("FLOWS_DISABLE_LIVE_BRIDGE","1",1);String endpoint;for(int i=1;i<argc;++i)if(String(argv[i])=="--endpoint"&&i+1<argc)endpoint=argv[++i];check(endpoint.isNotEmpty(),"Usage: HostTests --endpoint http://127.0.0.1:PORT/v1");check(SystemStats::getEnvironmentVariable("FLOWS_WORKSPACE",{}).isNotEmpty(),"FLOWS_WORKSPACE must point at an isolated test directory.");setenv("FLOWS_AI_CONFIG_DIR",File(SystemStats::getEnvironmentVariable("FLOWS_WORKSPACE",{})).getChildFile("test-global-ai").getFullPathName().toRawUTF8(),1);ScopedJuceInitialiser_GUI initialise;json_budget_tests();endpoint_tests();global_model_tests();{FlowsProcessor processor(false, memorySecrets());processor.prepareToPlay(48000,128);document_tests(processor);graph_tests(processor);choice_tests(processor);probe_tests(processor);stereo_host_tests(processor);preference_tests(processor);midi_tests(processor);stopped_audio_macro_mapping_tests(processor);current_source_audio_tests(processor);developer_host_tests(processor);package_tests(processor);multi_document_tests(processor);ai_tests(processor,endpoint);processor.releaseResources();}standalone_transport_tests();bundled_example_migration_tests();workspace_recovery_tests();std::cout<<"Host integration passed: discrete choices/UTF-16 edits/undo/dependencies, probe aliases/control voice/root budgets, safe example migration, current unsaved-source audio and invalid-source muting, source conflicts, UTF-16 bindings, gestures, stopped-device macro maps, structural unbinding, portable recall, MIDI identities, independent document drafts/undo/save/close, focus/audio isolation, workspace recovery, AI real render/proposal/stale/cancel.\n";return 0;}catch(const std::exception& exception){std::cerr<<"Host integration failure: "<<exception.what()<<'\n';return 1;}}
