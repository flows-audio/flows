#include "app/StepSequencer.hpp"
#include "app/Metronome.hpp"
#include <array>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>
#include <vector>

namespace { thread_local bool realtime=false; }
void* operator new(std::size_t size) {
    if (realtime) std::abort();
    if (auto* p=std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { if (realtime && p) std::abort(); std::free(p); }
void operator delete(void* p,std::size_t) noexcept { ::operator delete(p); }
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete[](void* p,std::size_t) noexcept { ::operator delete(p); }

namespace {
using namespace flows;
using namespace flows::app;
void check(bool ok,const char* message) { if (!ok) throw std::runtime_error(message); }
struct Capture {
    std::array<Event,8192> events{}; std::size_t count=0;
    bool add(Event event) noexcept { if (count==events.size()) return false; events[count++]=event; return true; }
};
SequencePattern allSteps() {
    auto pattern=rollingBassPattern();
    for (auto& step:pattern.rows[0].steps) step={true,.8,.5};
    return pattern;
}
void block(StepSequencer& scheduler,Capture& captured,std::uint64_t clock,std::uint32_t frames,double bpm=120,double ppq=-1,bool enabled=true,bool playing=true) {
    if (ppq<0) ppq=clock*bpm/(60*48000);
    realtime=true;
    const bool success=scheduler.process(enabled,clock,frames,48000,Transport{bpm,ppq,playing,true},[&](Event event){return captured.add(event);});
    realtime=false;
    check(success,"Scheduler exceeded its fixed event or work budget");
}
std::vector<Event> render(const SequencePattern& pattern,std::uint32_t chunk,std::uint32_t frames) {
    StepSequencer scheduler; scheduler.configure(pattern); Capture captured;
    for (std::uint32_t clock=0;clock<frames;) {
        const auto count=std::min(chunk,frames-clock); block(scheduler,captured,clock,count); clock+=count;
    }
    return {captured.events.begin(),captured.events.begin()+static_cast<std::ptrdiff_t>(captured.count)};
}
void metronome_test() {
    auto renderClick=[](std::size_t block,int division,bool muted) {
        Metronome metro;MetronomeSettings settings;settings.mute=muted;settings.division=division;settings.volume=-12;
        std::vector<float> left(48000),right(48000);
        for(std::size_t at=0;at<left.size();at+=block){auto count=std::min(block,left.size()-at);realtime=true;metro.process(true,settings,48000,{120,double(at)/24000,true,true},std::span(left).subspan(at,count),std::span(right).subspan(at,count));realtime=false;}
        check(left==right,"Metronome was not centered");return left;
    };
    const auto one=renderClick(1,8,false),chunk=renderClick(127,8,false),muted=renderClick(128,8,true);
    for(std::size_t i=0;i<one.size();++i){check(std::abs(one[i]-chunk[i])<1e-6,"Metronome timing depends on buffer size");check(muted[i]==0,"Muted metronome made sound");}
    check(one[100]!=0&&one[12000]==0&&one[12100]!=0&&one[13001]!=0&&one[14000]==0,"Half-beat metronome spacing or click envelope is wrong");
}
void timing() {
    auto pattern=allSteps(); pattern.swing=.5; pattern.rowCount=2; pattern.rows[1]=pattern.rows[0]; pattern.rows[1].note=42;
    const auto one=render(pattern,1,48000),chunks=render(pattern,127,48000),large=render(pattern,2048,48000);
    check(one.size()==32 && chunks.size()==one.size() && large.size()==one.size(),"Swing polyphonic event count depends on block size");
    for (std::size_t i=0;i<one.size();++i) {
        const auto& expected=one[i];
        for (const auto* actual:{&chunks[i],&large[i]})
            check(expected.sample==actual->sample && expected.note_id==actual->note_id && expected.type==actual->type && expected.pitch==actual->pitch,"Events changed with audio buffer partition");
    }
    check(one[0].sample==0 && one[2].sample==4500 && one[4].sample==9000 && one[6].sample==10500 && one[8].sample==12000,"Swing must delay alternate sixteenths without moving beat boundaries");
    auto longer=allSteps(); longer.length=32; longer.division=32;
    longer.rows[0].steps[31].velocity=.125;
    const auto thirtyTwo=render(longer,127,96001);
    check(thirtyTwo[62].sample==93000 && thirtyTwo[62].velocity==.125 && thirtyTwo[64].sample==96000,"32-step / 1/32 pattern did not use its second half or wrap on time");
    auto bass=render(rollingBassPattern(),128,24000);
    check(bass.size()==6 && bass[0].sample==6000 && bass[2].sample==12000 && bass[4].sample==18000,"Default xBBB rhythm changed");
}
void tempo_and_edits() {
    auto pattern=allSteps(); pattern.rows[0].steps[0].gate=1;
    StepSequencer scheduler; scheduler.configure(pattern); Capture captured;
    block(scheduler,captured,0,3000); // 0.125 quarter notes into the gate.
    block(scheduler,captured,3000,1501,240,.125);
    check(captured.count==3 && captured.events[1].sample==4500 && captured.events[1].type==EventType::note_off && captured.events[2].sample==4500,"BPM change did not keep the gate and next onset in musical time");
    scheduler.reset(); captured.count=0; scheduler.configure(allSteps());
    block(scheduler,captured,0,1000);
    pattern=allSteps(); pattern.rows[0].note=72; pattern.rows[0].steps[1].velocity=.25; scheduler.configure(pattern);
    block(scheduler,captured,1000,5001);
    check(captured.count==3 && captured.events[1].sample==3000 && captured.events[1].pitch==30 && captured.events[1].note_id==captured.events[0].note_id,"A row-note edit changed the scheduled release identity or restarted playback");
    check(captured.events[2].sample==6000 && captured.events[2].pitch==72 && captured.events[2].velocity==.25,"Step edit failed to apply to the next scheduled onset");
    pattern.rows[0].mute=true; scheduler.configure(pattern); block(scheduler,captured,6001,64);
    check(captured.count==4 && captured.events[3].type==EventType::note_off && captured.events[3].sample==6001,"Muting a row left its note held");
}
void controls() {
    auto pattern=allSteps(); pattern.scale=StepScale::major; pattern.root=0;
    check(sequencePitch(pattern,61)==60 && sequencePitch(pattern,66)==65,"Scale quantization must choose the closest note and lower equal-distance pitch");
    pattern.transpose=12; check(sequencePitch(pattern,61)==72,"Transpose must precede scale quantization");
    pattern.root=11; pattern.scale=StepScale::minor;
    check(sequencePitch(pattern,127)<=127 && sequencePitch(pattern,0)>=0,"Scale quantization exceeded MIDI limits");
    pattern.transpose=0; pattern.root=1; pattern.scale=StepScale::major;
    check(sequencePitch(pattern,0)==0 && sequencePitch(pattern,127)==126,"Scale boundary quantization chose an out-of-range candidate");
    pattern=allSteps(); pattern.direction=StepDirection::reverse;
    StepSequencer scheduler; scheduler.configure(pattern); Capture capture;
    block(scheduler,capture,0,1); check(scheduler.playhead()==15,"Reverse direction did not begin at the last step");
    pattern.direction=StepDirection::pingpong; scheduler.configure(pattern); scheduler.reset(); capture.count=0;
    for (int step=0;step<33;++step) {
        block(scheduler,capture,static_cast<std::uint64_t>(step)*6000,6000);
        const auto expected=step%30; check(scheduler.playhead()==(expected<16 ? expected : 30-expected),"Ping-pong duplicated an endpoint or escaped pattern length");
    }
    pattern.direction=StepDirection::random;
    const auto a=render(pattern,127,192000),b=render(pattern,1024,192000);
    check(a.size()==b.size(),"Random order event count depends on buffers");
    // Distinct per-step velocities expose random index order independently of IDs.
    for (std::size_t i=0;i<32;++i) pattern.rows[0].steps[i].velocity=(i+1)/32.0;
    const auto c=render(pattern,127,192000),d=render(pattern,1024,192000);
    bool changed=false;
    for (std::size_t i=0;i<c.size();++i) { check(c[i].sample==d[i].sample && c[i].velocity==d[i].velocity,"Random playback is not repeatable across buffer sizes"); if(i>1 && c[i].velocity!=c[0].velocity && c[i].type==EventType::note_on) changed=true; }
    check(changed,"Random direction never changed steps");
}
void cleanup_and_bounds() {
    auto pattern=allSteps(); pattern.rowCount=8;
    for (auto& row:pattern.rows) { row=pattern.rows[0]; row.steps[0].gate=1; }
    StepSequencer scheduler; scheduler.configure(pattern); Capture captured;
    block(scheduler,captured,0,1); block(scheduler,captured,1,64,120,-1,false);
    check(captured.count==16 && scheduler.playhead()==-1,"Stop did not release all held row notes");
    for (std::size_t i=0;i<8;++i) check(captured.events[i+8].type==EventType::note_off && captured.events[i+8].note_id==captured.events[i].note_id,"Stop lost per-row note identity");
    scheduler.reset(); captured.count=0; block(scheduler,captured,0,1); block(scheduler,captured,1,64,120,-1,true,false);
    check(captured.count==16,"Host transport stop left sequencer notes held");
    scheduler.reset(); captured.count=0; block(scheduler,captured,0,1); block(scheduler,captured,1,1,120,10);
    check(captured.count==24 && captured.events[8].type==EventType::note_off && captured.events[16].type==EventType::note_on,"Host locate did not release before restarting");
    // Bounded emitter refusal and extreme catch-up cannot loop or allocate.
    scheduler.reset(); realtime=true;
    auto success=scheduler.process(true,0,64,48000,{120,0,true,true},[](Event){return false;});
    realtime=false; check(!success,"Emitter overflow was not reported to the host panic path");
    scheduler.reset(); realtime=true;
    success=scheduler.process(true,0,1000000000,48000,{1000,0,true,true},[](Event){return true;});
    realtime=false; check(!success,"Enormous host block did not hit bounded work limit");
}
}
int main() {
    try { metronome_test(); timing(); tempo_and_edits(); controls(); cleanup_and_bounds(); std::cout<<"Sequencer timing, swing, tempo, key modes, live edits, cleanup and realtime bounds passed.\n"; }
    catch (const std::exception& error) { realtime=false; std::cerr<<error.what()<<'\n'; return 1; }
}
