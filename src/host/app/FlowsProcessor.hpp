#pragma once
#include "RealtimeSession.hpp"
#include "Json.hpp"
#include "HostAutomation.hpp"
#include "StepSequencer.hpp"
#include "SequenceBank.hpp"
#include "Metronome.hpp"
#include "SampleAssets.hpp"
#include "AssistantHistory.hpp"
#include "AIModels.hpp"
#include "engine/boundary_smoother.hpp"
#include <juce_audio_utils/juce_audio_utils.h>
#include <condition_variable>
#include <functional>
#include <thread>
#include <mutex>

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
    juce::var snapshot();
    juce::var companionRequest(const juce::String&, const juce::var&, const std::function<bool()>& cancelled = {});
    juce::var importSample(const juce::File& file);
    juce::var telemetry();
    std::vector<juce::var> takeAgentEvents();
    std::function<void()> showAudioSettings;
    const bool standalone;
    std::atomic<bool> windowTransparency{false};
    void beginHostAutomation() noexcept;
    void releaseMidiNotes() noexcept { midiRelease_.store(true); }
    void hostParameter(int slot, int offset, float value) noexcept;
    void auParameter(int slot, int offset, unsigned int duration, float start, float end, bool ramp) noexcept;
private:
    struct History { juce::String text; };
    struct Document {
        juce::String path, text, savedText, gesture, gestureStart;
        std::uint64_t revision = 1, savedRevision = 1;
        bool needsSave = false;
        bool dirty() const { return needsSave || text != savedText; }
        std::vector<History> undo, redo;
    } document_;
    struct CompileView {
        std::uint64_t revision = 0;
        juce::String status = "pending";
        juce::Array<juce::var> diagnostics, signals, parameters, choices;
        juce::var graph;
        std::optional<Program> program;
    } compiled_;
    struct Exported { juce::String binding, name; bool bound = false; juce::String structure; };
    std::array<Exported, 32> exports_;
    struct SequenceSound {
        juce::String kind = "active", path, source, playback = "one_shot";
        std::vector<SequenceSound> lanes;
        void rename(const juce::String& from,const juce::String& to) { if(kind=="instrument"&&path==from)path=to;for(auto& lane:lanes)lane.rename(from,to); }
        bool references(const juce::String& file) const { if(kind=="instrument"&&path==file)return true;for(const auto& lane:lanes)if(lane.references(file))return true;return false; }
        void collect(std::vector<juce::String>& sources) const { if(kind!="lanes")sources.push_back(source);for(const auto& lane:lanes)lane.collect(sources); }
        bool pinned() const { return kind != "active"; }
    };
    struct OpenDocument {
        Document document;
        CompileView compiled;
        std::array<Exported, 32> exports{};
        std::array<float, 32> macroValues{};
        juce::String playableSource;
        std::vector<std::string> selectedSignals;
        int selectedVoice = -1;
        ProbeProjection projection = ProbeProjection::mid;
        SequencePattern sequencer = rollingBassPattern();
        SequenceSound sequenceSound;
    };
    // The active document stays in the fields above; inactive tabs own all their
    // editing and instrument state here. Only the active instrument runs DSP.
    std::map<juce::String, OpenDocument> inactiveDocuments_;
    std::vector<juce::String> documentOrder_;
    std::uint64_t nextRevision_ = 1;
    std::atomic<std::uint64_t> audioDocumentRevision_{1};
    // A failed draft may retain historical source, but never audible DSP.
    std::atomic<bool> sourcePlayable_{false};
    struct MacroBinding { ValueId value = invalid_value; double min = 0, max = 1; bool logarithmic = false; };
    struct MacroMap { std::uint64_t revision = 0; std::array<MacroBinding,32> bindings{}; };
    LatestMailbox<MacroMap> macroMaps_;
    MacroMap macros_;
    MacroMap waitingMacros_;
    std::array<juce::AudioParameterFloat*, 32> hostParams_{};
    std::array<float,32> previousMacro_{};
    struct HostPoint { int slot = 0, offset = 0; float value = 0; unsigned int duration = 0; float start = 0; int kind = 0; };
    HostQueue<HostPoint,256> auPoints_;
    std::atomic<bool> auOverflow_{false};
    std::array<HostPoint,256> hostPoints_{};
    std::size_t hostPointCount_ = 0;
    bool rawHostPoints_ = false, hostPointOverflow_ = false;
    struct KeyboardEvent { Event event; std::uint64_t documentRevision = 0; };
    SpscQueue<KeyboardEvent, 256> keyboard_;
    struct MidiHeld { bool used = false; int channel = 0, pitch = 0; std::uint64_t id = 0, order = 0; };
    std::array<MidiHeld,1024> midiHeld_{};
    std::uint64_t nextNote_ = 1, midiOrder_ = 0;
    std::array<Event,256> blockEvents_{};
    std::array<ParameterUpdate,256> blockParameters_{};
    std::array<float,maxFrames> mono_{};
    std::array<float,maxFrames> right_{};
    RealtimeSession session_;
    SequenceBank sequenceSession_;
    std::atomic<double> rate_{48000};
    std::atomic<double> bpm_{138}, reportedBpm_{138};
    std::atomic<bool> hostTempo_{false};
    std::atomic<int> blockSize_{256};
    std::atomic<bool> running_{false}, panic_{false}, demo_{false};
    std::atomic<bool> midiRelease_{false};
    std::atomic<std::uint64_t> hostOverflows_{0};
    SequencePattern sequencePattern_ = rollingBassPattern(), pinnedSequencePattern_ = rollingBassPattern();
    SequenceSound documentSequenceSound_, sequenceSound_;
    juce::String sequenceOwner_, sequenceSoundStatus_ = "ready", sequenceSoundError_;
    std::uint64_t sequenceUnitRevision_ = 1, sequenceUnitGeneration_ = 1, preparedSequenceUnitRevision_ = 0;
    double preparedSequenceRate_ = 0;
    std::atomic<bool> sequencePinned_{false}, sequencePanic_{false}, sequenceAuditionMuted_{false};
    std::atomic<bool> sequenceSourcePlayable_{false};
    std::atomic<std::uint64_t> sequenceTargetRevision_{1}, sequenceMinimumRevision_{1};
    std::array<Event,256> sequenceEvents_{};
    std::array<float,maxFrames> sequenceLeft_{}, sequenceRight_{}, clickLeft_{}, clickRight_{};
    std::atomic<float> mixedPeak_{0};
    std::atomic<std::uint32_t> mixedVoices_{0};
    std::uint64_t sequenceRevision_ = 1;
    LatestMailbox<SequencePattern> sequencePatterns_;
    StepSequencer sequencer_;
    Metronome metronome_;
    bool sequenceAudioPinned_ = false, sequenceWasReady_ = true;
    BoundarySmoother sequenceRouting_;
    bool sequenceRouteEnabled_ = false;
    std::atomic<int> sequencePlayhead_{-1};
    std::optional<juce::int64> nextTransportSample_;
    double fallbackPpq_ = 0;
    std::mutex mutex_;
    juce::File workspace_, settingsFile_;
    juce::var preferences_;
    juce::Array<juce::var> files_, samples_;
    SampleAssets sampleAssets_;
    std::uint64_t requestedRevision_ = 1, preparedRevision_ = 0;
    double preparedRate_ = 0;
    juce::String playableSource_;
    std::vector<std::string> selectedSignals_;
    std::string auditionSignal_;
    int selectedVoice_ = -1;
    ProbeProjection projection_ = ProbeProjection::mid;
    std::uint64_t selectionVersion_ = 0;
    juce::var latestTelemetry_;
    struct Checkpoint {
        juce::MemoryBlock prefix;
        std::shared_ptr<const juce::MemoryBlock> assets;
    };
    std::shared_ptr<const Checkpoint> checkpoint_;
    juce::String checkpointError_;
    std::shared_ptr<const juce::MemoryBlock> restore_;
    std::jthread worker_, aiWorker_, companionWorker_;
    juce::String companionId_;
    struct CompanionProposal { juce::String source, before, documentId; std::uint64_t baseRevision=0, appliedRevision=0; bool applied=false, restored=false; };
    std::map<juce::String,CompanionProposal> companionProposals_;
    void companionLoop(std::stop_token);
    AIModels aiModels_;
    juce::var aiProfiles_;
    juce::String aiSelectedId_, aiSettingsError_;
    double aiRefreshAt_ = 0;
    void refreshAI(bool force = false);
    juce::String aiEndpoint_, aiModel_, apiKey_;
    AssistantHistory assistantHistory_;
    juce::String runningConversationId_, runningRequestId_;
    std::atomic<bool> aiBusy_{false};
    std::atomic<std::uint64_t> aiGeneration_{0};
    std::vector<juce::var> agentEvents_;
    struct Proposal { juce::String text; std::uint64_t revision; juce::String documentId, conversationId, baseSource; bool workspace = false; };
    std::map<juce::String,Proposal> proposals_;
    void worker(std::stop_token);
    void updateCheckpointLocked();
    void updateMacrosLocked();
    void applyLocked(juce::String text, juce::String gesture = {}, juce::String phase = {});
    void loadPackageLocked(const juce::var&, bool asNewDocument = false);
    juce::var packageLocked(bool includeAssets = true) const;
    juce::var recoveryLocked(bool includeAssets = true) const;
    std::vector<juce::String> recoverySourcesLocked() const;
    void loadRecoveryLocked(const juce::var&);
    void cacheActiveLocked();
    void activateLocked(const juce::String&);
    void resetActiveAudioLocked();
    void saveLocked(Document&);
    SequencePattern loadSequenceLocked(const juce::String&) const;
    SequenceSound loadSequenceSoundLocked(const juce::String&) const;
    SequenceSound parseSequenceSound(const juce::var&, bool capture) const;
    juce::var sequenceStateJSON(const SequencePattern&, const SequenceSound&, bool includeSource = true) const;
    void saveSequenceLocked(const juce::String&, const SequencePattern&, const SequenceSound&) const;
    void selectSequenceSoundLocked(SequenceSound, const SequencePattern&, juce::String owner);
    void retainSequenceStateLocked();
    void remapSequenceSound(SequenceSound&, const SamplePathMap&) const;
    const SequencePattern& displayedSequenceLocked() const { return sequenceSound_.pinned() ? pinnedSequencePattern_ : sequencePattern_; }
    std::vector<juce::String> packageSourcesLocked() const;
    void configureWorkspace();
    void sendAI(const juce::var&);
    juce::var conversationViewLocked();
    juce::var readAIInstrumentLocked(const juce::String& path);
    bool proposalCurrentLocked(const Proposal&);
    juce::var runAITool(const juce::String&, const juce::var&, const juce::String& source, const std::function<bool()>& externalCancel = {});
};
void beginHostAutomation(juce::AudioProcessor*) noexcept;
void captureHostParameter(juce::AudioProcessor*, int, int, float) noexcept;
void captureAUParameter(juce::AudioProcessor*, int, unsigned int, float) noexcept;
void captureAURamp(juce::AudioProcessor*, int, int, unsigned int, float, float) noexcept;
}
