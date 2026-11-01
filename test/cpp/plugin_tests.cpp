// Loads the shipped bundles, exercising their native wrappers rather than the
// shared AudioProcessor test entry point. Nothing is installed system-wide.
#include "compiler/program.hpp"
#include "host/app/Json.hpp"
#include <juce_audio_utils/juce_audio_utils.h>
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#include <pluginterfaces/base/ibstream.h>
#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivstparameterchanges.h>
#include <pluginterfaces/vst/ivstevents.h>
#include <pluginterfaces/vst/ivsthostapplication.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace Steinberg;
using namespace Steinberg::Vst;
constexpr int frames = 128;
void check(bool ok,const std::string& message){if(!ok)throw std::runtime_error(message);}
void vst(tresult result,const char* operation){check(result==kResultOk,std::string(operation)+" returned "+std::to_string(result));}
void au(OSStatus result,const char* operation){check(result==noErr,std::string(operation)+" returned "+std::to_string(result));}
void near(double actual,double expected,const std::string& message){check(std::abs(actual-expected)<2e-5,message+": got "+std::to_string(actual)+", expected "+std::to_string(expected));}
using Block=std::array<float,frames>;
struct StereoBlock { Block left,right; };
const std::string source=
    "import standard as std\n"
    "class Amplitude:\n"
    "    def init:\n"
    "        self.register_knob(gain, default=0.25, range=(0, 1), step=0.001)\n"
    "    def signal(gate: scalar, gain: scalar = 0.25):\n"
    "        return gate * gain\n"
    "Inst.sigstream(std.event.keys.poly()) as key:\n"
    "    amplitude = Amplitude.make()\n"
    "    return amplitude(gate: key.gate)\n";
