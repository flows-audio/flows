#pragma once

#include "compiler/program.hpp"
#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace flows::app {
struct DataflowNode {
    std::string id, label, op, kind, signal;
    SourceSpan span;
    std::vector<std::string> parameterIds, choiceIds;
};
struct DataflowEdge { std::string id, source, target, kind, label; };
struct DataflowGraph {
    std::vector<DataflowNode> nodes;
    std::vector<DataflowEdge> edges;
    std::string output;
};

// The graph describes authored names, not every internal arithmetic/conversion
// instruction. Equal ValueIds are aliases: one canonical computation owns its
// inputs/parameters, and other names point to that computation without cycles.
inline DataflowGraph makeDataflowGraph(const Program& program) {
    constexpr std::size_t missing = static_cast<std::size_t>(-1);
    if (program.named_values.size() + program.named_analysis.size() > 4096)
        throw std::runtime_error("Signal graph exceeds 4096 displayed nodes.");
    DataflowGraph graph;
    std::vector<std::size_t> dspNames(program.nodes.size(), missing), analysisNames(program.analysis.size(), missing);
    std::vector<bool> audio(program.nodes.size());
    std::vector<std::vector<const ParameterDescriptor*>> parameters(program.nodes.size());
    for (const auto& parameter : program.parameters)
        if (parameter.value < parameters.size()) parameters[parameter.value].push_back(&parameter);
    for (std::size_t id = 0; id < program.nodes.size(); ++id) {
        const auto& node = program.nodes[id];
        audio[id] = node.op == Op::sampler || node.op == Op::sine || node.op == Op::saw || node.op == Op::unison_sine || node.op == Op::unison_saw || node.op == Op::phasor;
        if (node.op != Op::lfo && node.op != Op::adsr && node.op != Op::key_trigger)
            for (auto input : node.inputs) if (input < id) audio[id] = audio[id] || audio[input];
    }
    for (const auto& named : program.named_values) {
        if (named.value >= program.nodes.size()) throw std::runtime_error("Invalid signal graph value.");
        const auto index = graph.nodes.size();
        const bool alias = dspNames[named.value] != missing;
        if (!alias) dspNames[named.value] = index;
        graph.nodes.push_back({"signal:" + named.name, named.name == "$output" ? "Output" : named.name,
            named.name == "$output" ? "output" : alias ? "alias" : std::string(op_name(program.nodes[named.value].op)),
            audio[named.value] ? "audio" : "control", named.name, named.span, {}, {}});
        if (named.name == "$output") graph.output = graph.nodes.back().id;
    }
    auto analysisOp = [](AnalysisOp op) -> std::string {
        switch (op) {
            case AnalysisOp::probe: return "probe"; case AnalysisOp::constant: return "constant";
            case AnalysisOp::add: return "add"; case AnalysisOp::subtract: return "subtract";
            case AnalysisOp::multiply: return "multiply"; case AnalysisOp::divide: return "divide";
            case AnalysisOp::power: return "power"; case AnalysisOp::spectrum: return "spectrum";
            case AnalysisOp::mel_spectrum: return "mel_spectrum";
        }
        return "analysis";
    };
    for (const auto& named : program.named_analysis) {
        if (named.value >= program.analysis.size()) throw std::runtime_error("Invalid analysis graph value.");
        const auto index = graph.nodes.size();
        const bool alias = analysisNames[named.value] != missing;
        if (!alias) analysisNames[named.value] = index;
        graph.nodes.push_back({"analysis:" + named.name, named.name,
            alias ? "alias" : analysisOp(program.analysis[named.value].op), "analysis", named.name, named.span, {}, {}});
    }
    // Choices belong to the authored call, including calls inside expanded
    // classes. Shared values and downstream expressions never inherit them.
    std::map<std::string,std::set<std::string>> choices;
    for (const auto& choice : program.choices) if (!choice.owner.empty())
        choices[choice.owner == "$return" ? "$output" : choice.owner].insert(choice.binding_id);
    for (auto& node : graph.nodes) if (const auto found=choices.find(node.signal); found!=choices.end())
        node.choiceIds.assign(found->second.begin(),found->second.end());
    std::set<std::string> edgeIds;
    auto connect = [&](std::size_t from, std::size_t to, const std::string& label) {
        if (from == to || from == missing) return;
        const auto& source = graph.nodes[from]; const auto& target = graph.nodes[to];
        const auto id = source.id + "->" + target.id + ":" + label;
        if (!edgeIds.insert(id).second) return;
        if (graph.edges.size() >= 16384) throw std::runtime_error("Signal graph exceeds 16384 displayed edges.");
        graph.edges.push_back({id, source.id, target.id, target.kind == "analysis" ? "analysis" : source.kind, label});
    };
    auto role = [](Op op, std::size_t input) -> std::string {
        if (op == Op::sine || op == Op::saw) return input == 0 ? "frequency" : "level";
        if (op == Op::phasor) return "frequency";
        if (op == Op::math_sin || op == Op::math_cos) return "radians";
        if (op == Op::unison_sine || op == Op::unison_saw) { static constexpr const char* names[]{"frequency","level","detune","spread","pan","phase","random"}; return input < 7 ? names[input] : ""; }
        if (op == Op::sampler) { static constexpr const char* names[]{"trigger","pitch","root","rate","start","end","level","gate"}; return input < 8 ? names[input] : ""; }
        if (op == Op::lfo) { static constexpr const char* names[]{"rate / sync","trigger","phase","delay","fade"}; return input < 5 ? names[input] : ""; }
        if (op == Op::distortion) { static constexpr const char* names[]{"input","drive","mix","output"}; return input < 4 ? names[input] : ""; }
        if (op == Op::stereo) return input==0 ? "left" : "right";
        if (op == Op::left || op == Op::right || op == Op::mid || op == Op::side) return "input";
        if (op == Op::filter_lowpass || op == Op::filter_highpass) return input == 0 ? "input" : input == 1 ? "cutoff" : "resonance";
        if (op == Op::adsr) { static constexpr const char* names[]{"gate","trigger","A","D","S","R"}; return input < 6 ? names[input] : ""; }
        if (op == Op::select) return input == 0 ? "condition" : input == 1 ? "true" : "false";
        return "";
    };
    // Walk each root input separately to preserve its port role, stopping at the
    // closest authored name. A cutoff dependency never becomes a flat edge from
    // every envelope upstream of cutoff, and never borrows their parameter knobs.
    for (std::size_t target = 0; target < program.named_values.size(); ++target) {
        const auto& named = program.named_values[target];
        if (dspNames[named.value] != target) { connect(dspNames[named.value], target, "alias"); continue; }
        std::set<std::string> owned;
        const auto& root = program.nodes[named.value];
        for (const auto* parameter : parameters[named.value]) owned.insert(parameter->binding_id);
        for (std::size_t port = 0; port < root.inputs.size(); ++port) {
            const auto label = role(root.op, port);
            std::vector<ValueId> pending{root.inputs[port]}; std::vector<bool> visited(program.nodes.size());
            while (!pending.empty()) {
                const auto value = pending.back(); pending.pop_back();
                if (value >= program.nodes.size() || visited[value]) continue;
                visited[value] = true;
                if (dspNames[value] != missing) { connect(dspNames[value], target, label); continue; }
                for (const auto* parameter : parameters[value]) owned.insert(parameter->binding_id);
                for (auto input : program.nodes[value].inputs) pending.push_back(input);
            }
        }
        // Compile-time literals such as unison don't survive as DSP inputs.
        // Their stable authored binding still identifies this exact expression.
        const auto context = named.name == "$output" ? "$return" : named.name;
        const auto prefix = context + ".literal";
        for (const auto& parameter : program.parameters)
            if (parameter.binding_id.starts_with(prefix)) owned.insert(parameter.binding_id);
        graph.nodes[target].parameterIds.assign(owned.begin(), owned.end());
    }
    for (std::size_t i = 0; i < program.named_analysis.size(); ++i) {
        const auto target = program.named_values.size() + i;
        const auto root = program.named_analysis[i].value;
        if (analysisNames[root] != target) { connect(analysisNames[root], target, "alias"); continue; }
        std::vector<std::uint32_t> pending{root}; std::vector<bool> visited(program.analysis.size());
        while (!pending.empty()) {
            const auto value = pending.back(); pending.pop_back();
            if (value >= program.analysis.size() || visited[value]) continue;
            visited[value] = true;
            if (value != root && analysisNames[value] != missing) { connect(analysisNames[value], target, ""); continue; }
            const auto& node = program.analysis[value];
            if (node.op == AnalysisOp::probe && node.signal < dspNames.size()) {
                // A class port is a legal probe target even when its caller
                // supplies an unnamed expression. Compress that expression just
                // like DSP inputs, without giving the analysis node its knobs.
                std::vector<ValueId> signals{node.signal}; std::vector<bool> visitedSignals(program.nodes.size());
                while (!signals.empty()) {
                    const auto signal = signals.back(); signals.pop_back();
                    if (signal >= dspNames.size() || visitedSignals[signal]) continue;
                    visitedSignals[signal] = true;
                    if (dspNames[signal] != missing) { connect(dspNames[signal], target, "probe"); continue; }
                    for (auto input : program.nodes[signal].inputs) signals.push_back(input);
                }
            }
            for (auto input : node.inputs) pending.push_back(input);
        }
    }
    if (graph.output.empty() && program.output < dspNames.size() && dspNames[program.output] != missing)
        graph.output = graph.nodes[dspNames[program.output]].id;
    return graph;
}
}
