#pragma once
// The portable Flows application core: documents, undo, compilation, probes,
// sequencer, packages, recovery, AI tools and the Live bridge. Every host (the
// JUCE app and plugins, the browser) drives the same instance type through the
// versioned request/snapshot protocol described in protocol/flows-protocol.json.
#include "AIModels.hpp"
#include "AssistantHistory.hpp"
#include "AudioRuntime.hpp"
#include "Json.hpp"
#include "Platform.hpp"
#include "SampleAssets.hpp"

#include <array>
#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace flows::app {
struct CoreOptions {
    // The workspace directory and the exact path text used for workspaceId.
    std::filesystem::path workspace;
    std::string workspaceIdentity;
    // Directory for machine-wide AI model profiles.
    std::filesystem::path settingsDirectory;
    // Live bridge folder; empty disables the bridge (browser, tests).
    std::filesystem::path liveBridgeDirectory;
    // Standalone hosts own a session recovery file; plugins use DAW state.
    bool standalone = false;
    std::vector<BundledFile> bundled;
    AISecrets secrets;
    std::shared_ptr<HttpClient> http;
    std::shared_ptr<AudioDecoder> decoder;
    // Host-owned macro parameters; must outlive the core.
    MacroParameters* parameters = nullptr;
    // Host facts published in snapshot.capabilities (see protocol/README.md).
    json::Value capabilities;
};

class AppCore {
public:
    explicit AppCore(CoreOptions options);
    ~AppCore();
    AppCore(const AppCore&) = delete;
    AppCore& operator=(const AppCore&) = delete;

    // Protocol entry point. Throws std::runtime_error with a user-facing message.
    json::Value request(const std::string& method, const json::Value& params);
    json::Value snapshot();
    json::Value telemetry();
    std::vector<json::Value> takeAgentEvents();
    json::Value importSample(const std::filesystem::path& file);
    std::string importExtensions() const { return sampleAssets_.importExtensions(); }
    json::Value companionRequest(const std::string& method, const json::Value& params,
                                 const std::function<bool()>& cancelled = {});
    // Merges host facts that change at runtime (e.g. window transparency).
    void setCapability(const std::string& name, json::Value value);

    // Plugin state: identical bytes to the previous JUCE host format.
    std::string saveState();
    void restoreState(std::string bytes);

    AudioRuntime& audio() { return runtime_; }
    bool standalone() const { return options_.standalone; }
    const std::filesystem::path& workspace() const { return workspace_; }

private:
    struct History {
        std::string text;
    };
    struct Document {
        std::string path, text, savedText, gesture, gestureStart;
        std::uint64_t revision = 1, savedRevision = 1;
        bool needsSave = false;
        bool dirty() const { return needsSave || text != savedText; }
        std::vector<History> undo, redo;
    };
    struct CompileView {
        std::uint64_t revision = 0;
        std::string status = "pending";
        json::Array diagnostics, signals, parameters, choices;
        json::Value graph;
        std::optional<Program> program;
    };
    struct Exported {
        std::string binding, name;
        bool bound = false;
        std::string structure;
    };
    struct SequenceSound {
        std::string kind = "active", path, source, playback = "one_shot";
        std::vector<SequenceSound> lanes;
        void rename(const std::string& from, const std::string& to) {
            if (kind == "instrument" && path == from) path = to;
            for (auto& lane : lanes) lane.rename(from, to);
        }
        bool references(const std::string& file) const {
            if (kind == "instrument" && path == file) return true;
            for (const auto& lane : lanes)
                if (lane.references(file)) return true;
            return false;
        }
        void collect(std::vector<std::string>& sources) const {
            if (kind != "lanes") sources.push_back(source);
            for (const auto& lane : lanes) lane.collect(sources);
        }
        bool pinned() const { return kind != "active"; }
    };
    struct OpenDocument {
        Document document;
        CompileView compiled;
        std::array<Exported, 32> exports{};
        std::array<float, 32> macroValues{};
        std::string playableSource;
        std::vector<std::string> selectedSignals;
        int selectedVoice = -1;
        ProbeProjection projection = ProbeProjection::mid;
        SequencePattern sequencer = rollingBassPattern();
        SequenceSound sequenceSound;
    };
    struct Checkpoint {
        std::string prefix;
        std::shared_ptr<const std::string> assets;
    };
    struct Proposal {
        std::string text;
        std::uint64_t revision;
        std::string documentId, conversationId, baseSource;
        bool workspace = false;
    };
    struct CompanionProposal {
        std::string source, before, documentId;
        std::uint64_t baseRevision = 0, appliedRevision = 0;
        bool applied = false, restored = false;
    };

