#pragma once
// JUCE host adapter. The instrument logic lives in the portable AppCore; this
// class supplies JUCE's audio/MIDI/automation and the macOS platform services.
#include "Json.hpp"
#include "JuceServices.hpp"
#include "app/AppCore.hpp"

#include <juce_audio_utils/juce_audio_utils.h>

#include <array>
#include <atomic>
#include <functional>
#include <memory>

namespace flows::app {
class FlowsProcessor final : public juce::AudioProcessor {
public:
    explicit FlowsProcessor(bool standalone = false, AISecrets secrets = {});
    ~FlowsProcessor() override;
    const juce::String getName() const override { return "Flows"; }
    void prepareToPlay(double, int) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    bool isBusesLayoutSupported(const BusesLayout&) const override;
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 30; }
    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return "Instrument"; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;

    juce::var request(const juce::String& method, const juce::var& params);
    // Same request in the core's JSON type (used by the editor bridge).
    json::Value requestValue(const std::string& method, const json::Value& params);
    juce::var snapshot();
    json::Value snapshotValue();
    juce::var companionRequest(const juce::String&, const juce::var&, const std::function<bool()>& cancelled = {});
    juce::var importSample(const juce::File& file);
    juce::var telemetry();
    std::vector<juce::var> takeAgentEvents();
    // File chooser pattern for importable audio, e.g. "*.wav;*.aif;*.mp3".
    juce::String importWildcard() const;
    std::function<void()> showAudioSettings;
    const bool standalone;
    std::atomic<bool> windowTransparency{false};
    void beginHostAutomation() noexcept { core_->audio().beginHostAutomation(); }
    void releaseMidiNotes() noexcept { core_->audio().releaseMidiNotes(); }
    void hostParameter(int slot, int offset, float value) noexcept { core_->audio().hostParameter(slot, offset, value); }
    void auParameter(int slot, int offset, unsigned int duration, float start, float end, bool ramp) noexcept {
        core_->audio().auParameter(slot, offset, duration, start, end, ramp);
    }
    AppCore& core() { return *core_; }

private:
    std::array<juce::AudioParameterFloat*, 32> hostParams_{};
    std::unique_ptr<JuceMacroParameters> macroParameters_;
    std::unique_ptr<AppCore> core_;
    std::array<RawMidi, 257> midi_{};
    json::Value stamp(json::Value value) const;
};
void beginHostAutomation(juce::AudioProcessor*) noexcept;
void captureHostParameter(juce::AudioProcessor*, int, int, float) noexcept;
void captureAUParameter(juce::AudioProcessor*, int, unsigned int, float) noexcept;
void captureAURamp(juce::AudioProcessor*, int, int, unsigned int, float, float) noexcept;
}  // namespace flows::app
