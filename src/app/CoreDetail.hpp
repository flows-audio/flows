#pragma once
// Internal helpers shared by the AppCore translation units. Not a public API.
#include "Analyzer.hpp"
#include "DataflowGraph.hpp"
#include "Json.hpp"

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace flows::app::detail {
inline constexpr std::uint64_t uiNoteMask = 1ull << 62;
inline constexpr std::size_t maximumSourceBytes = 1024 * 1024;
inline constexpr std::size_t maximumOpenDocuments = 32;
inline constexpr std::size_t maximumSequenceBytes = 8 * 1024 * 1024;
inline constexpr std::size_t maximumRecoveryBytes = 192 * 1024 * 1024;

[[noreturn]] inline void fail(const std::string& message) { throw std::runtime_error(message); }
std::string readSourceFile(const std::filesystem::path& file);
bool audioNode(const Program& program, ValueId id);
ValueId auditionValue(const Program& program, const std::string& name);
std::vector<ValueId> captureRoots(const Program& program, std::uint32_t analysis);
ProbeSelectionResult selectionForNames(const Program& program, std::uint64_t revision,
                                       const std::vector<std::string>& names, int voice,
                                       ProbeProjection projection = ProbeProjection::mid);
const char* projectionName(ProbeProjection projection);
ProbeProjection parseProjection(const json::Value& value);
const char* analysisShape(AnalysisShape shape);
double sourceScale(const std::string& value);
std::string bindingStructure(const Program& program);
json::Value floatArray(const std::vector<float>& values);
json::Value graphMetadata(const DataflowGraph& graph, const std::string& source);
json::Value emptyGraph();
json::Value preferencesWithPatch(const json::Value& current, const json::Value& patch = {});
// Source for a sampler instrument (document.new) or sequencer sample lane.
std::string samplerSource(const std::string& samplePath, bool probe, const std::string& playback);
}  // namespace flows::app::detail