std::string package(double gain){
    auto result=flows::compile(source,"wrapper-test.flows");check(result.program.has_value(),result.diagnostics.empty()?"Test source rejected":result.diagnostics[0].message);
    auto it=std::find_if(result.program->parameters.begin(),result.program->parameters.end(),[](const auto& p){return p.binding_id=="amplitude.gain.default.literal0";});
    check(it!=result.program->parameters.end()&&it->min==0&&it->max==1&&it->smoothing_seconds==0,"Test requires unsmoothed explicit 0..1 gain binding.");
    juce::Array<juce::var> macros;macros.add(flows::app::object({{"slot",1},{"bindingId",it->binding_id},{"name","Gain"},{"value",gain}}));
    return juce::JSON::toString(flows::app::object({{"format","flows.instrument"},{"version",1},{"path","Wrapper Test.flows"},{"source",source},{"playableSource",source},{"macros",macros}}),true).toStdString();
}
struct Bundle {
    CFBundleRef value=nullptr;
    explicit Bundle(const std::string& path){
        auto url=CFURLCreateFromFileSystemRepresentation(nullptr,reinterpret_cast<const UInt8*>(path.data()),static_cast<CFIndex>(path.size()),true);
        check(url!=nullptr,"Invalid bundle URL");value=CFBundleCreate(nullptr,url);CFRelease(url);check(value&&CFBundleLoadExecutable(value),"Cannot load bundle: "+path);
    }
    ~Bundle(){if(value)CFRelease(value);} // CFBundle keeps executable resident until process exit.
    template<class T>T symbol(CFStringRef name){auto p=CFBundleGetFunctionPointerForName(value,name);check(p!=nullptr,"Bundle entry point missing");return reinterpret_cast<T>(p);}
};
// Stack-owned host interfaces outlive every process call; the plug-in may retain
// the host context until terminate. QueryInterface uses SDK inline IDs, requiring
// no SDK implementation objects or duplicated global UID definitions.
#define HOST_INTERFACE(Interface) \
    tresult PLUGIN_API queryInterface(const TUID id,void** out) override { \
        if(!out)return kInvalidArgument;*out=nullptr; \
        if(FUnknownPrivate::iidEqual(id,Interface##_iid)||FUnknownPrivate::iidEqual(id,FUnknown_iid)){*out=static_cast<Interface*>(this);addRef();return kResultOk;}return kNoInterface;} \
    uint32 PLUGIN_API addRef() override{return 1;} \
    uint32 PLUGIN_API release() override{return 1;}
struct Host: IHostApplication {
    HOST_INTERFACE(IHostApplication)
    tresult PLUGIN_API getName(String128 name) override {std::fill_n(name,128,0);const char* text="Flows Wrapper Test";for(int i=0;text[i];++i)name[i]=static_cast<TChar>(text[i]);return kResultOk;}
    tresult PLUGIN_API createInstance(TUID,TUID,void** out) override{*out=nullptr;return kNoInterface;}
};
struct Stream: IBStream {
    HOST_INTERFACE(IBStream)
    std::vector<char> bytes;int64 position=0;
    Stream()=default;explicit Stream(const std::string& text):bytes(text.begin(),text.end()){}
    tresult PLUGIN_API read(void* out,int32 count,int32* readCount) override {
        if(count<0)return kInvalidArgument;auto n=std::min<std::size_t>(static_cast<std::size_t>(count),bytes.size()-std::min(bytes.size(),static_cast<std::size_t>(position)));
        if(n)std::memcpy(out,bytes.data()+position,n);position+=static_cast<int64>(n);if(readCount)*readCount=static_cast<int32>(n);return kResultOk;
    }
    tresult PLUGIN_API write(void* in,int32 count,int32* written) override{if(count<0||position<0||position+count>4*1024*1024)return kInvalidArgument;bytes.resize(std::max(bytes.size(),static_cast<std::size_t>(position+count)));if(count)std::memcpy(bytes.data()+position,in,static_cast<std::size_t>(count));position+=count;if(written)*written=count;return kResultOk;}
    tresult PLUGIN_API seek(int64 offset,int32 mode,int64* result) override{auto next=(mode==kIBSeekSet?0:mode==kIBSeekCur?position:static_cast<int64>(bytes.size()))+offset;if(next<0)return kInvalidArgument;position=next;if(result)*result=position;return kResultOk;}
    tresult PLUGIN_API tell(int64* result) override{if(!result)return kInvalidArgument;*result=position;return kResultOk;}
};
struct Events:IEventList {
    HOST_INTERFACE(IEventList)
    std::vector<Event> events;
    int32 PLUGIN_API getEventCount() override{return static_cast<int32>(events.size());}
    tresult PLUGIN_API getEvent(int32 i,Event& out) override{if(i<0||static_cast<std::size_t>(i)>=events.size())return kInvalidArgument;out=events[static_cast<std::size_t>(i)];return kResultOk;}
    tresult PLUGIN_API addEvent(Event& event) override{events.push_back(event);return kResultOk;}
};
struct Queue:IParamValueQueue {
    HOST_INTERFACE(IParamValueQueue)
    ParamID id=0;std::vector<std::pair<int32,double>> points;
    ParamID PLUGIN_API getParameterId() override{return id;}
    int32 PLUGIN_API getPointCount() override{return static_cast<int32>(points.size());}
    tresult PLUGIN_API getPoint(int32 i,int32& offset,ParamValue& value) override{if(i<0||static_cast<std::size_t>(i)>=points.size())return kInvalidArgument;offset=points[static_cast<std::size_t>(i)].first;value=points[static_cast<std::size_t>(i)].second;return kResultOk;}
    tresult PLUGIN_API addPoint(int32 offset,ParamValue value,int32& index) override{index=static_cast<int32>(points.size());points.emplace_back(offset,value);return kResultOk;}
};
struct Changes:IParameterChanges {
    HOST_INTERFACE(IParameterChanges)
    Queue queue;
    int32 PLUGIN_API getParameterCount() override{return queue.points.empty()?0:1;}
    IParamValueQueue* PLUGIN_API getParameterData(int32 i) override{return i==0?&queue:nullptr;}
    IParamValueQueue* PLUGIN_API addParameterData(const ParamID& id,int32& index) override{queue.id=id;index=0;return &queue;}
};
#undef HOST_INTERFACE
class VST {
    Bundle bundle_;Host host_;IPluginFactory* factory_=nullptr;IComponent* component_=nullptr;IAudioProcessor* processor_=nullptr;IEditController* controller_=nullptr;
    IConnectionPoint* componentPoint_=nullptr;IConnectionPoint* controllerPoint_=nullptr;
    bool(*exit_)()=nullptr;int64 clock_=0;Events events_;
public:
    ParamID parameter=0;
    double tempo=120;std::optional<double> ppq;bool playing=true;
    explicit VST(const std::string& path):bundle_(path){
        auto enter=bundle_.symbol<bool(*)(CFBundleRef)>(CFSTR("bundleEntry"));exit_=bundle_.symbol<bool(*)()>(CFSTR("bundleExit"));check(enter(bundle_.value),"VST3 bundleEntry failed");
        factory_=bundle_.symbol<IPluginFactory*(*)()>(CFSTR("GetPluginFactory"))();check(factory_,"VST3 factory missing");
        for(int32 i=0;i<factory_->countClasses();++i){PClassInfo info{};vst(factory_->getClassInfo(i,&info),"getClassInfo");if(std::string(info.category)==kVstAudioEffectClass){vst(factory_->createInstance(info.cid,IComponent_iid,reinterpret_cast<void**>(&component_)),"create component");break;}}
        check(component_,"VST3 audio component missing");vst(component_->initialize(&host_),"component initialize");vst(component_->queryInterface(IAudioProcessor_iid,reinterpret_cast<void**>(&processor_)),"audio processor interface");
        TUID controllerId{};vst(component_->getControllerClassId(controllerId),"controller class");vst(factory_->createInstance(controllerId,IEditController_iid,reinterpret_cast<void**>(&controller_)),"create controller");vst(controller_->initialize(&host_),"controller initialize");
        vst(component_->queryInterface(IConnectionPoint_iid,reinterpret_cast<void**>(&componentPoint_)),"component connection");vst(controller_->queryInterface(IConnectionPoint_iid,reinterpret_cast<void**>(&controllerPoint_)),"controller connection");vst(componentPoint_->connect(controllerPoint_),"connect component");vst(controllerPoint_->connect(componentPoint_),"connect controller");
        bool found=false;for(int32 i=0;i<controller_->getParameterCount();++i){ParameterInfo info{};vst(controller_->getParameterInfo(i,info),"get parameter");auto title=juce::String(juce::CharPointer_UTF16(reinterpret_cast<const juce::CharPointer_UTF16::CharType*>(info.title)));if(title=="Macro 01"){parameter=info.id;found=true;}}
        check(found,"VST3 Macro 01 was not exposed");
        SpeakerArrangement output=SpeakerArr::kStereo;vst(processor_->setBusArrangements(nullptr,0,&output,1),"stereo bus");vst(component_->activateBus(kAudio,kOutput,0,true),"activate audio");vst(component_->activateBus(kEvent,kInput,0,true),"activate events");
        ProcessSetup setup{kRealtime,kSample32,frames,48000};vst(processor_->setupProcessing(setup),"setup processing");vst(component_->setActive(true),"set active");vst(processor_->setProcessing(true),"set processing");
    }
    ~VST(){if(processor_)processor_->setProcessing(false);if(component_)component_->setActive(false);if(componentPoint_&&controllerPoint_){componentPoint_->disconnect(controllerPoint_);controllerPoint_->disconnect(componentPoint_);}if(componentPoint_)componentPoint_->release();if(controllerPoint_)controllerPoint_->release();if(controller_){controller_->terminate();controller_->release();}if(component_)component_->terminate();if(processor_)processor_->release();if(component_)component_->release();if(factory_)factory_->release();if(exit_)exit_();}
    void load(const std::string& text){Stream stream(text);vst(component_->setState(&stream),"VST3 set state");}
    std::vector<char> save(){Stream stream;vst(component_->getState(&stream),"VST3 get state");check(!stream.bytes.empty(),"VST3 empty state");return stream.bytes;}
    void restore(const std::vector<char>& bytes){Stream stream;stream.bytes=bytes;vst(component_->setState(&stream),"VST3 restore state");}
    void note(bool on,int offset=0,int pitch=69){Event e{};e.type=on?Event::kNoteOnEvent:Event::kNoteOffEvent;e.sampleOffset=offset;e.flags=Event::kIsLive;if(on)e.noteOn={0,static_cast<int16>(pitch),0,1,0,pitch};else e.noteOff={0,static_cast<int16>(pitch),0,pitch,0};events_.events.push_back(e);}
    StereoBlock renderStereo(std::initializer_list<std::pair<int32,double>> points={}){
        Block left,right;left.fill(std::numeric_limits<float>::quiet_NaN());right=left;float* channels[]{left.data(),right.data()};AudioBusBuffers output{};output.numChannels=2;output.channelBuffers32=channels;
        Changes changes;changes.queue.id=parameter;changes.queue.points=points;ProcessContext context{};context.sampleRate=48000;context.projectTimeSamples=clock_;context.tempo=tempo;context.projectTimeMusic=ppq.value_or(double(clock_)*tempo/(60*48000));context.state=ProcessContext::kTempoValid|ProcessContext::kProjectTimeMusicValid|(playing ? ProcessContext::kPlaying : 0);
        ProcessData data{};data.processMode=kRealtime;data.symbolicSampleSize=kSample32;data.numSamples=frames;data.numOutputs=1;data.outputs=&output;data.inputEvents=&events_;data.inputParameterChanges=&changes;data.processContext=&context;
        vst(processor_->process(data),"VST3 process");events_.events.clear();clock_+=frames;for(int i=0;i<frames;++i){check(std::isfinite(left[i])&&std::isfinite(right[i]),"VST3 stereo output must be finite");}return {left,right};
    }
    Block render(std::initializer_list<std::pair<int32,double>> points={}) {
        const auto stereo=renderStereo(points);check(stereo.left==stereo.right,"VST3 mono output must match on both channels");return stereo.left;
    }
};
class AU {
    Bundle bundle_;AudioComponentInstance unit_=nullptr;Float64 clock_=0;
public:
    AudioUnitParameterID parameter=0;
    double tempo=120;std::optional<double> ppq;bool playing=true;
    explicit AU(const std::string& path):bundle_(path){
        auto factory=bundle_.symbol<AudioComponentFactoryFunction>(CFSTR("FlowsAUFactory"));AudioComponentDescription description{kAudioUnitType_MusicDevice,'Flw1','Flwa',kAudioComponentFlag_SandboxSafe,0};
        // Registration belongs to the process, while each recall test needs a
        // fresh instance. Registering the same description again returns null.
        static const auto component=AudioComponentRegister(&description,CFSTR("Flows Audio: Wrapper Test"),65536,factory);
        check(component,"AU process-local registration failed");au(AudioComponentInstanceNew(component,&unit_),"AU instantiate");
        AudioStreamBasicDescription format{};format.mSampleRate=48000;format.mFormatID=kAudioFormatLinearPCM;format.mFormatFlags=static_cast<AudioFormatFlags>(kAudioFormatFlagsNativeFloatPacked)|static_cast<AudioFormatFlags>(kAudioFormatFlagIsNonInterleaved);format.mBytesPerPacket=4;format.mFramesPerPacket=1;format.mBytesPerFrame=4;format.mChannelsPerFrame=2;format.mBitsPerChannel=32;
        au(AudioUnitSetProperty(unit_,kAudioUnitProperty_StreamFormat,kAudioUnitScope_Output,0,&format,sizeof(format)),"AU stream format");UInt32 maximum=frames;au(AudioUnitSetProperty(unit_,kAudioUnitProperty_MaximumFramesPerSlice,kAudioUnitScope_Global,0,&maximum,sizeof(maximum)),"AU maximum frames");
        UInt32 bytes=0;au(AudioUnitGetPropertyInfo(unit_,kAudioUnitProperty_ParameterList,kAudioUnitScope_Global,0,&bytes,nullptr),"AU parameter list size");std::vector<AudioUnitParameterID> parameters(bytes/sizeof(AudioUnitParameterID));au(AudioUnitGetProperty(unit_,kAudioUnitProperty_ParameterList,kAudioUnitScope_Global,0,parameters.data(),&bytes),"AU parameter list");
        bool found=false;for(auto id:parameters){AudioUnitParameterInfo info{};UInt32 size=sizeof(info);au(AudioUnitGetProperty(unit_,kAudioUnitProperty_ParameterInfo,kAudioUnitScope_Global,id,&info,&size),"AU parameter info");auto name=info.cfNameString?juce::String::fromCFString(info.cfNameString):juce::String(info.name);if(name=="Macro 01"){parameter=id;found=true;near(info.minValue,0,"AU parameter minimum");near(info.maxValue,1,"AU parameter maximum");}if((info.flags&kAudioUnitParameterFlag_CFNameRelease)&&info.cfNameString)CFRelease(info.cfNameString);}
        check(found,"AU Macro 01 was not exposed");
        HostCallbackInfo callbacks{};callbacks.hostUserData=this;
        callbacks.beatAndTempoProc=[](void* context,Float64* beat,Float64* tempo)->OSStatus {
            auto& host=*static_cast<AU*>(context);if(beat)*beat=host.ppq.value_or(host.clock_*host.tempo/(60*48000));if(tempo)*tempo=host.tempo;return noErr;
        };
        callbacks.transportStateProc=[](void* context,Boolean* playing,Boolean* changed,Float64* sample,Boolean* cycling,Float64* cycleStart,Float64* cycleEnd)->OSStatus {
            auto& host=*static_cast<AU*>(context);if(playing)*playing=host.playing;if(changed)*changed=false;if(sample)*sample=host.clock_;if(cycling)*cycling=false;if(cycleStart)*cycleStart=0;if(cycleEnd)*cycleEnd=0;return noErr;
        };
        au(AudioUnitSetProperty(unit_,kAudioUnitProperty_HostCallbacks,kAudioUnitScope_Global,0,&callbacks,sizeof(callbacks)),"AU host transport callbacks");
        au(AudioUnitInitialize(unit_),"AU initialize");
    }
    ~AU(){if(unit_){AudioUnitUninitialize(unit_);AudioComponentInstanceDispose(unit_);}}
    CFPropertyListRef save(){CFPropertyListRef state=nullptr;UInt32 size=sizeof(state);au(AudioUnitGetProperty(unit_,kAudioUnitProperty_ClassInfo,kAudioUnitScope_Global,0,&state,&size),"AU save");check(state&&CFGetTypeID(state)==CFDictionaryGetTypeID(),"AU state dictionary missing");return state;}
    void restore(CFPropertyListRef state){au(AudioUnitSetProperty(unit_,kAudioUnitProperty_ClassInfo,kAudioUnitScope_Global,0,&state,sizeof(state)),"AU restore");}
    void load(const std::string& text){auto original=save();auto copy=CFDictionaryCreateMutableCopy(nullptr,0,static_cast<CFDictionaryRef>(original));auto data=CFDataCreate(nullptr,reinterpret_cast<const UInt8*>(text.data()),static_cast<CFIndex>(text.size()));CFDictionarySetValue(copy,CFSTR("jucePluginState"),data);restore(copy);CFRelease(data);CFRelease(copy);CFRelease(original);}
    void note(bool on,int offset=0,int pitch=69){au(MusicDeviceMIDIEvent(unit_,on?0x90:0x80,static_cast<UInt32>(pitch),on?127:0,static_cast<UInt32>(offset)),"AU MIDI event");}
    void immediate(int offset,float value){AudioUnitParameterEvent event{};event.scope=kAudioUnitScope_Global;event.parameter=parameter;event.eventType=kParameterEvent_Immediate;event.eventValues.immediate={static_cast<UInt32>(offset),value};au(AudioUnitScheduleParameters(unit_,&event,1),"AU schedule immediate");}
    void ramp(int offset,UInt32 duration,float from,float to){AudioUnitParameterEvent event{};event.scope=kAudioUnitScope_Global;event.parameter=parameter;event.eventType=kParameterEvent_Ramped;event.eventValues.ramp={offset,duration,from,to};au(AudioUnitScheduleParameters(unit_,&event,1),"AU schedule ramp");}
    StereoBlock renderStereo(){Block left,right;left.fill(std::numeric_limits<float>::quiet_NaN());right=left;struct StereoBuffer {UInt32 count;AudioBuffer buffers[2];} buffers{2,{{1,sizeof(left),left.data()},{1,sizeof(right),right.data()}}};AudioTimeStamp stamp{};stamp.mSampleTime=clock_;stamp.mFlags=kAudioTimeStampSampleTimeValid;AudioUnitRenderActionFlags flags=0;au(AudioUnitRender(unit_,&flags,&stamp,0,frames,reinterpret_cast<AudioBufferList*>(&buffers)),"AU render");clock_+=frames;for(int i=0;i<frames;++i)check(std::isfinite(left[i])&&std::isfinite(right[i]),"AU stereo output must be finite");return {left,right};}
    Block render(){const auto stereo=renderStereo();check(stereo.left==stereo.right,"AU mono output must match on both channels");return stereo.left;}
};
// A known DC instrument makes MIDI sample offsets and automation interpolation
// observable without phase alignment, FFT tolerances, or implementation mocks.
template<class Plugin>void settle(Plugin& plugin,double gain){
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    while(std::chrono::steady_clock::now()<deadline){plugin.note(false);plugin.render();plugin.render();plugin.note(true);auto block=plugin.render();bool ready=true;for(int i=64;i<frames;++i)ready=ready&&std::abs(block[i]-gain)<2e-5;if(ready){plugin.note(false);plugin.render();plugin.render();return;}std::this_thread::sleep_for(std::chrono::milliseconds(5));}
    throw std::runtime_error("Plugin recall did not produce the expected instrument amplitude "+std::to_string(gain));
}
template<class Plugin>void transport_lfo(Plugin& plugin) {
    const std::string text="import standard as std\nfrom lib.mod import ClassicLFO\nInst.sigstream(std.event.keys.poly()) as key:\n    lfo = ClassicLFO.make()\n    motion = lfo(shape: sine, sync: 1/4, mode: transport, bipolar: true)\n    return motion * key.gate * 0.2\n";
    plugin.tempo=240;plugin.ppq=.25;plugin.playing=false;
    plugin.load(juce::JSON::toString(flows::app::object({{"format","flows.instrument"},{"version",1},{"path","Transport.flows"},{"source",text},{"playableSource",text}}),true).toStdString());
    settle(plugin,.2);plugin.ppq=.75;settle(plugin,-.2);
    plugin.ppq.reset();plugin.playing=true;plugin.tempo=120;
}
template<class Plugin>void sine_render(Plugin& plugin,const char* format){
    const std::string sine="import standard as std\nfrom lib.osc import ClassicSine\nInst.sigstream(std.event.keys.poly()) as key:\n    osc = ClassicSine.make()\n    raw = osc(pitch: key.pitch, level: -6db)\n    return raw * key.gate\n";
    plugin.load(juce::JSON::toString(flows::app::object({{"format","flows.instrument"},{"version",1},{"path","Sine.flows"},{"source",sine},{"playableSource",sine}}),true).toStdString());
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);bool ready=false;
    while(std::chrono::steady_clock::now()<deadline){
        // Recall compiles asynchronously and crossfades from the old stereo
        // instrument. Assert mono only once the sine candidate is audible.
        plugin.note(false);plugin.renderStereo();plugin.renderStereo();plugin.note(true);plugin.renderStereo();double square=0,peak=0;int crossings=0;float previous=0;bool mono=true;
        for(int block=0;block<8;++block){const auto rendered=plugin.renderStereo();mono=mono&&rendered.left==rendered.right;for(auto value:rendered.left){square+=value*value;peak=std::max(peak,std::abs(static_cast<double>(value)));if(previous<0&&value>=0)++crossings;previous=value;}}
        const double rms=std::sqrt(square/(8*frames));ready=mono&&rms>.34&&rms<.37&&peak>.500&&peak<.503&&crossings>=8&&crossings<=10;
        plugin.note(false);plugin.renderStereo();plugin.renderStereo();if(ready)break;std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    check(ready,std::string(format)+" A4 sine did not render expected 440 Hz/-6 dB amplitude");for(auto sample:plugin.render())near(sample,0,std::string(format)+" sine note-off silence");
}
template<class Plugin>void stereo_render(Plugin& plugin,const char* format) {
    const std::string text="import standard as std\nInst.sigstream(std.event.keys.poly()) as key:\n    return stereo(key.gate * 0.25, key.gate * 0.75)\n";
    plugin.load(juce::JSON::toString(flows::app::object({{"format","flows.instrument"},{"version",1},{"path","Stereo.flows"},{"source",text},{"playableSource",text}}),true).toStdString());
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);bool ready=false;
    while(std::chrono::steady_clock::now()<deadline) {
        plugin.note(false);plugin.renderStereo();plugin.renderStereo();plugin.note(true);const auto block=plugin.renderStereo();
        ready=true;for(int i=64;i<frames;++i)ready=ready&&std::abs(block.left[i]-.25)<2e-5&&std::abs(block.right[i]-.75)<2e-5;
        plugin.note(false);plugin.renderStereo();plugin.renderStereo();if(ready)break;std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    check(ready,std::string(format)+" wrapper flattened or swapped stereo channels");
    const auto silent=plugin.renderStereo();for(int i=0;i<frames;++i){near(silent.left[i],0,std::string(format)+" stereo note-off left");near(silent.right[i],0,std::string(format)+" stereo note-off right");}
}
// Independent envelope reference for a constant source at 48 kHz: one
// millisecond onset, then three milliseconds from note-off/EOF to exact zero.
double boundaryGate(int sample,int on,int off,double gain) {
    const auto smooth=[](double t){t=std::clamp(t,0.0,1.0);return t*t*(3-2*t);};
    if(sample<on)return 0;
    return gain*smooth(double(sample-on)/48)*(1-smooth(double(sample-off)/144));
}
template<class Plugin>void midi(Plugin& plugin,const char* format){
    plugin.note(true,17);plugin.note(false,91);
    for(int b=0;b<3;++b){const auto block=plugin.render();for(int i=0;i<frames;++i)
        near(block[i],boundaryGate(b*frames+i,17,91,.25),std::string(format)+" MIDI offset / boundary sample "+std::to_string(b*frames+i));}
    plugin.note(true,0,60);plugin.note(true,0,72);auto block=plugin.render();near(block.back(),.5,std::string(format)+" independent voices");
    plugin.note(false,0,60);
    for(int b=0;b<2;++b){block=plugin.render();for(int i=0;i<frames;++i)
        near(block[i],.25+boundaryGate(b*frames+i,-48,0,.25),std::string(format)+" independent release");}
    plugin.note(false,0,72);
    for(int b=0;b<2;++b){block=plugin.render();for(int i=0;i<frames;++i)
        near(block[i],boundaryGate(b*frames+i,-48,0,.25),std::string(format)+" final release");}
}
template<class Plugin>void invalid_draft_recall(Plugin& plugin){
    auto draft=juce::JSON::parse(juce::String(package(.42)));draft.getDynamicObject()->setProperty("source","this is an invalid draft\n");
    plugin.load(juce::JSON::toString(draft,true).toStdString());settle(plugin,0);
    auto state=plugin.save();plugin.load(package(.1));settle(plugin,.1);plugin.restore(state);
    if constexpr(std::is_same_v<decltype(state),CFPropertyListRef>)CFRelease(state);
    settle(plugin,0);
}
// A short stereo WAV makes real sample playback, event offsets and finite
// playback lifetime observable through each shipped wrapper. Its name is
// deliberately absent from the bundled factory library.
juce::MemoryBlock sampler_file(const juce::File& file) {
    check(file.getParentDirectory().createDirectory().wasOk(),"Cannot create sampler wrapper fixture folder");
    auto stream=file.createOutputStream();check(bool(stream),"Cannot create sampler wrapper fixture");
    juce::WavAudioFormat format;
    std::unique_ptr<juce::AudioFormatWriter> writer(format.createWriterFor(stream.get(),48000,2,32,{},0));
    check(bool(writer),"Cannot create sampler wrapper WAV writer");stream.release();
    juce::AudioBuffer<float> buffer(2,64);
    for(int i=0;i<64;++i){buffer.setSample(0,i,.25f);buffer.setSample(1,i,-.5f);}
    check(writer->writeFromAudioSampleBuffer(buffer,0,64)&&writer->flush(),"Cannot write sampler wrapper WAV");writer.reset();
    juce::MemoryBlock bytes;check(file.loadFileAsData(bytes),"Cannot read sampler wrapper WAV");return bytes;
}
std::string state_text(const std::vector<char>& state){return {state.begin(),state.end()};}
std::string state_text(CFPropertyListRef state){
    auto value=CFDictionaryGetValue(static_cast<CFDictionaryRef>(state),CFSTR("jucePluginState"));
    check(value&&CFGetTypeID(value)==CFDataGetTypeID(),"AU native state is missing its processor bytes");
    const auto data=static_cast<CFDataRef>(value);
    return {reinterpret_cast<const char*>(CFDataGetBytePtr(data)),static_cast<std::size_t>(CFDataGetLength(data))};
}
template<class State>struct NativeState {
    State value{};
    void reset(State next){if constexpr(std::is_same_v<State,CFPropertyListRef>)if(value)CFRelease(value);value=std::move(next);}
    ~NativeState(){if constexpr(std::is_same_v<State,CFPropertyListRef>)if(value)CFRelease(value);}
};
template<class Plugin>void sampler_offsets(Plugin& plugin,const char* format,bool wait=false){
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    do {
        plugin.note(false);plugin.renderStereo();plugin.renderStereo();
        plugin.note(true,17,48);plugin.note(false,91,48);const auto block=plugin.renderStereo();
        bool ready=true;
        for(int i=0;i<frames;++i){const auto expected=boundaryGate(i,17,81,.25);ready=ready&&std::abs(block.left[i]-expected)<2e-5&&std::abs(block.right[i]+2*expected)<2e-5;}
        if(ready){
            const auto after=plugin.renderStereo();
            for(int i=0;i<frames;++i){const auto expected=boundaryGate(frames+i,17,81,.25);near(after.left[i],expected,std::string(format)+" sampler EOF left");near(after.right[i],-2*expected,std::string(format)+" sampler EOF right");}
            return;
        }
        if(!wait)break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }while(std::chrono::steady_clock::now()<deadline);
    throw std::runtime_error(std::string(format)+" sampler must preserve stereo, start at offset 17 and finish DSP at frame 81 with a bounded playback tail");
}
template<class Plugin>void sampler_portable_recall(const std::string& bundle,const char* format){
    const auto workspace=juce::File(juce::String(juce::SystemStats::getEnvironmentVariable("FLOWS_WORKSPACE",{})));
    const auto original=workspace.getChildFile("sampler-source"),fresh=workspace.getChildFile("sampler-recall");
    check(original.createDirectory().wasOk()&&fresh.createDirectory().wasOk(),"Cannot create isolated sample wrapper workspaces");
    const juce::String resource="Samples/wrapper-portable-stereo.wav";
    const auto bytes=sampler_file(original.getChildFile(resource));
    const auto encoded=juce::Base64::toBase64(bytes.getData(),bytes.getSize()).toStdString();
    const std::string text="import standard as std\nfrom lib.sample import ClassicSampler\nInst.sigstream(std.event.keys.poly()) as key:\n    sampler = ClassicSampler.make()\n    raw = sampler(file: \"Samples/wrapper-portable-stereo.wav\", trigger: key.trigger, pitch: C3, root: C3, mode: one_shot, level: 0db)\n    return raw\n";
    auto compiled=flows::compile(text,"Wrapper Sample.flows");check(bool(compiled.program),"Wrapper sample source must compile");
    using State=decltype(std::declval<Plugin>().save());NativeState<State> state;
    setenv("FLOWS_WORKSPACE",original.getFullPathName().toRawUTF8(),1);
    {
        Plugin plugin(bundle);
        plugin.load(juce::JSON::toString(flows::app::object({{"format","flows.instrument"},{"version",1},{"path","Wrapper Sample.flows"},{"source",text},{"playableSource",text}}),true).toStdString());
        sampler_offsets(plugin,format,true);sampler_offsets(plugin,format);
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);bool embedded=false;
        do {
            state.reset(plugin.save());const auto saved=state_text(state.value);
            embedded=saved.find(resource.toStdString())!=std::string::npos&&saved.find(encoded)!=std::string::npos;
            if(embedded)break;
            plugin.renderStereo();std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }while(std::chrono::steady_clock::now()<deadline);
        check(embedded,std::string(format)+" saved native state must embed the sample bytes");
    }
    check(original.deleteRecursively(),"Cannot remove original sampler workspace before recall");
    setenv("FLOWS_WORKSPACE",fresh.getFullPathName().toRawUTF8(),1);
    {
        Plugin plugin(bundle);
        check(!fresh.getChildFile(resource).exists(),"Fresh wrapper workspace unexpectedly contains the test sample");
        plugin.restore(state.value);sampler_offsets(plugin,format,true);sampler_offsets(plugin,format);
        juce::MemoryBlock restored;check(fresh.getChildFile(resource).loadFileAsData(restored)&&restored==bytes,std::string(format)+" state recall must recreate the exact sample in the new workspace");
    }
    setenv("FLOWS_WORKSPACE",workspace.getFullPathName().toRawUTF8(),1);
    std::cout<<format<<" sample wrapper passed: stereo file playback, sample-accurate MIDI onset/EOF and embedded asset recall into a fresh workspace.\n";
}
void vst_tests(const std::string& path){
    VST plugin(path);transport_lfo(plugin);stereo_render(plugin,"VST3");sine_render(plugin,"VST3");plugin.load(package(.25));settle(plugin,.25);midi(plugin,"VST3");plugin.note(true);plugin.render();
    // SDK points form piecewise linear ramps, including the implicit previous
    // block value at sample -1. Consecutive points encode a one-sample jump.
    auto block=plugin.render({{15,.25},{16,.75},{47,.75},{79,.35}});
    for(int i=0;i<frames;++i){double expected=i<16?.25:i<=47?.75:i<=79?.75+(.35-.75)*(i-47)/32:.35;near(block[i],expected,"VST3 multipoint automation sample "+std::to_string(i));}
    block=plugin.render({{31,.55}});for(int i=0;i<frames;++i)near(block[i],i<=31?.35+(.55-.35)*(i+1)/32:.55,"VST3 previous-block interpolation sample "+std::to_string(i));
    auto state=plugin.save();plugin.note(false);plugin.render();plugin.render();plugin.load(package(.1));settle(plugin,.1);plugin.restore(state);settle(plugin,.55);plugin.note(true);block=plugin.render();near(block.back(),.55,"VST3 state must preserve latest host automation");plugin.note(false);plugin.render();plugin.render();
    invalid_draft_recall(plugin);
    std::cout<<"VST3 bundle passed: host PPQ/transport LFO, distinct stereo channels, A4 sine, MIDI offsets/polyphony, all parameter points/interpolation, automated/invalid-draft state recall.\n";
}
void au_tests(const std::string& path){
    AU plugin(path);transport_lfo(plugin);stereo_render(plugin,"AU");sine_render(plugin,"AU");plugin.load(package(.25));settle(plugin,.25);midi(plugin,"AU");plugin.note(true);plugin.render();
    plugin.immediate(23,.7f);plugin.immediate(87,.2f);auto block=plugin.render();for(int i=0;i<frames;++i)near(block[i],i<23?.25:i<87?.7:.2,"AU immediate offset sample "+std::to_string(i));
    plugin.ramp(16,32,.2f,.6f);block=plugin.render();for(int i=0;i<frames;++i)near(block[i],i<16?.2:i<=48?.2+.4*(i-16)/32:.6,"AU scheduled ramp sample "+std::to_string(i));
    plugin.ramp(-16,64,.2f,.6f);block=plugin.render();for(int i=0;i<frames;++i)near(block[i],i<=48?.2+.4*(i+16)/64:.6,"AU negative-start ramp sample "+std::to_string(i));
    plugin.ramp(96,64,.6f,.4f);block=plugin.render();for(int i=0;i<frames;++i)near(block[i],i<96?.6:.6-.2*(i-96)/64,"AU cross-block ramp start sample "+std::to_string(i));block=plugin.render();for(int i=0;i<frames;++i)near(block[i],i<=32?.6-.2*(i+32)/64:.4,"AU cross-block ramp continuation sample "+std::to_string(i));
    auto state=plugin.save();plugin.note(false);plugin.render();plugin.render();plugin.load(package(.1));settle(plugin,.1);plugin.restore(state);CFRelease(state);settle(plugin,.4);plugin.note(true);block=plugin.render();near(block.back(),.4,"AU state must preserve latest host automation");plugin.note(false);plugin.render();plugin.render();
    invalid_draft_recall(plugin);
    std::cout<<"AU bundle passed: host PPQ/transport LFO, distinct stereo channels, A4 sine, MIDI offsets/polyphony, scheduled offsets/ramps across blocks, automated/invalid-draft state recall.\n";
}
}
int main(int argc,char** argv){try{setenv("FLOWS_DISABLE_LIVE_BRIDGE","1",1);
    std::string vstPath,auPath;for(int i=1;i<argc;++i){std::string arg=argv[i];if(arg=="--vst3"&&i+1<argc)vstPath=argv[++i];else if(arg=="--au"&&i+1<argc)auPath=argv[++i];}
    check(!vstPath.empty()||!auPath.empty(),"Usage: PluginTests --vst3 bundle --au bundle");juce::ScopedJuceInitialiser_GUI init;
    auto directory=juce::File("/private/tmp").getNonexistentChildFile("flows-wrapper-tests",{},false);check(directory.createDirectory().wasOk(),"Cannot create isolated wrapper workspace");setenv("FLOWS_AI_CONFIG_DIR",directory.getChildFile("test-global-ai").getFullPathName().toRawUTF8(),1);setenv("FLOWS_WORKSPACE",directory.getFullPathName().toRawUTF8(),1);
    if(!vstPath.empty()){auto child=directory.getChildFile("vst3");child.createDirectory();setenv("FLOWS_WORKSPACE",child.getFullPathName().toRawUTF8(),1);vst_tests(vstPath);sampler_portable_recall<VST>(vstPath,"VST3");}
    if(!auPath.empty()){auto child=directory.getChildFile("au");child.createDirectory();setenv("FLOWS_WORKSPACE",child.getFullPathName().toRawUTF8(),1);au_tests(auPath);sampler_portable_recall<AU>(auPath,"AU");}
    directory.deleteRecursively();return 0;
}catch(const std::exception& error){std::cerr<<"Plugin wrapper integration failure: "<<error.what()<<'\n';return 1;}}
