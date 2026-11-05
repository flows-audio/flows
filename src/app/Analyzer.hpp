#pragma once

#include "RealtimeSession.hpp"

#include <memory>
#include <span>
#include <string>
#include <vector>

namespace flows::app {

struct ProbeSelectionResult {
    ProbeSelection selection;
    std::vector<std::string> analysisNames;
    std::string error;
    explicit operator bool() const noexcept { return error.empty(); }
};

// Empty analysisNames selects the first four authored analysis views. Explicit
// names select only their dependencies; unused probes consume no capture budget.
ProbeSelectionResult makeProbeSelection(
    const Program& program, std::uint64_t revision,
    std::span<const ValueId> additionalValues = {}, int voice = -1,
    std::span<const std::string> analysisNames = {}, bool defaultAnalysis = true,
    ProbeProjection projection = ProbeProjection::mid);

struct AnalysisView {
    std::string name;
    std::string shape;
    std::string unit;
    std::string kind; // mixed_audio, last_voice_control, selected_voice, analysis
    std::vector<float> values;
    // Presentation-only views; never used as operands in probe analysis.
    std::vector<float> detail, beatMin, beatMax, beatAge;
    double detailSeconds = 0, beatPhase = 0;
    bool beatAvailable = false, signedSpectrum = false;
    std::vector<float> frequencies;
    std::vector<float> spectrum, spectrumFrequencies;
    double rms = 0.0, peak = 0.0;
    double scalar = 0.0;
    std::size_t sampleCount = 0;
    bool windowReady = false;
    bool valid = false;
    std::string error;
};

struct AnalysisSnapshot {
    std::uint64_t revision = 0, generation = 0, parameterEpoch = 0, observationEpoch = 0;
    std::uint64_t start = 0, end = 0, resets = 0;
    double sampleRate = 48000;
    int voice = -1;
    ProbeProjection projection = ProbeProjection::mid;
    bool continuous = true, captureHeld = false, captureIdle = false;
    std::uint64_t keyEpoch = 0;
    bool ready = false;
    bool hasCapture = false;
    std::string status;
    std::vector<AnalysisView> signals;
    std::vector<AnalysisView> results;
};

// Worker/message-thread only. Capture windows never stitch across a missing
// frame, program/parameter/observation boundary or selected-voice replacement.
class Analyzer {
public:
    Analyzer();
    ~Analyzer();
    Analyzer(const Analyzer&) = delete;
    Analyzer& operator=(const Analyzer&) = delete;
    void configure(const Program& program, std::uint64_t revision,
                   std::span<const std::string> analysisNames = {}, bool defaultAnalysis = true);
    void consume(const CaptureFrame& frame);
    AnalysisSnapshot snapshot();
    void reset(std::string reason = "waiting for continuous capture");
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace flows::app