    CoreOptions options_;
    std::unique_ptr<AtomicMacroParameters> ownedParameters_;
    MacroParameters& parameters_;
    AudioRuntime runtime_;
    std::filesystem::path workspace_, settingsFile_;
    // The active document stays in these fields; inactive tabs own all their
    // editing and instrument state in inactiveDocuments_. Only the active
    // instrument runs DSP.
    Document document_;
    CompileView compiled_;
    std::array<Exported, 32> exports_{};
    std::map<std::string, OpenDocument> inactiveDocuments_;
    std::vector<std::string> documentOrder_;
    std::uint64_t nextRevision_ = 1;
    SequencePattern sequencePattern_ = rollingBassPattern(), pinnedSequencePattern_ = rollingBassPattern();
    SequenceSound documentSequenceSound_, sequenceSound_;
    std::string sequenceOwner_, sequenceSoundStatus_ = "ready", sequenceSoundError_;
    std::uint64_t sequenceUnitRevision_ = 1, sequenceUnitGeneration_ = 1, preparedSequenceUnitRevision_ = 0;
    double preparedSequenceRate_ = 0;
    std::uint64_t sequenceRevision_ = 1;
    std::mutex mutex_;
    json::Value preferences_;
    json::Array files_, samples_;
    SampleAssets sampleAssets_;
    std::uint64_t requestedRevision_ = 1, preparedRevision_ = 0;
    double preparedRate_ = 0;
    std::string playableSource_;
    std::vector<std::string> selectedSignals_;
    std::string auditionSignal_;
    int selectedVoice_ = -1;
    ProbeProjection projection_ = ProbeProjection::mid;
    std::uint64_t selectionVersion_ = 0;
    json::Value latestTelemetry_;
    std::mutex stateMutex_;
    std::shared_ptr<const Checkpoint> checkpoint_;
    std::shared_ptr<const std::string> restore_;
    std::string checkpointError_;
    json::Value capabilities_;
    std::string companionId_;
    std::map<std::string, CompanionProposal> companionProposals_;
    AIModels aiModels_;
    json::Value aiProfiles_;
    std::string aiSelectedId_, aiSettingsError_;
    double aiRefreshAt_ = 0;
    std::string aiEndpoint_, aiModel_, apiKey_;
    AssistantHistory assistantHistory_;
    std::string runningConversationId_, runningRequestId_;
    std::atomic<bool> aiBusy_{false};
    std::atomic<std::uint64_t> aiGeneration_{0};
    std::vector<json::Value> agentEvents_;
    std::map<std::string, Proposal> proposals_;
    // Declared last: threads stop before the state they use is destroyed.
    std::jthread worker_, aiWorker_, companionWorker_;

    const BundledFile* bundled(std::string_view name) const;
    void configureWorkspace();
    void worker(std::stop_token);
    void companionLoop(std::stop_token);
    void refreshAI(bool force = false);
    void updateCheckpointLocked();
    void updateMacrosLocked();
    void applyLocked(std::string text, std::string gesture = {}, std::string phase = {});
    void loadPackageLocked(const json::Value&, bool asNewDocument = false);
    json::Value packageLocked(bool includeAssets = true) const;
    json::Value recoveryLocked(bool includeAssets = true) const;
    std::vector<std::string> recoverySourcesLocked() const;
    void loadRecoveryLocked(const json::Value&);
    void cacheActiveLocked();
    void activateLocked(const std::string&);
    void resetActiveAudioLocked();
    void saveLocked(Document&);
    void savePreferencesLocked();
    SequencePattern loadSequenceLocked(const std::string&) const;
    SequenceSound loadSequenceSoundLocked(const std::string&) const;
    SequenceSound parseSequenceSound(const json::Value&, bool capture) const;
    json::Value sequenceStateJSON(const SequencePattern&, const SequenceSound&, bool includeSource = true) const;
    void saveSequenceLocked(const std::string&, const SequencePattern&, const SequenceSound&) const;
    void selectSequenceSoundLocked(SequenceSound, const SequencePattern&, std::string owner);
    void retainSequenceStateLocked();
    void remapSequenceSound(SequenceSound&, const SamplePathMap&) const;
    const SequencePattern& displayedSequenceLocked() const {
        return sequenceSound_.pinned() ? pinnedSequencePattern_ : sequencePattern_;
    }
    std::vector<std::string> packageSourcesLocked() const;
    std::filesystem::path sequenceFile(const std::string& path) const;
    std::string recoveryPrefix(const json::Value& recovery) const;
    void writeRecovery(const std::string& prefix, const std::string& assets) const;
    void emitLocked(json::Value event);
    void sendAI(const json::Value&);
    json::Value conversationViewLocked();
    json::Value readAIInstrumentLocked(const std::string& path);
    bool proposalCurrentLocked(const Proposal&);
    json::Value runAITool(const std::string&, const json::Value&, const std::string& source,
                          const std::function<bool()>& externalCancel = {});
    json::Value postAI(const std::string& endpoint, const std::string& key, const json::Value& payload,
                       const std::function<bool()>& cancelled);
};
}  // namespace flows::app
