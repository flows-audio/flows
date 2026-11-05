#include "compiler/program.hpp"
#include "engine/engine.hpp"
#include "app/RealtimeSession.hpp"
#include "app/DataflowGraph.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace { bool watch = false; std::size_t allocations = 0, deletions = 0; }
void* operator new(std::size_t n) { if (watch) ++allocations; if (auto* p = std::malloc(n ? n : 1)) return p; throw std::bad_alloc(); }
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { if (watch && p) ++deletions; std::free(p); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }

namespace {
using namespace flows;
using namespace flows::app;
constexpr double tau = 6.283185307179586;
void check(bool ok, const char* reason) { if (!ok) throw std::runtime_error(reason); }
Program valid(const std::string& source) {
    auto result = compile(source,"developer-test.flows");
    if (!result.program) { for (const auto& d : result.diagnostics) std::cerr << d.message << '\n'; throw std::runtime_error("developer source failed"); }
    return std::move(*result.program);
}
std::string source(const std::string& color = "0", const std::string& harmonic = "2") {
    return "import standard as std\nfrom lib.dsp import Phasor\n"
        "class Tone:\n    def init:\n        self.register_knob(color, default=0, range=(0, 0.8), step=0.01, smoothing=5ms)\n"
        "    def signal(freq: hz, color: scalar = 0):\n        clock = Phasor.make()\n"
        "        phase = clock(freq: freq)\n        angle = phase * 6.283185307179586\n"
        "        fundamental = sin(angle)\n        harmonic = sin(angle * " + harmonic + ")\n"
        "        return (fundamental + color * harmonic) / 2\n"
        "Inst.sigstream(std.event.keys.poly()) as key:\n    osc = Tone.make()\n"
        "    raw = osc(freq: pitch_hz(key.pitch), color: " + color + ")\n    p = probe(raw)\n"
        "    return raw * db_gain(-6db) * key.gate * key.velocity\n";
}
ValueId named(const Program& p, const std::string& name) {
    for (const auto& n : p.named_values) if (n.name == name) return n.value;
    throw std::runtime_error("missing developer signal");
}
std::vector<float> render(const Program& p, double rate, std::size_t block, std::size_t frames = 4800) {
    Engine engine(p,{rate,1024,32,0}); std::vector<float> audio(frames);
    for (std::size_t offset=0; offset<frames; offset+=block) {
        const std::array start{Event{0,EventType::note_on,1,69,1}};
        auto events = offset == 0 ? std::span<const Event>(start) : std::span<const Event>{};
        watch = true;
        const auto result = engine.process(events,std::span(audio).subspan(offset,std::min(block,frames-offset)));
        watch = false;
        check(bool(result),"developer render rejected");
    }
    check(allocations==0 && deletions==0,"developer callback touched heap"); return audio;
}
void formulas_and_state() {
    const auto p = valid(source());
    check(p.states.size()==1 && p.states[0].name=="osc.clock","custom class did not own one phase state");
    auto edited = valid(source("0.5"));
    check(dsp_structure_key(p)==dsp_structure_key(edited),"color edit changed state identity");
    check(dsp_structure_key(edited)==dsp_structure_key(valid(source("0.5","3"))),"formula coefficient edit changed graph");
    const auto color = std::find_if(p.parameters.begin(),p.parameters.end(),[](const auto& item){return item.name=="color";});
    check(color!=p.parameters.end() && color->smoothing_seconds==.005,"explicit color smoothing missing");
    const auto json = program_json(p);
    check(json.find("\"op\": \"phasor\"")!=std::string::npos && json.find("\"state_index\"")!=std::string::npos,"phase absent from IR");
    for (double rate : {44100.,48000.}) {
        const auto a=render(edited,rate,64), b=render(edited,rate,127);
        check(a==b,"developer sound depends on callback partition");
        for (std::size_t i=0;i<a.size();++i) {
            const auto angle=tau*std::fmod(double(i)*440./rate,1.);
            const auto expected=(std::sin(angle)+.5*std::sin(2*angle))*.5*std::pow(10.,-6./20.);
            check(std::isfinite(a[i]) && std::abs(a[i]-expected)<2e-6,"custom oscillator differs from authored formula");
        }
    }
    const auto graph = makeDataflowGraph(p);
    check(std::any_of(graph.nodes.begin(),graph.nodes.end(),[](const auto& n){return n.signal=="raw" && n.kind=="audio";}),"formula output lost audio classification");
    const auto text=source(); PreparedProgram prepared(valid(text),text,1,48000);
    check(prepared.audioValues[named(prepared.program,"raw")],"formula cannot be auditioned");
}
void diagnostics_and_math() {
    auto bad = source(); const auto at=bad.find("pitch_hz(key.pitch)"); bad.replace(at,19,"key.pitch");
    check(!compile(bad,"invalid-developer.flows").program,"Phasor accepted pitch without conversion");
    bad=source(); bad.replace(bad.find("sin(angle)"),10,"sin(440hz)");
    check(!compile(bad,"invalid-developer.flows").program,"sin accepted Hz as radians");
    bad=source(); bad.replace(bad.find("sin(angle)"),10,"sin(angle, 1)");
    check(!compile(bad,"invalid-developer.flows").program,"sin accepted wrong arity");
    bad=source(); bad.replace(bad.find("phase = clock(freq: freq)"),25,"phase = clock(freq: freq)\n        again = clock(freq: freq)");
    check(!compile(bad,"invalid-developer.flows").program,"one phase instance evaluated twice");
    check(!compile(source("0.9"),"invalid-developer.flows").program,"custom control range ignored");
    bad=source(); bad.replace(bad.find("smoothing=5ms"),13,"smoothing=5");
    check(!compile(bad,"invalid-developer.flows").program,"smoothing accepted non-time units");
    bad=source(); bad.replace(bad.find("smoothing=5ms"),13,"smoothing=2s");
    check(!compile(bad,"invalid-developer.flows").program,"smoothing accepted an unbounded ramp");
    const auto stereo=valid("import standard as std\nInst.sigstream(std.event.keys.poly()) as key:\n"
        "    pair = stereo(0, 1.5707963267948966)\n    wave = sin(pair)\n    return wave * key.gate\n");
    Engine engine(stereo,{48000,64,32,0}); std::array<float,64> l{},r{};
    const std::array start{Event{0,EventType::note_on,1,69,1}};
    check(bool(engine.process_stereo(start,l,r)),"stereo math rejected");
    check(l[0]==0 && std::abs(r[0]-1)<1e-6,"math failed to preserve stereo channels");
    const auto cosine=valid("import standard as std\nInst.sigstream(std.event.keys.poly()) as key:\n    return cos(0) * key.gate\n");
    check(render(cosine,48000,64)[1]==1,"cosine evaluated incorrectly");
}
void live_edit_and_completion() {
    RealtimeSession session;
    auto submit=[&](const std::string& text,std::uint64_t revision) {
        auto next=std::make_unique<PreparedProgram>(valid(text),text,revision,48000);
        next->probes={revision,{named(next->program,"raw")},{true},1,0};
        session.submit(std::move(next));
    };
    std::array<float,64> out{}; const std::array start{Event{0,EventType::note_on,1,69,1}};
    submit(source(),1);session.process(start,out);
    const auto generation=session.status.generation.load();
    submit(source("0.5"),2);session.process({},out);
    check(session.status.revision==2 && session.status.generation==generation && session.status.voices==1,"live color edit reset phase or voice");
    submit(source("0.5","3"),3);session.process({},out);
    check(session.status.revision==3 && session.status.generation==generation,"harmonic multiplier edit rebuilt phase");
    bool capture=false;
    while (const auto* f=session.frames().readSlot()) {capture=capture || (f->audio[0] && f->count==64);session.frames().commitRead();}
    check(capture,"developer probe not captured");
    const std::array end{Event{192,EventType::note_off,1,69,0}}; session.process(end,out);
    check(session.status.voices==0,"developer voice failed to retire");
}
}
int main() {
    try {formulas_and_state();diagnostics_and_math();live_edit_and_completion();std::cout<<"Developer formula, live edit, capture, lifetime, stereo, partitions and no-heap checks passed.\n";return 0;}
    catch (const std::exception& e) {watch=false;std::cerr<<e.what()<<'\n';return 1;}
}
