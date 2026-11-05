#include "app/Analyzer.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace flows;
using namespace flows::app;
void check(bool value,const std::string& message){if(!value)throw std::runtime_error(message);}
Program compileProgram(const std::string& source){auto result=compile(source,"analysis-test.flows");if(!result.program)throw std::runtime_error(result.diagnostics[0].message);return std::move(*result.program);}
const std::string source=
    "import standard as std\nfrom lib.osc import ClassicSine\n"
    "Inst.sigstream(std.event.keys.poly()) as key:\n"
    "    one = ClassicSine.make()\n    two = ClassicSine.make()\n"
    "    a = one(pitch: key.pitch, level: 0.8)\n"
    "    b = two(pitch: key.pitch, level: 0.4)\n"
    "    p = probe(a)\n    q = probe(b)\n"
    "    difference = p - q\n"
    "    spectrum_diff = spec(p) - spec(q)\n"
    "    energy = power(p) - power(q)\n"
    "    mel_difference = mel_spec(p) - mel_spec(q)\n"
    "    ratio = p / (q - q)\n"
    "    return (a + b) * key.gate\n";
const std::vector<std::string> views{"difference","spectrum_diff","energy","mel_difference"};
const AnalysisView& view(const AnalysisSnapshot& snapshot,const std::string& name){for(const auto& item:snapshot.results)if(item.name==name)return item;throw std::runtime_error("Missing view "+name);}
const AnalysisView& signal(const AnalysisSnapshot& snapshot,const std::string& name){for(const auto& item:snapshot.signals)if(item.name==name)return item;throw std::runtime_error("Missing signal "+name);}
CaptureFrame frame(const ProbeSelection& selection,std::uint64_t sequence,std::uint64_t start){
    CaptureFrame out;out.revision=7;out.generation=2;out.parameterEpoch=1;out.observationEpoch=3;out.sequence=sequence;out.start=start;out.count=512;out.channels=selection.count;out.values=selection.values;out.audio=selection.audio;out.voice=selection.voice;out.voiceGeneration=selection.voice<0?0:1;
    for(std::size_t channel=0;channel<out.channels;++channel){out.channelVoices[channel]=selection.voice;out.channelVoiceGenerations[channel]=out.voiceGeneration;for(std::size_t i=0;i<out.count;++i)out.samples[channel][i]=static_cast<float>((channel==0?.8:.4)*std::sin(2*std::numbers::pi*40*static_cast<double>(start+i)/4096.0));}
    return out;
}
void fill(Analyzer& analyzer,const ProbeSelection& selection,std::uint64_t firstSequence=0,std::uint64_t firstSample=0){for(std::uint64_t i=0;i<8;++i)analyzer.consume(frame(selection,firstSequence+i,firstSample+i*512));}
void tone_diff_power(){
    auto program=compileProgram(source);auto selection=makeProbeSelection(program,7,{},-1,views);check(static_cast<bool>(selection),selection.error);check(selection.selection.count==2,"Probe sources must be shared across four views.");
    Analyzer analyzer;analyzer.configure(program,7,views);for(std::uint64_t i=0;i<7;++i)analyzer.consume(frame(selection.selection,i,i*512));check(!analyzer.snapshot().ready,"FFT must wait for a complete 4096-sample window.");analyzer.consume(frame(selection.selection,7,3584));auto result=analyzer.snapshot();check(result.ready&&result.start==0&&result.end==4096,"Window bounds incorrect.");
    const auto& wave=view(result,"difference");check(wave.valid&&wave.values.size()<=192,"Waveform is invalid or exceeds display budget.");double wavePeak=0;for(float sample:wave.values)wavePeak=std::max(wavePeak,std::abs(static_cast<double>(sample)));check(std::abs(wavePeak-.4)<.0001,"Waveform subtraction is incorrect.");
    const auto& spectrum=view(result,"spectrum_diff");check(spectrum.valid&&spectrum.values.size()<=512&&spectrum.values.size()==spectrum.frequencies.size(),"Spectrum shape/axis incorrect.");auto max=std::max_element(spectrum.values.begin(),spectrum.values.end());const auto index=static_cast<std::size_t>(max-spectrum.values.begin());check(std::abs(spectrum.frequencies[index]-468.75)<.001,"FFT peak frequency incorrect.");check(std::abs(*max-.4)<.0001,"Hann-normalized spectrum difference incorrect.");
    check(!wave.spectrum.empty() && wave.spectrum.size()==wave.spectrumFrequencies.size(),"A waveform difference must expose its own spectrum preview.");
    const auto peak=std::max_element(wave.spectrum.begin(),wave.spectrum.end());
    check(std::abs(*peak-.4)<.0001 && std::abs(wave.spectrumFrequencies[static_cast<std::size_t>(peak-wave.spectrum.begin())]-468.75)<.001,"Difference preview must transform the actual difference samples.");
    check(spectrum.signedSpectrum,"Spectral subtraction must keep a signed axis even when this frame is positive.");
    const auto& power=view(result,"energy");check(power.valid&&power.shape=="scalar"&&std::abs(power.scalar-.24)<.000001,"Mean-square power difference incorrect.");
    const auto& mel=view(result,"mel_difference");check(mel.valid&&mel.values.size()==64&&mel.frequencies.size()==64,"Mel output must have 64 frequency-labelled bands.");double total=0;for(auto value:mel.values)total+=value;check(std::abs(total-.24)<.0001,"Mel triangular filters must preserve interior-band tone power.");
    check(result.signals.size()==2&&result.signals[0].kind=="mixed_audio","Raw audio capture kind incorrect.");
}
void musical_scope() {
    const auto program=compileProgram(source);
    const std::vector<std::string> names{"p","difference"};
    const auto selected=makeProbeSelection(program,7,{},-1,names);
    Analyzer analyzer;analyzer.configure(program,7,names);
    for(std::uint64_t block=0;block<60;++block) {
        auto f=frame(selected.selection,block,block*512);
        f.transport={120,double(f.start)/24000,true,true};
        analyzer.consume(f);
    }
    const auto result=analyzer.snapshot(); const auto& a=signal(result,"a");
    check(a.beatAvailable && a.beatMin.size()==256 && a.beatAge.size()==256,"Beat scope missing its transport-aligned envelope.");
    check(std::abs(a.beatPhase-(30719.0/24000-1))<1e-9,"Beat cursor does not match the captured sample time.");
    check(a.beatAge[0]==0 && a.beatAge[200]==1,"Beat sweep must distinguish current data from the preceding beat.");
    check(a.beatMax[0]>.79 && a.beatMin[0]<-.79,"Beat envelope dropped a within-bin transient.");
    check(view(result,"p").beatMin==a.beatMin,"A direct probe must share its captured beat envelope.");
    check(!view(result,"difference").beatAvailable,"Derived analysis must not pretend to have uncaptured beat history.");
    check(a.detail.size()==1024 && std::abs(a.detail[0])<1e-5 && a.detail[1]>0,"Detail scope must start on the interpolated rising trigger.");
    check(std::abs(a.detailSeconds-1023.0/48000)<1e-10,"Detail scope time axis is wrong.");
    auto gap=frame(selected.selection,61,31232);gap.transport={120,31232.0/24000,true,true};analyzer.consume(gap);
    const auto afterGap=analyzer.snapshot();check(signal(afterGap,"a").beatAge[0]==-1,"A capture gap must not retain an old beat as valid data.");
    auto seek=frame(selected.selection,62,31744);seek.transport={120,-.25,true,true};analyzer.consume(seek);
    const auto afterSeek=analyzer.snapshot();check(signal(afterSeek,"a").beatAge[0]==-1 && signal(afterSeek,"a").beatPhase>.74,"Transport seeks and negative PPQ must map safely.");
    auto stopped=frame(selected.selection,63,32256);stopped.transport={120,0,false,true};analyzer.consume(stopped);
    check(!signal(analyzer.snapshot(),"a").beatAvailable,"Stopped transport must use detail rather than a fictitious moving beat.");
}

void key_capture() {
    auto program=compileProgram(source);
    const auto pitch=std::find_if(program.named_values.begin(),program.named_values.end(),[](const auto& named){return named.name=="key.pitch";});
    check(pitch!=program.named_values.end(),"Missing pitch control.");
    const std::array additional{pitch->value};
    auto selected=makeProbeSelection(program,7,additional,-1,views);
    Analyzer analyzer;analyzer.configure(program,7,views);
    const auto noBeat=[](const AnalysisSnapshot& snapshot) {
        for(const auto* collection:{&snapshot.signals,&snapshot.results})for(const auto& value:*collection)
            check(!value.beatAvailable&&value.beatMin.empty()&&value.beatMax.empty()&&value.beatAge.empty()&&value.beatPhase==0,
                  "Manual performance exposed a synthetic beat cursor or beat history.");
    };
    auto make=[&](std::uint64_t block,bool active,std::uint64_t epoch=1) {
        auto f=frame(selected.selection,block,block*512);f.continuous=false;
        f.performanceActive=active;f.keyEpoch=epoch;f.keyStart=529;f.transport={120,double(f.start)/24000,true,true};
        if(!active)for(auto& channel:f.samples)channel.fill(0);
        return f;
    };
    analyzer.consume(make(0,false,0));
    check(analyzer.snapshot().captureIdle&&!analyzer.snapshot().hasCapture,"Idle captured a free-running oscillator.");
    analyzer.consume(make(1,true));
    check(analyzer.snapshot().start==529,"Performance did not start at the accepted key sample.");
    for(std::uint64_t i=2;i<200;++i)analyzer.consume(make(i,true));
    auto sustained=analyzer.snapshot();noBeat(sustained);
    check(signal(sustained,"key.pitch").valid,"Manual pitch control lost its measured values.");
    check(sustained.ready&&!sustained.captureHeld&&!sustained.captureIdle&&sustained.end==102400,"A held key froze after one beat.");
    auto overlap=make(200,true,2);overlap.keyStart=overlap.start+31;analyzer.consume(overlap);
    check(analyzer.snapshot().ready&&analyzer.snapshot().resets==sustained.resets,"Overlapping poly keys discarded the mixed analysis window.");
    const auto peak=[](const AnalysisSnapshot& snapshot){const auto& spectrum=signal(snapshot,"a").spectrum;return *std::max_element(spectrum.begin(),spectrum.end());};
    for(std::uint64_t i=201;i<210;++i){auto release=make(i,true,2);for(auto& c:release.samples)for(auto& sample:c)sample*=.1f;analyzer.consume(release);}
    noBeat(analyzer.snapshot());
    check(peak(analyzer.snapshot())<peak(sustained)*.11,"Release retained the sustained spectral peak.");
    for(std::uint64_t i=210;i<219;++i)analyzer.consume(make(i,false,2));
    noBeat(analyzer.snapshot());
    check(analyzer.snapshot().ready&&!analyzer.snapshot().captureIdle&&peak(analyzer.snapshot())==0,"Silence must flush the FFT before idle.");
    for(std::uint64_t i=219;i<365;++i)analyzer.consume(make(i,false,2));
    check(analyzer.snapshot().captureIdle&&!analyzer.snapshot().hasCapture&&analyzer.snapshot().signals.empty(),"Finished voices retained old spectra.");
    check(analyzer.snapshot().generation==sustained.generation&&analyzer.snapshot().observationEpoch==sustained.observationEpoch,"Natural idle changed observation identity and released explicit Hold.");
    auto fresh=make(365,true,3);fresh.keyStart=fresh.start+17;analyzer.consume(fresh);
    check(analyzer.snapshot().hasCapture&&analyzer.snapshot().start==fresh.keyStart&&!analyzer.snapshot().ready,"A new performance retained stale history.");
    analyzer.configure(program,7,views);analyzer.consume(make(366,true,3));
    check(analyzer.snapshot().hasCapture&&!analyzer.snapshot().captureIdle,"Changing observed signals during a held key required another press.");
    auto playing=make(367,false,3);playing.continuous=true;playing.transport={120,4,true,true};analyzer.consume(playing);
    check(analyzer.snapshot().continuous&&analyzer.snapshot().hasCapture,"Muted transport must keep acquiring silence.");
    check(signal(analyzer.snapshot(),"key.pitch").beatAvailable,"Silent sequencer transport lost its beat cursor.");
    analyzer.consume(make(368,false,3));
    check(analyzer.snapshot().captureIdle&&!analyzer.snapshot().hasCapture,"Stopped transport retained its history.");
    playing.start+=1024;playing.sequence+=2;analyzer.consume(playing);
    auto stopAndPress=make(370,true,4);stopAndPress.keyStart=stopAndPress.start+23;analyzer.consume(stopAndPress);
    check(!analyzer.snapshot().captureIdle&&analyzer.snapshot().start==stopAndPress.keyStart,"Stop discarded a simultaneous manual key.");
    auto tempo=make(371,true,4);tempo.transport.bpm=60;analyzer.consume(tempo);
    noBeat(analyzer.snapshot());
    check(analyzer.snapshot().hasCapture,"Changing manual tempo interrupted real-time capture.");
}
void gaps_epochs_and_voice(){
    auto program=compileProgram(source);auto selected=makeProbeSelection(program,7,{},0,views);Analyzer analyzer;analyzer.configure(program,7,views);fill(analyzer,selected.selection);check(analyzer.snapshot().ready,"Initial window missing.");
    auto gap=frame(selected.selection,9,4096);analyzer.consume(gap);auto result=analyzer.snapshot();check(!result.ready&&result.status.find("gap")!=std::string::npos,"Missing sequence must invalidate FFT.");
    for(std::uint64_t i=1;i<8;++i)analyzer.consume(frame(selected.selection,9+i,4096+i*512));check(analyzer.snapshot().ready,"Analysis must recover after a fresh full window.");
    auto epoch=frame(selected.selection,17,8192);epoch.parameterEpoch=2;analyzer.consume(epoch);check(!analyzer.snapshot().ready,"Parameter epoch change must split windows.");
    analyzer.configure(program,7,views);fill(analyzer,selected.selection);auto voice=frame(selected.selection,8,4096);voice.voiceGeneration=2;for(auto& generation:voice.channelVoiceGenerations)generation=2;analyzer.consume(voice);check(!analyzer.snapshot().ready,"Selected slot voice replacement must reset capture.");
    analyzer.configure(program,7,views);fill(analyzer,selected.selection);auto observation=frame(selected.selection,8,4096);observation.observationEpoch=4;analyzer.consume(observation);check(!analyzer.snapshot().ready,"Observation selection changes must reset capture.");
    analyzer.configure(program,7,views);fill(analyzer,selected.selection);auto projection=frame(selected.selection,8,4096);projection.projection=ProbeProjection::right;analyzer.consume(projection);check(!analyzer.snapshot().ready&&analyzer.snapshot().projection==ProbeProjection::right,"Projection changes must split capture windows and preserve actual frame identity.");
    analyzer.configure(program,7,views);fill(analyzer,selected.selection);auto transition=frame(selected.selection,8,4096);transition.transition=true;analyzer.consume(transition);check(!analyzer.snapshot().ready,"Transition frames cannot enter a steady analysis window.");
    analyzer.configure(program,7,views);fill(analyzer,selected.selection);auto discontinuity=frame(selected.selection,8,4097);analyzer.consume(discontinuity);check(!analyzer.snapshot().ready,"Sample gaps must reset even with consecutive sequence numbers.");
    analyzer.configure(program,8,views);fill(analyzer,selected.selection);check(!analyzer.snapshot().ready,"Old source revision cannot populate the current view.");
}
void selected_views_and_budget(){
    auto program=compileProgram(source);auto defaults=makeProbeSelection(program,7);check(defaults&&defaults.analysisNames.size()==4,"Default selection is the first four views.");
    std::vector<std::string> only{"energy"};auto chosen=makeProbeSelection(program,7,{},-1,only);check(chosen&&chosen.analysisNames.size()==1&&chosen.selection.count==2,"Explicit selection must resolve just its dependency roots.");
    std::vector<std::string> unknown{"missing"};check(!makeProbeSelection(program,7,{},-1,unknown),"Unknown view must report an error.");
    std::string many="import standard as std\nfrom lib.osc import ClassicSine\nInst.sigstream(std.event.keys.poly()) as key:\n";std::vector<std::string> all;
    for(int i=0;i<9;++i){many+="    o"+std::to_string(i)+" = ClassicSine.make()\n    s"+std::to_string(i)+" = o"+std::to_string(i)+"(pitch: key.pitch)\n    p"+std::to_string(i)+" = probe(s"+std::to_string(i)+")\n";all.push_back("p"+std::to_string(i));}
    many+="    return s0 * key.gate\n";auto large=compileProgram(many);check(makeProbeSelection(large,7).selection.count==4,"Unused authored probes must not consume capture budget.");auto tooMany=makeProbeSelection(large,7,{},-1,all);check(!tooMany&&tooMany.error.find("9")!=std::string::npos,"Excess roots require an explicit budget error.");
    Analyzer analyzer;analyzer.configure(large,7,all);check(!analyzer.snapshot().ready,"Analyzer cannot silently process an over-budget selection.");
    const std::array raw{program.named_values.front().value};
    auto rawOnly=makeProbeSelection(program,7,raw,-1,{},false);
    check(rawOnly&&rawOnly.analysisNames.empty()&&rawOnly.selection.count==1,"Explicit raw-only selection must not subscribe to default analysis views.");
    analyzer.configure(program,7,{},false);fill(analyzer,rawOnly.selection);
    check(analyzer.snapshot().results.empty(),"Explicit raw-only analysis must not evaluate hidden defaults.");
    const std::vector<std::string> aliases{"p"};auto alias=makeProbeSelection(program,7,{},-1,aliases);
    analyzer.configure(program,7,aliases);fill(analyzer,alias.selection);
    const auto captured=analyzer.snapshot();
    check(view(captured,"p").spectrum==signal(captured,"a").spectrum&&!view(captured,"p").spectrum.empty(),"Direct probe must reuse its exact captured spectrum.");
    check(view(captured,"p").spectrumFrequencies==signal(captured,"a").spectrumFrequencies,"Probe spectrum lost the capture frequency axis.");
}
void expanded_analysis_budgets(){
    const std::string header="import standard as std\nInst.sigstream(std.event.keys.poly()) as key:\n    p = probe(key.gate)\n";
    std::string longChain=header;
    for(int i=0;i<65;++i)longChain+="    a"+std::to_string(i)+" = "+(i?"a"+std::to_string(i-1):"p")+" + 1\n";
    auto program=compileProgram(longChain+"    return key.gate\n");std::vector<std::string> selected{"a64"};
    auto rejected=makeProbeSelection(program,7,{},0,selected);check(!rejected&&rejected.error.find("128 nodes")!=std::string::npos,"A single capture root must not permit unbounded expanded waveform buffers.");
    Analyzer analyzer;analyzer.configure(program,7,selected);check(analyzer.snapshot().status.find("128 nodes")!=std::string::npos,"Budget failure must remain visible before capture, without allocating analysis windows.");
    std::string transforms=header,sum;for(int i=0;i<33;++i){auto name="s"+std::to_string(i);transforms+="    "+name+" = spec(p)\n";sum+=(i?" + ":"")+name;}
    program=compileProgram(transforms+"    combined = "+sum+"\n    return key.gate\n");selected={"combined"};rejected=makeProbeSelection(program,7,{},0,selected);check(!rejected&&rejected.error.find("32 spectral")!=std::string::npos,"Spectral transform work needs its own budget.");
    std::string previews=header; selected.clear();
    for(int i=0;i<33;++i){auto name="w"+std::to_string(i);previews+="    "+name+" = p * "+std::to_string(i+1)+"\n";selected.push_back(name);}
    program=compileProgram(previews+"    return key.gate\n");rejected=makeProbeSelection(program,7,{},0,selected);
    check(!rejected&&rejected.error.find("32 spectral")!=std::string::npos,"Implicit waveform preview FFTs must honor the transform budget.");
    std::string many=header;selected.clear();for(int view=0;view<6;++view){auto name="v"+std::to_string(view);many+="    "+name+" = p";for(int i=0;i<50;++i)many+=" + 1";many+="\n";selected.push_back(name);}
    program=compileProgram(many+"    return key.gate\n");rejected=makeProbeSelection(program,7,{},0,selected);check(!rejected&&rejected.error.find("512 nodes")!=std::string::npos,"Selected views must share a bounded aggregate window budget.");
    selected={"v0"};check(static_cast<bool>(makeProbeSelection(program,7,{},0,selected)),"Unselected analysis should not consume active window budget.");
}
void invalid_analysis_and_controls(){
    auto program=compileProgram(source);std::vector<std::string> ratio{"ratio"};auto selection=makeProbeSelection(program,7,{},-1,ratio);Analyzer analyzer;analyzer.configure(program,7,ratio);fill(analyzer,selection.selection);const auto result=analyzer.snapshot();check(result.ready&&!view(result,"ratio").valid&&view(result,"ratio").error.find("zero")!=std::string::npos,"Analysis division by zero must invalidate the result.");
    auto controls=compileProgram("import standard as std\nInst.sigstream(std.event.keys.poly()) as key:\n    p = probe(key.gate)\n    energy = power(p)\n    return key.gate\n");std::vector<std::string> energy{"energy"};auto mixed=makeProbeSelection(controls,7,{},-1,energy);analyzer.configure(controls,7,energy);fill(analyzer,mixed.selection);auto ambiguous=analyzer.snapshot();check(ambiguous.signals[0].kind=="last_voice_control"&&!view(ambiguous,"energy").valid,"Mixed controls must not masquerade as summed audio.");
    auto selected=makeProbeSelection(controls,7,{},0,energy);analyzer.configure(controls,7,energy);fill(analyzer,selected.selection);check(view(analyzer.snapshot(),"energy").valid,"An explicitly selected voice may analyze controls.");
    auto bad=frame(selected.selection,8,4096);bad.samples[0][0]=std::numeric_limits<float>::infinity();analyzer.consume(bad);check(!analyzer.snapshot().ready,"Non-finite captures must invalidate the window.");
}
void partial_capture_and_waiting_shapes(){
    const auto program=compileProgram(source);const auto selection=makeProbeSelection(program,7,{},-1,views);
    Analyzer analyzer;analyzer.configure(program,7,views);const auto waiting=analyzer.snapshot();
    check(!waiting.hasCapture&&view(waiting,"energy").shape=="scalar"&&view(waiting,"spectrum_diff").shape=="spectrum", "Waiting analysis views must retain their declared shapes.");
    analyzer.consume(frame(selection.selection,0,0));auto first=analyzer.snapshot();
    check(first.hasCapture&&!first.ready&&first.observationEpoch==3,"One frame should expose capture without claiming a full analysis window.");
    check(signal(first,"a").valid&&signal(first,"a").sampleCount==512&&!signal(first,"a").windowReady&&signal(first,"a").spectrum.empty(),"Raw waveforms must appear before the FFT is ready.");
    check(view(first,"difference").spectrum.empty()&&view(first,"difference").valid&&!view(first,"difference").windowReady&&!view(first,"energy").valid,"Aligned short waveforms can be subtracted, while power still waits for its full window.");
    auto gap=frame(selection.selection,2,1024);analyzer.consume(gap);auto afterGap=analyzer.snapshot();
    check(afterGap.hasCapture&&!afterGap.ready&&signal(afterGap,"a").sampleCount==512,"After a gap display only the new continuous samples, without blanking or stitching the old window.");
    auto transition=frame(selection.selection,3,1536);transition.transition=true;analyzer.consume(transition);auto split=analyzer.snapshot();
    check(!split.hasCapture&&!split.ready&&split.signals.size()==2&&!signal(split,"a").valid&&signal(split,"a").shape=="waveform", "A transition must retain channel identities but not claim valid captured samples.");
}
void short_poly_bursts_preserve_independent_audio(){
    const std::string text="import standard as std\nfrom lib.osc import ClassicSine\nInst.sigstream(std.event.keys.poly()) as key:\n    osc = ClassicSine.make()\n    a = osc(pitch: key.pitch, level: 0.5)\n    p = probe(a)\n    energy = power(p)\n    return a * key.gate\n";
    const auto program=compileProgram(text);ValueId gate=invalid_value;
    for(const auto& named:program.named_values)if(named.name=="key.gate")gate=named.value;
    check(gate!=invalid_value,"Expected named gate signal.");
    const std::array additional{gate};const std::vector<std::string> energy{"energy"};
    for(const int selectedVoice:{-1,0}){
        const auto selection=makeProbeSelection(program,7,additional,selectedVoice,energy);check(static_cast<bool>(selection),selection.error);
        Analyzer analyzer;analyzer.configure(program,7,energy);
        RealtimeSession session;auto prepared=std::make_unique<PreparedProgram>(program,text,7,48000);prepared->probes=selection.selection;session.submit(std::move(prepared));
        std::array<float,128> output{};bool sawAudioSpectrum=false,sawValidEnergy=false;
        for(std::uint64_t block=0;block<48;++block){
            const auto start=block*output.size();std::array<Event,2> events{};std::size_t count=0;
            if(block%2==0){events[0]={start,EventType::note_on,block+1,69,1};events[1]={start+64,EventType::note_off,block+1,0,0};count=2;}
            session.process(std::span(events).first(count),output);
            while(auto* captured=session.frames().readSlot()){analyzer.consume(*captured);session.frames().commitRead();}
            const auto current=analyzer.snapshot();const auto& audio=signal(current,"a");const auto& control=signal(current,"key.gate");
            check(current.hasCapture&&audio.valid&&control.valid,"Short poly notes must still publish raw waveform/control previews on every frame.");
            check(!control.windowReady&&control.spectrum.empty(),"A control window must never stitch different short-lived voices or idle identities.");
            if(selectedVoice<0&&block>=31){
                check(audio.windowReady&&audio.sampleCount==4096&&!audio.spectrum.empty(),"Control voice churn reset independent mixed-audio history.");
                check(view(current,"energy").valid&&view(current,"energy").windowReady,"Unrelated control resets blocked audio-only analysis.");
                sawAudioSpectrum=true;sawValidEnergy=true;
            }else if(selectedVoice>=0){
                check(!audio.windowReady&&audio.spectrum.empty()&&!view(current,"energy").valid,"Selected-voice transforms stitched together separate short notes.");
            }
        }
        if(selectedVoice<0)check(sawAudioSpectrum&&sawValidEnergy,"Mixed burst audio never acquired a continuous analysis window.");
    }
}
}
int main(){try{tone_diff_power();musical_scope();key_capture();gaps_epochs_and_voice();selected_views_and_budget();expanded_analysis_budgets();invalid_analysis_and_controls();partial_capture_and_waiting_shapes();short_poly_bursts_preserve_independent_audio();std::cout<<"Analyzer tests passed: FFT/diff/power/mel, budgets, gaps/epochs, partial previews, stable shapes and real short poly bursts without cross-voice stitching.\n";return 0;}catch(const std::exception& error){std::cerr<<"Analyzer test failure: "<<error.what()<<'\n';return 1;}}
