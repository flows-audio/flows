#include "AppCore.hpp"

#include "AIEndpoint.hpp"
#include "Analyzer.hpp"
#include "CoreDetail.hpp"
#include "DataflowGraph.hpp"
#include "Files.hpp"
#include "Protocol.hpp"
#include "SequencerJson.hpp"
#include "Text.hpp"
#include "WorkspaceLayout.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <set>

namespace flows::app {
namespace fs = std::filesystem;
using namespace detail;

namespace detail {
std::string readSourceFile(const fs::path& file) {
    const auto name = file.filename().string();
    const auto bytes = files::tryRead(file, maximumSourceBytes);
    if (!bytes) {
        if (files::isFile(file) && files::size(file) > static_cast<std::int64_t>(maximumSourceBytes))
            fail("Instrument exceeds the 1 MiB document limit: " + name);
        fail("Could not read instrument: " + name);
    }
    auto text = text::decodeText(*bytes);
    if (text.size() > maximumSourceBytes) fail("Instrument exceeds the 1 MiB document limit: " + name);
    return text;
}
bool audioNode(const Program& p, ValueId id) {
    std::vector<bool> audio(p.nodes.size());
    for (ValueId i = 0; i <= id; ++i) {
        const auto& n = p.nodes[i];
        audio[i] = n.op == Op::sine || n.op == Op::saw || n.op == Op::unison_sine || n.op == Op::unison_saw ||
                   n.op == Op::phasor || n.op == Op::sampler;
        if (n.op != Op::lfo && n.op != Op::adsr && n.op != Op::key_trigger)
            for (auto input : n.inputs) audio[i] = audio[i] || audio[input];
    }
    return audio[id];
}
ValueId auditionValue(const Program& program, const std::string& name) {
    for (const auto& value : program.named_values)
        if (value.name == name) return audioNode(program, value.value) ? value.value : invalid_value;
    for (const auto& value : program.named_analysis)
        if (value.name == name) {
            const auto& node = program.analysis[value.value];
            if (node.op == AnalysisOp::probe && audioNode(program, node.signal)) return node.signal;
        }
    return invalid_value;
}
std::vector<ValueId> captureRoots(const Program& program, std::uint32_t analysis) {
    std::set<ValueId> roots;
    std::vector<bool> visited(program.analysis.size());
    std::vector<std::uint32_t> pending{analysis};
    while (!pending.empty()) {
        const auto id = pending.back();
        pending.pop_back();
        if (id >= program.analysis.size() || visited[id]) continue;
        visited[id] = true;
        const auto& node = program.analysis[id];
        if (node.op == AnalysisOp::probe) roots.insert(node.signal);
        pending.insert(pending.end(), node.inputs.begin(), node.inputs.end());
    }
    return {roots.begin(), roots.end()};
}
ProbeSelectionResult selectionForNames(const Program& program, std::uint64_t revision,
                                       const std::vector<std::string>& names, int voice, ProbeProjection projection) {
    std::vector<ValueId> values;
    std::vector<std::string> analyses;
    for (const auto& name : names) {
        bool found = false;
        for (const auto& value : program.named_values)
            if (value.name == name) {
                values.push_back(value.value);
                found = true;
                break;
            }
        for (const auto& value : program.named_analysis)
            if (value.name == name) {
                analyses.push_back(name);
                found = true;
                break;
            }
        if (!found) {
            ProbeSelectionResult result;
            result.error = "Unknown signal: " + name;
            return result;
        }
    }
    return makeProbeSelection(program, revision, values, voice, analyses, false, projection);
}
const char* projectionName(ProbeProjection projection) {
    switch (projection) {
        case ProbeProjection::mid: return "mid";
        case ProbeProjection::left: return "left";
        case ProbeProjection::right: return "right";
        case ProbeProjection::side: return "side";
    }
    return "mid";
}
ProbeProjection parseProjection(const json::Value& value) {
    const auto name = value.asString();
    if (name == "mid") return ProbeProjection::mid;
    if (name == "left") return ProbeProjection::left;
    if (name == "right") return ProbeProjection::right;
    if (name == "side") return ProbeProjection::side;
    fail("Audio projection must be mid, left, right or side");
}
const char* analysisShape(AnalysisShape shape) {
    switch (shape) {
        case AnalysisShape::waveform: return "waveform";
        case AnalysisShape::scalar: return "scalar";
        case AnalysisShape::spectrum: return "spectrum";
        case AnalysisShape::mel_spectrum: return "mel_spectrum";
    }
    return "waveform";
}
double sourceScale(const std::string& value) {
    if (value.ends_with("khz")) return 1000;
    if (value.ends_with("ms")) return .001;
    return 1;
}
std::string bindingStructure(const Program& program) { return text::sha256Hex(dsp_structure_key(program)); }
json::Value floatArray(const std::vector<float>& values) {
    json::Array result;
    result.reserve(values.size());
    for (auto value : values) result.emplace_back(value);
    return json::Value(std::move(result));
}
json::Value graphMetadata(const DataflowGraph& graph, const std::string& source) {
    json::Array nodes, edges;
    for (const auto& node : graph.nodes) {
        nodes.push_back(json::object({{"id", node.id},
                                      {"label", node.label},
                                      {"op", node.op},
                                      {"kind", node.kind},
                                      {"signal", node.signal},
                                      {"from", text::utf16Offset(source, node.span.offset)},
                                      {"to", text::utf16Offset(source, node.span.offset + node.span.length)},
                                      {"parameterIds", json::array(node.parameterIds)},
                                      {"choiceIds", json::array(node.choiceIds)}}));
    }
    for (const auto& edge : graph.edges)
        edges.push_back(json::object({{"id", edge.id},
                                      {"source", edge.source},
                                      {"target", edge.target},
                                      {"kind", edge.kind},
                                      {"label", edge.label}}));
    return json::object({{"nodes", std::move(nodes)}, {"edges", std::move(edges)}, {"output", graph.output}});
}
json::Value emptyGraph() { return graphMetadata({}, {}); }
json::Value preferencesWithPatch(const json::Value& current, const json::Value& patch) {
    const auto value = [&](const char* name) -> const json::Value& {
        return patch.has(name) ? patch[name] : current[name];
    };
    const auto textValue = [&](const char* name, const char* fallback) {
        const auto& v = value(name);
        return v.isString() && !v.string().empty() ? text::codePointSubstring(v.string(), 0, 256) : std::string(fallback);
    };
    const auto number = [&](const char* name, double fallback, double minimum, double maximum) {
        const auto& v = value(name);
        if (!v.isNumber() || !std::isfinite(v.asDouble())) return fallback;
        return std::clamp(v.asDouble(), minimum, maximum);
    };
    const auto visibility = [&](const char* name) {
        const auto &patched = patch[name], &saved = current[name];
        if (patched.isBool()) return patched.asBool();
        return saved.isBool() ? saved.asBool() : true;
    };
    // Old settings stay explicitly version 1 so the UI can migrate their small
    // font once. A new workspace already has the complete version 2 defaults.
    const auto layoutVersion = number("layoutVersion", current.isObject() ? 1 : 2, 1, 2);
    return json::object({{"theme", textValue("theme", "graphite")},
                         {"opacity", number("opacity", 1, .2, 1)},
                         {"split", textValue("split", "none")},
                         {"fontFamily", textValue("fontFamily", "SFMono-Regular")},
                         {"fontSize", static_cast<int>(number("fontSize", 16, 10, 32))},
                         {"bpm", number("bpm", 138, 20, 300)},
                         {"leftWidth", number("leftWidth", 240, 120, 520)},
                         {"rightWidth", number("rightWidth", 320, 180, 600)},
                         {"flowHeight", number("flowHeight", 320, 80, 800)},
                         {"splitRatio", number("splitRatio", .5, .2, .8)},
                         {"showExplorer", visibility("showExplorer")},
                         {"showAssistant", visibility("showAssistant")},
                         {"showDataflow", visibility("showDataflow")},
                         {"showKeyboard", visibility("showKeyboard")},
                         {"layoutVersion", static_cast<int>(layoutVersion)}});
}
std::string samplerSource(const std::string& samplePath, bool probe, const std::string& playback) {
    std::string source =
        "import standard as std\nfrom lib.sample import ClassicSampler\n";
    if (!probe) source += "from lib.env import ClassicADSR\n";
    source += "\nInst.sigstream(std.event.keys.poly()) as key:\n    sampler = ClassicSampler.make()\n"
              "    raw = sampler(file: " +
              text::quoteJsonString(samplePath) +
              ", trigger: key.trigger, pitch: key.pitch, root: C3, level: -6db)\n";
    if (probe) return source + "    p_raw = probe(raw)\n    return raw * key.velocity\n";
    if (playback == "gated")
        source +=
            "    env = ClassicADSR.make()\n    amp = env(gate: key.gate, trigger: key.trigger, A: 0ms, D: 0ms, S: 1, "
            "R: 5ms)\n    return raw * amp * key.velocity\n";
    else
        source += "    return raw * key.velocity\n";
    return source;
}
}  // namespace detail

AppCore::AppCore(CoreOptions options)
    : options_(std::move(options)),
      ownedParameters_(options_.parameters ? nullptr : std::make_unique<AtomicMacroParameters>()),
      parameters_(options_.parameters ? *options_.parameters : static_cast<MacroParameters&>(*ownedParameters_)),
      runtime_(parameters_),
      aiModels_(options_.settingsDirectory, options_.secrets) {
    capabilities_ = protocolCapabilities(options_.capabilities);
    // Hosts may poll telemetry before the worker's first analysis frame.
    latestTelemetry_ = json::object({{"sequence", 0},
                                     {"programRevision", 0},
                                     {"signals", json::Value::array()},
                                     {"gap", true},
                                     {"analysisReady", false},
                                     {"status", "waiting for selected instrument"}});
    configureWorkspace();
    updateCheckpointLocked();
    worker_ = std::jthread([this](std::stop_token stop) { worker(stop); });
    companionId_ = text::randomId();
    if (!options_.liveBridgeDirectory.empty())
        companionWorker_ = std::jthread([this](std::stop_token stop) { companionLoop(stop); });
}
AppCore::~AppCore() {
    companionWorker_.request_stop();
    if (companionWorker_.joinable()) companionWorker_.join();
    aiGeneration_.fetch_add(1);
    aiWorker_.request_stop();
    worker_.request_stop();
    if (aiWorker_.joinable()) aiWorker_.join();
    if (worker_.joinable()) worker_.join();
    if (options_.standalone) try {
            const auto prefix = recoveryPrefix(recoveryLocked(false));
            writeRecovery(prefix, *sampleAssets_.serializedBundle(recoverySourcesLocked()));
        } catch (...) { /* Keep the last complete recovery file if an asset cannot be read. */
        }
}
const BundledFile* AppCore::bundled(std::string_view name) const {
    for (const auto& file : options_.bundled)
        if (files::fileName(file.name) == name) return &file;
    return nullptr;
}
fs::path AppCore::sequenceFile(const std::string& path) const {
    return workspace_ / ".sequencers" / (path + ".json");
}
std::string AppCore::recoveryPrefix(const json::Value& recovery) const {
    auto text = json::stringify(recovery);
    text.pop_back();  // drop the closing brace; assets follow
    return text + ",\"assets\":";
}
void AppCore::writeRecovery(const std::string& prefix, const std::string& assets) const {
    if (!files::createDirectories(workspace_)) fail("Could not create the recovery folder");
    try {
        files::writeAtomic(workspace_ / ".session.flowsinst", {prefix, assets, "}"});
    } catch (const std::exception&) {
        fail("Could not write instrument recovery");
    }
}
void AppCore::emitLocked(json::Value event) {
    if (agentEvents_.size() >= 128) agentEvents_.erase(agentEvents_.begin());
    agentEvents_.push_back(std::move(event));
}
void AppCore::setCapability(const std::string& name, json::Value value) {
    std::lock_guard lock(mutex_);
    capabilities_.set(name, std::move(value));
}

void AppCore::configureWorkspace() {
    workspace_ = options_.workspace;
    files::createDirectories(workspace_);
    sampleAssets_.setRoot(workspace_);
    sampleAssets_.setDecoder(options_.decoder);
    for (const auto& file : options_.bundled) {
        const auto name = files::fileName(file.name);
        if (name.ends_with(".wav")) {
            try {
                sampleAssets_.installBundled("Samples/" + name, file.bytes);
            } catch (...) {
            }
            continue;
        }
        if (!name.ends_with(".flows")) continue;
        const auto target = workspace_ / "examples" / name;
        files::createDirectories(target.parent_path());
        if (files::isFile(workspace_ / name)) continue;
        if (!files::exists(target)) try {
                files::writeAtomic(target, file.bytes);
            } catch (...) {
            }
    }
    organizeWorkspace(workspace_);
    assistantHistory_.setWorkspace(workspace_);
    settingsFile_ = workspace_ / ".flows-settings.json";
    json::Value settings;
    try {
        if (files::isFile(settingsFile_) && files::size(settingsFile_) < 65536)
            settings = json::parseBounded(files::read(settingsFile_, 65536), 65536);
    } catch (...) {
    }
    preferences_ = preferencesWithPatch(settings["preferences"]);
    compiled_.graph = emptyGraph();
    try {
        aiModels_.migrate(json::str(settings, "endpoint"), json::str(settings, "model"));
    } catch (const std::exception& error) {
        aiSettingsError_ = error.what();
    }
    refreshAI(true);
    document_.path = "examples/trance-rolling-bass.flows";
    sequencePattern_ = loadSequenceLocked(document_.path);
    documentSequenceSound_ = loadSequenceSoundLocked(document_.path);
    selectSequenceSoundLocked(documentSequenceSound_, sequencePattern_, document_.path);
    if (const auto bytes = files::tryRead(workspace_ / document_.path, maximumSourceBytes))
        document_.text = text::decodeText(*bytes);
    document_.savedText = document_.text;
    documentOrder_.push_back(document_.path);
    if (options_.standalone) try {
            const auto recoveryFile = workspace_ / ".session.flowsinst";
            if (files::isFile(recoveryFile) && files::size(recoveryFile) <= static_cast<std::int64_t>(maximumRecoveryBytes)) {
                const auto recovery =
                    json::parseBounded(files::read(recoveryFile, maximumRecoveryBytes), maximumRecoveryBytes);
                if (json::str(recovery, "format") == "flows.workspace")
                    loadRecoveryLocked(recovery);
                else
                    loadPackageLocked(recovery);
            }
        } catch (...) { /* Keep the bundled instrument if recovery is corrupt. */
        }
    runtime_.bpm.store(preferences_["bpm"].asDouble());
    samples_ = sampleAssets_.catalogue();
    refreshFilesLocked();
}

void AppCore::cacheActiveLocked() {
    if (document_.path.empty()) return;
    document_.gesture.clear();
    document_.gestureStart.clear();
    OpenDocument cached;
    cached.document = std::move(document_);
    cached.compiled = std::move(compiled_);
    cached.exports = exports_;
    cached.playableSource = std::move(playableSource_);
    cached.sequencer = sequencePattern_;
    cached.sequenceSound = documentSequenceSound_;
    cached.selectedSignals = std::move(selectedSignals_);
    cached.selectedVoice = selectedVoice_;
    cached.projection = projection_;
    for (int i = 0; i < 32; ++i) cached.macroValues[static_cast<std::size_t>(i)] = parameters_.get(i);
    auto id = cached.document.path;
    inactiveDocuments_[id] = std::move(cached);
    document_ = {};
}
void AppCore::resetActiveAudioLocked() {
    runtime_.bpm.store(preferences_["bpm"].asDouble());
    const bool clean = !document_.dirty();
    document_.revision = ++nextRevision_;
    if (clean) document_.savedRevision = document_.revision;
    requestedRevision_ = document_.revision;
    preparedRevision_ = 0;
    compiled_.status = document_.path.empty() ? "idle" : "pending";
    if (document_.path.empty()) {
        compiled_.revision = 0;
        compiled_.graph = emptyGraph();
    }
    // A program from a different tab is never the fallback for this tab.
    runtime_.audioDocumentRevision.store(document_.revision, std::memory_order_release);
    runtime_.sourcePlayable.store(false, std::memory_order_release);
    runtime_.panic.store(true);
    runtime_.session.requestPanic();
    if (!sequenceSound_.pinned()) {
        runtime_.demo.store(false);
        runtime_.sequencePatterns.push(sequencePattern_);
        runtime_.sequencePanic.store(true);
        runtime_.sequencePlayhead.store(-1);
    }
    ++sequenceRevision_;
    auditionSignal_.clear();
    runtime_.sequenceAuditionMuted.store(false);
    ++selectionVersion_;
    latestTelemetry_ = json::object({{"sequence", 0},
                                     {"programRevision", 0},
                                     {"signals", json::Value::array()},
                                     {"gap", true},
                                     {"analysisReady", false},
                                     {"status", "waiting for selected instrument"}});
}
void AppCore::activateLocked(const std::string& id) {
    if (!id.empty() && id == document_.path) return;
    if (!id.empty() && inactiveDocuments_.find(id) == inactiveDocuments_.end()) fail("This instrument is not open");
    cacheActiveLocked();
    if (!id.empty()) {
        auto it = inactiveDocuments_.find(id);
        auto next = std::move(it->second);
        inactiveDocuments_.erase(it);
        document_ = std::move(next.document);
        compiled_ = std::move(next.compiled);
        exports_ = next.exports;
        playableSource_ = std::move(next.playableSource);
        sequencePattern_ = next.sequencer;
        documentSequenceSound_ = std::move(next.sequenceSound);
        selectedSignals_ = std::move(next.selectedSignals);
        selectedVoice_ = next.selectedVoice;
        projection_ = next.projection;
        for (int i = 0; i < 32; ++i) parameters_.set(i, next.macroValues[static_cast<std::size_t>(i)]);
    } else {
        document_ = {};
        compiled_ = {};
        exports_ = {};
        playableSource_.clear();
        sequencePattern_ = rollingBassPattern();
        documentSequenceSound_ = {};
        selectedSignals_.clear();
        selectedVoice_ = -1;
        projection_ = ProbeProjection::mid;
        for (int i = 0; i < 32; ++i) parameters_.set(i, .5f);
    }
    if (!sequenceSound_.pinned()) selectSequenceSoundLocked(documentSequenceSound_, sequencePattern_, document_.path);
    resetActiveAudioLocked();
    updateCheckpointLocked();
}

AppCore::SequenceSound AppCore::parseSequenceSound(const json::Value& value, bool capture) const {
    if (value.isNull()) return {};
    if (!value.isObject()) fail("Sequencer sound must be an object");
    SequenceSound sound;
    sound.kind = json::str(value, "kind");
    if (sound.kind == "active") return sound;
    if (sound.kind == "lanes") {
        const auto* lanes = value["lanes"].getArray();
        if (!lanes || lanes->empty() || lanes->size() > sequencerRows) fail("Choose one sound for each of 1–8 lanes");
        for (const auto& lane : *lanes) {
            if (json::str(lane, "kind") == "lanes") fail("Lane sounds cannot be nested");
            sound.lanes.push_back(parseSequenceSound(lane, capture));
        }
        return sound;
    }
    if (sound.kind != "instrument" && sound.kind != "sample")
        fail("Choose active, instrument or sample as the sequencer sound kind");
    sound.path = json::str(value, "path");
    if (sound.path.empty() || text::codePointCount(sound.path) > 2048 || text::containsAnyOf(sound.path, "\\\r\n") ||
        sound.path.front() == '/' || sound.path.front() == '.' || sound.path.find("/../") != std::string::npos ||
        sound.path.find("/./") != std::string::npos)
        fail("Choose a sound file inside this workspace");
    const auto file = files::child(workspace_, sound.path);
    if (!file || (sound.kind == "instrument" && !files::hasExtension(sound.path, "flows")) ||
        (sound.kind == "sample" && !files::hasExtension(sound.path, "wav;aif;aiff;aifc")))
        fail("Invalid sequencer sound path");
    if (sound.kind == "sample") {
        sound.playback = json::str(value, "playback");
        if (sound.playback.empty()) sound.playback = "one_shot";
        if (sound.playback != "one_shot" && sound.playback != "gated") fail("Sample playback must be one_shot or gated");
        if (capture && !sampleAssets_.isManaged(sound.path)) fail("Choose an imported sample from the Samples library");
        sound.source = samplerSource(sound.path, false, sound.playback);
    } else if (capture) {
        if (!files::isInsideResolved(workspace_, *file))
            fail("Sequencer sound escapes this workspace through a symbolic link");
        if (sound.path == document_.path)
            sound.source = document_.text;
        else if (const auto found = inactiveDocuments_.find(sound.path); found != inactiveDocuments_.end())
            sound.source = found->second.document.text;
        else
            sound.source = readSourceFile(*file);
    } else
        sound.source = json::str(value, "source");
    if (sound.source.empty() || sound.source.size() > maximumSourceBytes)
        fail("Sequencer sound needs a source snapshot of at most 1 MiB");
    return sound;
}
json::Value AppCore::sequenceStateJSON(const SequencePattern& pattern, const SequenceSound& sound,
                                       bool includeSource) const {
    auto result = sequenceJSON(pattern);
    const auto encode = [&](const auto& self, const SequenceSound& item) -> json::Value {
        auto unit = json::object({{"kind", item.kind}});
        if (item.kind == "lanes") {
            json::Array lanes;
            for (const auto& lane : item.lanes) lanes.push_back(self(self, lane));
            unit.set("lanes", std::move(lanes));
        } else if (item.pinned()) {
            if (item.kind == "sample") unit.set("playback", item.playback);
            unit.set("path", item.path);
            if (includeSource) unit.set("source", item.source);
        }
        return unit;
    };
    result.set("sound", encode(encode, sound));
    return result;
}
SequencePattern AppCore::loadSequenceLocked(const std::string& path) const {
    const auto file = sequenceFile(path);
    try {
        if (files::isInside(workspace_, file) && files::isFile(file) &&
            files::size(file) <= static_cast<std::int64_t>(maximumSequenceBytes))
            return parseSequence(json::parseBounded(files::read(file, maximumSequenceBytes), maximumSequenceBytes));
    } catch (...) { /* A damaged optional pattern must not prevent source recovery. */
    }
    return rollingBassPattern();
}
AppCore::SequenceSound AppCore::loadSequenceSoundLocked(const std::string& path) const {
    const auto file = sequenceFile(path);
    try {
        if (files::isInside(workspace_, file) && files::isFile(file) &&
            files::size(file) <= static_cast<std::int64_t>(maximumSequenceBytes))
            return parseSequenceSound(
                json::parseBounded(files::read(file, maximumSequenceBytes), maximumSequenceBytes)["sound"], false);
    } catch (...) {
    }
    return {};
}
void AppCore::saveSequenceLocked(const std::string& path, const SequencePattern& sequence,
                                 const SequenceSound& sound) const {
    const auto file = sequenceFile(path);
    if (path.empty() || !files::isInside(workspace_, file)) fail("Could not save the sequencer pattern");
    const auto content = json::stringify(sequenceStateJSON(sequence, sound));
    if (content.size() > maximumSequenceBytes) fail("Sequencer pattern and source exceed the 8 MiB metadata limit");
    if (files::isDirectory(file)) fail("Could not save the sequencer pattern");
    try {
        files::writeAtomic(file, content);
    } catch (const std::exception&) {
        fail("Could not write the sequencer pattern");
    }
}
void AppCore::retainSequenceStateLocked() {
    if (!sequenceSound_.pinned()) {
        documentSequenceSound_ = {};
        return;
    }
    if (sequenceOwner_ == document_.path) {
        sequencePattern_ = pinnedSequencePattern_;
        documentSequenceSound_ = sequenceSound_;
    } else if (auto found = inactiveDocuments_.find(sequenceOwner_); found != inactiveDocuments_.end()) {
        found->second.sequencer = pinnedSequencePattern_;
        found->second.sequenceSound = sequenceSound_;
    }
}
void AppCore::selectSequenceSoundLocked(SequenceSound sound, const SequencePattern& pattern, std::string owner) {
    const bool newUnit = json::stringify(sequenceStateJSON(pattern, sound, false)["sound"]) !=
                         json::stringify(sequenceStateJSON(pattern, sequenceSound_, false)["sound"]);
    const bool changed = newUnit || json::stringify(sequenceStateJSON(pattern, sound)["sound"]) !=
                                        json::stringify(sequenceStateJSON(pattern, sequenceSound_)["sound"]);
    sequenceSound_ = std::move(sound);
    if (sequenceSound_.pinned()) {
        pinnedSequencePattern_ = pattern;
        sequenceOwner_ = std::move(owner);
        retainSequenceStateLocked();
    } else {
        sequencePattern_ = pattern;
        documentSequenceSound_ = {};
        sequenceOwner_ = document_.path;
    }
    runtime_.sequencePinned.store(sequenceSound_.pinned(), std::memory_order_release);
    runtime_.sequencePatterns.push(displayedSequenceLocked());
    if (changed) {
        ++sequenceUnitRevision_;
        if (newUnit) {
            ++sequenceUnitGeneration_;
            runtime_.sequenceMinimumRevision.store(sequenceUnitRevision_, std::memory_order_release);
            runtime_.sequenceTargetRevision.store(sequenceUnitRevision_, std::memory_order_release);
            runtime_.sequencePanic.store(true);
            runtime_.sequenceSession.requestPanic();
        }
        sequenceSoundError_.clear();
        sequenceSoundStatus_ = sequenceSound_.pinned() ? "pending" : "ready";
    }
}
void AppCore::remapSequenceSound(SequenceSound& sound, const SamplePathMap& paths) const {
    for (auto& lane : sound.lanes) remapSequenceSound(lane, paths);
    sound.source = remapSamplePaths(sound.source, paths);
    if (sound.kind == "sample")
        if (const auto found = paths.find(sound.path); found != paths.end()) sound.path = found->second;
    if (sound.source.size() > maximumSourceBytes) fail("Remapped sequencer source exceeds 1 MiB");
}
std::vector<std::string> AppCore::packageSourcesLocked() const {
    std::vector<std::string> sources{playableSource_, document_.text};
    (sequenceSound_.pinned() ? sequenceSound_ : documentSequenceSound_).collect(sources);
    return sources;
}
// Requests that add, rename or remove workspace files list them at once
// instead of waiting for the worker's periodic scan.
void AppCore::refreshFilesLocked() {
    files_ = workspaceFiles(workspace_);
    ++listingGeneration_;
}
void AppCore::saveLocked(Document& document) {
    if (document.path.empty()) fail("No instrument is open");
    const auto file = files::child(workspace_, document.path);
    if (!file) fail("Could not save instrument");
    const bool created = !files::exists(*file);
    try {
        files::writeAtomic(*file, document.text);
    } catch (const std::exception&) {
        fail("Could not save instrument");
    }
    if (created) refreshFilesLocked();
    const auto cached = inactiveDocuments_.find(document.path);
    const bool active = document.path == document_.path;
    saveSequenceLocked(document.path, active ? sequencePattern_ : cached->second.sequencer,
                       active ? documentSequenceSound_ : cached->second.sequenceSound);
    document.savedText = document.text;
    document.savedRevision = document.revision;
    document.needsSave = false;
}
void AppCore::applyLocked(std::string value, std::string gesture, std::string phase) {
    if (value.size() > maximumSourceBytes) fail("Source exceeds the 1 MiB document limit");
    if (phase == "cancel" && gesture == document_.gesture) {
        value = document_.gestureStart;
        if (!document_.undo.empty()) document_.undo.pop_back();
    } else if (value != document_.text && (gesture.empty() || gesture != document_.gesture)) {
        document_.undo.push_back({document_.text});
        if (document_.undo.size() > 256) document_.undo.erase(document_.undo.begin());
        document_.redo.clear();
        document_.gestureStart = document_.text;
    }
    if (value != document_.text) {
        document_.text = std::move(value);
        document_.revision = ++nextRevision_;
        requestedRevision_ = document_.revision;
        compiled_.status = "pending";
    }
    document_.gesture = !phase.empty() ? std::string() : gesture;
    updateCheckpointLocked();
}
json::Value AppCore::packageLocked(bool includeAssets) const {
    json::Array slots;
    for (int i = 0; i < 32; ++i) {
        const auto& exported = exports_[static_cast<std::size_t>(i)];
        slots.push_back(json::object({{"slot", i + 1},
                                      {"bindingId", exported.binding},
                                      {"name", exported.name},
                                      {"value", parameters_.get(i)},
                                      {"structure", exported.structure}}));
    }
    auto package = json::object({{"format", "flows.instrument"},
                                 {"version", 1},
                                 {"empty", document_.path.empty()},
                                 {"path", document_.path},
                                 {"source", document_.text},
                                 {"playableSource", playableSource_},
                                 {"macros", std::move(slots)},
                                 {"preferences", preferences_},
                                 {"sequencer", sequenceStateJSON(displayedSequenceLocked(), sequenceSound_)}});
    if (includeAssets) package.set("assets", sampleAssets_.bundle(packageSourcesLocked(), true));
    return package;
}
void AppCore::updateCheckpointLocked() {
    try {
        auto assets = sampleAssets_.serializedBundle(packageSourcesLocked());
        auto prefix = recoveryPrefix(packageLocked(false));
        auto checkpoint = std::make_shared<Checkpoint>();
        checkpoint->prefix = std::move(prefix);
        checkpoint->assets = std::move(assets);
        {
            std::lock_guard lock(stateMutex_);
            checkpoint_ = std::move(checkpoint);
        }
        checkpointError_.clear();
    } catch (const std::exception& e) {
        const auto error = std::string("State assets could not be packed: ") + e.what();
        if (checkpointError_ != error) {
            checkpointError_ = error;
            agentEvents_.push_back(json::object({{"status", "failed"}, {"error", error}}));
        }
    }
}
json::Value AppCore::recoveryLocked(bool includeAssets) const {
    json::Array documents;
    std::vector<std::string> sources;
    const auto append = [&](const Document& d, const std::string& playable, const std::array<Exported, 32>& exports,
                            const std::array<float, 32>& values, const SequencePattern& sequence,
                            const SequenceSound& sound) {
        sources.push_back(playable);
        sources.push_back(d.text);
        sources.push_back(d.savedText);
        sources.push_back(sound.source);
        json::Array slots;
        for (std::size_t i = 0; i < 32; ++i)
            slots.push_back(json::object({{"slot", static_cast<int>(i + 1)},
                                          {"bindingId", exports[i].binding},
                                          {"name", exports[i].name},
                                          {"value", values[i]},
                                          {"structure", exports[i].structure}}));
        documents.push_back(json::object({{"path", d.path},
                                          {"source", d.text},
                                          {"savedSource", d.savedText},
                                          {"needsSave", d.needsSave},
                                          {"playableSource", playable},
                                          {"macros", std::move(slots)},
                                          {"sequencer", sequenceStateJSON(sequence, sound)}}));
    };
    for (const auto& id : documentOrder_) {
        if (id == document_.path) {
            std::array<float, 32> values{};
            for (int i = 0; i < 32; ++i) values[static_cast<std::size_t>(i)] = parameters_.get(i);
            append(document_, playableSource_, exports_, values, sequencePattern_, documentSequenceSound_);
        } else if (const auto it = inactiveDocuments_.find(id); it != inactiveDocuments_.end()) {
            const auto& d = it->second;
            append(d.document, d.playableSource, d.exports, d.macroValues, d.sequencer, d.sequenceSound);
        }
    }
    auto recovery = json::object({{"format", "flows.workspace"},
                                  {"version", 1},
                                  {"activeDocument", document_.path},
                                  {"documents", std::move(documents)},
                                  {"preferences", preferences_}});
    if (sequenceSound_.pinned()) {
        recovery.set("pinnedSequencer", json::object({{"ownerDocumentId", sequenceOwner_},
                                                      {"pattern", sequenceStateJSON(pinnedSequencePattern_, sequenceSound_)}}));
        sequenceSound_.collect(sources);
    }
    if (includeAssets) recovery.set("assets", sampleAssets_.bundle(sources));
    return recovery;
}
std::vector<std::string> AppCore::recoverySourcesLocked() const {
    std::vector<std::string> sources;
    for (const auto& id : documentOrder_) {
        if (id == document_.path) {
            sources.push_back(playableSource_);
            sources.push_back(document_.text);
            sources.push_back(document_.savedText);
            documentSequenceSound_.collect(sources);
        } else if (const auto found = inactiveDocuments_.find(id); found != inactiveDocuments_.end()) {
            sources.push_back(found->second.playableSource);
            sources.push_back(found->second.document.text);
            sources.push_back(found->second.document.savedText);
            found->second.sequenceSound.collect(sources);
        }
    }
    if (sequenceSound_.pinned()) sequenceSound_.collect(sources);
    return sources;
}
void AppCore::loadRecoveryLocked(const json::Value& recovery) {
    if (json::str(recovery, "format") != "flows.workspace" || recovery["version"].asInt() != 1)
        fail("Unsupported workspace recovery");
    const auto* entries = recovery["documents"].getArray();
    if (!entries || entries->size() > maximumOpenDocuments) fail("Invalid workspace document list");
    // Reject malformed document metadata before publishing any embedded files.
    std::set<std::string> recoveredPaths;
    for (const auto& entry : *entries) {
        const auto path = json::str(entry, "path");
        const auto file = files::child(workspace_, path);
        if (path.empty() || !file || !files::hasExtension(path, "flows") ||
            !recoveredPaths.insert(files::relative(workspace_, *file)).second ||
            json::str(entry, "source").size() > maximumSourceBytes ||
            json::str(entry, "savedSource").size() > maximumSourceBytes ||
            json::str(entry, "playableSource").size() > maximumSourceBytes)
            fail("Invalid recovered instrument metadata");
        if (entry.has("sequencer")) {
            (void)parseSequence(entry["sequencer"]);
            (void)parseSequenceSound(entry["sequencer"]["sound"], false);
        }
    }
    const auto& pin = recovery["pinnedSequencer"];
    SequenceSound recoveredSound;
    SequencePattern recoveredPattern;
    if (pin.isObject()) {
        recoveredPattern = parseSequence(pin["pattern"]);
        recoveredSound = parseSequenceSound(pin["pattern"]["sound"], false);
    }
    const auto assetPaths = sampleAssets_.restore(recovery["assets"]);
    remapSequenceSound(recoveredSound, assetPaths);
    std::map<std::string, OpenDocument> documents;
    std::vector<std::string> order;
    for (const auto& entry : *entries) {
        OpenDocument opened;
        auto& d = opened.document;
        if (entry.has("sequencer")) {
            opened.sequencer = parseSequence(entry["sequencer"]);
            opened.sequenceSound = parseSequenceSound(entry["sequencer"]["sound"], false);
            remapSequenceSound(opened.sequenceSound, assetPaths);
        }
        d.path = json::str(entry, "path");
        d.text = json::str(entry, "source");
        d.savedText = json::str(entry, "savedSource");
        d.needsSave = entry["needsSave"].asBool();
        const auto originalSource = d.text;
        d.text = remapSamplePaths(d.text, assetPaths);
        d.needsSave = d.needsSave || d.text != originalSource;
        opened.playableSource = remapSamplePaths(json::str(entry, "playableSource"), assetPaths);
        opened.macroValues.fill(.5f);
        const auto file = files::child(workspace_, d.path);
        if (d.path.empty() || !file || !files::hasExtension(d.path, "flows") || d.text.size() > maximumSourceBytes ||
            d.savedText.size() > maximumSourceBytes || opened.playableSource.size() > maximumSourceBytes)
            fail("Invalid recovered instrument");
        d.path = files::relative(workspace_, *file);
        if (documents.contains(d.path)) fail("Duplicate recovered instrument");
        if (!d.dirty()) {
            if (files::isFile(*file)) {
                try {
                    auto current = readSourceFile(*file);
                    if (current != d.text) opened.playableSource.clear();
                    d.text = std::move(current);
                    d.savedText = d.text;
                } catch (const std::exception& error) {
                    // Keep the complete recovered source if the disk file is
                    // unreadable or oversized; never load a truncated prefix.
                    d.needsSave = true;
                    agentEvents_.push_back(json::object(
                        {{"status", "failed"}, {"error", std::string("Recovered source retained: ") + error.what()}}));
                }
            } else
                d.needsSave = true;
        }
        d.revision = ++nextRevision_;
        d.savedRevision = !d.dirty() ? d.revision : 0;
        opened.compiled.graph = emptyGraph();
        if (const auto* slots = entry["macros"].getArray())
            for (const auto& slot : *slots) {
                const auto i = slot["slot"].asInt() - 1;
                if (i < 0 || i >= 32) continue;
                opened.exports[static_cast<std::size_t>(i)] = {json::str(slot, "bindingId"), json::str(slot, "name"),
                                                               false, json::str(slot, "structure")};
                const auto value = static_cast<float>(slot["value"].asDouble());
                opened.macroValues[static_cast<std::size_t>(i)] = std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : .5f;
            }
        order.push_back(d.path);
        const auto path = d.path;
        documents[path] = std::move(opened);
    }
    auto active = json::str(recovery, "activeDocument");
    if (!order.empty() && !documents.contains(active)) active = order.front();
    document_ = {};
    compiled_ = {};
    inactiveDocuments_ = std::move(documents);
    documentOrder_ = std::move(order);
    const auto& prefs = recovery["preferences"];
    if (prefs.isObject()) preferences_ = preferencesWithPatch(prefs);
    sequenceSound_ = {};
    runtime_.sequencePinned.store(false);
    runtime_.demo.store(false);
    runtime_.sequencePanic.store(true);
    runtime_.sequenceSession.requestPanic();
    activateLocked(active);
    if (recoveredSound.pinned()) {
        auto owner = json::str(pin, "ownerDocumentId");
        if (owner.empty() || !files::child(workspace_, owner)) owner = active;
        selectSequenceSoundLocked(std::move(recoveredSound), recoveredPattern, owner);
    }
}
void AppCore::loadPackageLocked(const json::Value& package, bool asNewDocument) {
    if (json::str(package, "format") != "flows.instrument" || package["version"].asInt() != 1)
        fail("Unsupported InstrumentPackage");
    if (package["empty"].asBool()) {
        if (asNewDocument) fail("This package contains no instrument");
        document_ = {};
        compiled_ = {};
        inactiveDocuments_.clear();
        documentOrder_.clear();
        const auto& prefs = package["preferences"];
        if (prefs.isObject()) preferences_ = preferencesWithPatch(prefs);
        sequenceSound_ = {};
        runtime_.sequencePinned.store(false);
        runtime_.demo.store(false);
        runtime_.sequencePanic.store(true);
        runtime_.sequenceSession.requestPanic();
        activateLocked({});
        return;
    }
    OpenDocument opened;
    auto& d = opened.document;
    if (package.has("sequencer")) {
        opened.sequencer = parseSequence(package["sequencer"]);
        opened.sequenceSound = parseSequenceSound(package["sequencer"]["sound"], false);
    }
    d.text = json::str(package, "source");
    opened.playableSource = json::str(package, "playableSource");
    if (d.text.size() > maximumSourceBytes || opened.playableSource.size() > maximumSourceBytes)
        fail("Instrument source exceeds 1 MiB");
    if (asNewDocument && documentOrder_.size() >= maximumOpenDocuments)
        fail("Close an instrument before opening another (32 open instruments maximum)");
    d.path = files::fileName(json::str(package, "path"));
    if (!d.path.ends_with(".flows")) d.path = "Imported.flows";
    if (asNewDocument && (files::exists(workspace_ / d.path) ||
                          std::find(documentOrder_.begin(), documentOrder_.end(), d.path) != documentOrder_.end())) {
        const auto base = files::stem(d.path);
        for (int suffix = 1;; ++suffix) {
            const auto candidate = base + " (import " + std::to_string(suffix) + ").flows";
            if (!files::exists(workspace_ / candidate) &&
                std::find(documentOrder_.begin(), documentOrder_.end(), candidate) == documentOrder_.end()) {
                d.path = candidate;
                break;
            }
        }
    }
    opened.sequenceSound.rename(json::str(package, "path"), d.path);
    // Explicit import opens a separate draft. DAW state recall replaces the
    // session with its portable active instrument instead of accumulating tabs.
    d.savedText.clear();
    d.savedRevision = 0;
    d.needsSave = true;
    opened.compiled.graph = emptyGraph();
    opened.macroValues.fill(.5f);
    if (const auto* macros = package["macros"].getArray())
        for (const auto& slot : *macros) {
            const auto i = slot["slot"].asInt() - 1;
            if (i < 0 || i >= 32) continue;
            opened.exports[static_cast<std::size_t>(i)] = {json::str(slot, "bindingId"), json::str(slot, "name"), false,
                                                           json::str(slot, "structure")};
            const auto value = static_cast<float>(slot["value"].asDouble());
            opened.macroValues[static_cast<std::size_t>(i)] = std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : .5f;
        }
    const auto assetPaths = sampleAssets_.restore(package["assets"]);
    d.text = remapSamplePaths(d.text, assetPaths);
    opened.playableSource = remapSamplePaths(opened.playableSource, assetPaths);
    remapSequenceSound(opened.sequenceSound, assetPaths);
    if (d.text.size() > maximumSourceBytes || opened.playableSource.size() > maximumSourceBytes)
        fail("Remapped instrument source exceeds 1 MiB");
    samples_ = sampleAssets_.catalogue();
    if (!asNewDocument) {
        document_ = {};
        compiled_ = {};
        inactiveDocuments_.clear();
        documentOrder_.clear();
    }
    const auto id = d.path;
    documentOrder_.push_back(id);
    inactiveDocuments_[id] = std::move(opened);
    const auto& prefs = package["preferences"];
    if (prefs.isObject()) preferences_ = preferencesWithPatch(prefs);
    sequenceSound_ = {};
    runtime_.sequencePinned.store(false);
    runtime_.demo.store(false);
    runtime_.sequencePanic.store(true);
    runtime_.sequenceSession.requestPanic();
    activateLocked(id);
}

namespace {
void putInt(std::string& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) out += static_cast<char>((value >> (8 * i)) & 0xff);
}
constexpr std::uint32_t stateMagic = 0x464c5731;
}  // namespace
std::string AppCore::saveState() {
    std::shared_ptr<const Checkpoint> snapshot;
    {
        std::lock_guard lock(stateMutex_);
        snapshot = checkpoint_;
    }
    if (!snapshot) return {};
    // Source and dependencies are prebuilt off audio. Only a bounded 32-float
    // runtime overlay is sampled at the instant the DAW asks to save.
    std::string out;
    const auto size = snapshot->prefix.size() + snapshot->assets->size() + 1;
    out.reserve(136 + size);
    putInt(out, stateMagic);
    putInt(out, static_cast<std::uint32_t>(size));
    for (int i = 0; i < 32; ++i) putInt(out, std::bit_cast<std::uint32_t>(parameters_.get(i)));
    out += snapshot->prefix;
    out += *snapshot->assets;
    out += '}';
    return out;
}
void AppCore::restoreState(std::string bytes) {
    if (bytes.empty() || bytes.size() > maximumInstrumentPackageBytes + 136) return;
    std::lock_guard lock(stateMutex_);
    restore_ = std::make_shared<const std::string>(std::move(bytes));
}

void AppCore::updateMacrosLocked() {
    MacroMap map;
    map.revision = compiled_.revision;
    const auto structure = compiled_.program ? bindingStructure(*compiled_.program) : std::string();
    for (std::size_t i = 0; i < exports_.size(); ++i) {
        auto& e = exports_[i];
        e.bound = false;
        if (e.structure.empty()) e.structure = structure;
        if (e.structure != structure) continue;
        if (compiled_.program)
            for (const auto& p : compiled_.program->parameters)
                if (p.binding_id == e.binding && p.live) {
                    e.bound = true;
                    map.bindings[i] = {p.value, p.min, p.max, p.logarithmic};
                    break;
                }
    }
    runtime_.macroMaps.push(map);
}
void AppCore::refreshAI(bool force) {
    const auto now = text::steadyMillis();
    if (!force && now < aiRefreshAt_) return;
    aiRefreshAt_ = now + 500;
    json::Value settings;
    try {
        const auto connection = aiModels_.connection(&settings);
        aiEndpoint_ = connection.endpoint;
        aiModel_ = connection.model;
        apiKey_ = connection.key;
        aiSettingsError_.clear();
    } catch (const std::exception& error) {
        aiEndpoint_.clear();
        aiModel_.clear();
        apiKey_.clear();
        aiSettingsError_ = error.what();
    }
    if (settings.isObject()) {
        aiProfiles_ = settings["profiles"];
        aiSelectedId_ = json::str(settings, "selectedId");
    }
}
json::Value AppCore::snapshot() {
    std::lock_guard lock(mutex_);
    refreshAI();
    const auto compileView = [](const CompileView& view) {
        return json::object(
            {{"status", view.status},
             {"revision", static_cast<std::int64_t>(view.revision)},
             {"diagnostics", view.diagnostics},
             {"signals", view.signals},
             {"parameters", view.parameters},
             {"choices", view.choices},
             {"graph", view.graph},
             {"voiceLimit", view.program && view.program->voice_policy.mode == VoiceMode::mono ? 1 : 32},
             {"outputChannels", view.program ? static_cast<int>(view.program->nodes[view.program->output].channels) : 1}});
    };
    const auto documentView = [&](const Document& d, const CompileView& view) {
        return json::object({{"id", d.path},
                             {"path", d.path},
                             {"text", d.text},
                             {"revision", static_cast<std::int64_t>(d.revision)},
                             {"savedRevision", static_cast<std::int64_t>(d.savedRevision)},
                             {"dirty", d.dirty()},
                             {"compile", compileView(view)}});
    };
    json::Array documents;
    for (const auto& id : documentOrder_) {
        if (id == document_.path)
            documents.push_back(documentView(document_, compiled_));
        else if (const auto it = inactiveDocuments_.find(id); it != inactiveDocuments_.end())
            documents.push_back(documentView(it->second.document, it->second.compiled));
    }
    json::Array slots;
    for (int i = 0; i < 32; ++i) {
        const auto& exported = exports_[static_cast<std::size_t>(i)];
        slots.push_back(json::object({{"slot", i + 1},
                                      {"bindingId", exported.binding},
                                      {"name", exported.name},
                                      {"bound", exported.bound},
                                      {"value", parameters_.get(i)}}));
    }
    auto& session = runtime_.session;
    const auto audioRevision = session.status.revision.load();
    const bool selectedAudio = runtime_.sourcePlayable.load() && !document_.path.empty() &&
                               audioRevision >= runtime_.audioDocumentRevision.load();
    auto sequence = sequenceStateJSON(displayedSequenceLocked(), sequenceSound_, false);
    sequence.set("ownerDocumentId", sequenceSound_.pinned() ? sequenceOwner_ : document_.path);
    const auto unitStatus = sequenceSound_.pinned() && sequenceSoundStatus_ == "ready" &&
                                    runtime_.sequenceSession.status.revision.load() < runtime_.sequenceTargetRevision.load()
                                ? std::string("pending")
                                : sequenceSoundStatus_;
    sequence.set("soundStatus", unitStatus);
    sequence.set("soundError", sequenceSoundError_);
    sequence.set("playing", runtime_.demo.load());
    sequence.set("revision", static_cast<std::int64_t>(sequenceRevision_));
    const bool hostTempo = runtime_.hostTempo.load();
    const int voices = sequenceSound_.pinned() ? static_cast<int>(runtime_.mixedVoices.load())
                                               : (selectedAudio ? static_cast<int>(session.status.voices.load()) : 0);
    const bool aiConfigured = !aiModel_.empty() && isAllowedAIEndpoint(aiEndpoint_) &&
                              (!apiKey_.empty() || isLoopbackEndpoint(aiEndpoint_));
    return json::object(
        {{"protocol", protocolVersion},
         {"sequencer", std::move(sequence)},
         {"sessionId", text::hex(reinterpret_cast<std::uintptr_t>(this))},
         {"workspaceId", text::sha256Hex(options_.workspaceIdentity.empty() ? workspace_.string() : options_.workspaceIdentity)},
         {"document", documentView(document_, compiled_)},
         {"documents", std::move(documents)},
         {"files", files_},
         {"samples", samples_},
         {"compile", compileView(compiled_)},
         {"observation", json::object({{"voice", selectedVoice_},
                                       {"signals", json::array(selectedSignals_)},
                                       {"projection", projectionName(projection_)}})},
         {"audio", json::object({{"bpm", hostTempo ? runtime_.reportedBpm.load() : runtime_.bpm.load()},
                                 {"tempoSource", hostTempo ? "host" : "internal"},
                                 {"running", runtime_.running.load()},
                                 {"sampleRate", runtime_.rate.load()},
                                 {"blockSize", runtime_.blockSize.load()},
                                 {"voices", voices},
                                 {"cpu", session.status.cpu.load()},
                                 {"peak", runtime_.mixedPeak.load()},
                                 {"programRevision", static_cast<std::int64_t>(selectedAudio ? audioRevision : 0)},
                                 {"demoRunning", runtime_.demo.load()},
                                 {"audition", auditionSignal_}})},
         {"ai", json::object({{"profiles", aiProfiles_},
                              {"selectedId", aiSelectedId_},
                              {"settingsError", aiSettingsError_},
                              {"configured", aiConfigured},
                              {"model", aiModel_},
                              {"endpoint", aiEndpoint_},
                              {"busy", aiBusy_.load()},
                              {"runningConversationId", runningConversationId_},
                              {"runningRequestId", runningRequestId_},
                              {"activeConversationId", assistantHistory_.activeId()},
                              {"historyError", assistantHistory_.error()}})},
         {"preferences", preferences_},
         {"exportedParameters", std::move(slots)},
         {"capabilities", capabilities_}});
}
json::Value AppCore::conversationViewLocked() {
    auto result = assistantHistory_.view();
    auto* conversation = result.getObject() ? result.getObject()->find("conversation") : nullptr;
    if (conversation && conversation->isObject()) {
        const auto conversationId = json::str(*conversation, "id");
        const bool hidden = (*conversation)["deleted"].asBool() || (*conversation)["archived"].asBool();
        if (auto* messages = conversation->getObject()->find("messages"); messages && messages->isArray()) {
            for (auto& message : *messages->getArray()) {
                const auto it = proposals_.find(json::str(message, "id"));
                const bool available = it != proposals_.end() && proposalCurrentLocked(it->second) &&
                                       it->second.conversationId == conversationId &&
                                       it->second.text == json::str(message, "proposedSource") && !hidden;
                message.set("proposalAvailable", available);
            }
        }
    }
    result.set("runningConversationId", runningConversationId_);
    result.set("runningRequestId", runningRequestId_);
    return result;
}
json::Value AppCore::importSample(const fs::path& file) {
    const auto imported = sampleAssets_.importAudio(file);
    auto samples = sampleAssets_.catalogue();
    {
        std::lock_guard lock(mutex_);
        samples_ = std::move(samples);
        preparedRevision_ = 0;
        requestedRevision_ = document_.revision;
        updateCheckpointLocked();
    }
    auto result = snapshot();
    result.set("sampleImportedPath", imported);
    return result;
}
json::Value AppCore::telemetry() {
    std::lock_guard lock(mutex_);
    return latestTelemetry_;
}
std::vector<json::Value> AppCore::takeAgentEvents() {
    std::lock_guard lock(mutex_);
    std::vector<json::Value> result;
    result.swap(agentEvents_);
    return result;
}
void AppCore::savePreferencesLocked() {
    try {
        files::writeAtomic(settingsFile_, json::stringify(json::object({{"preferences", preferences_}})));
    } catch (...) { /* Preferences stay in memory; recovery keeps them too. */
    }
}

json::Value AppCore::request(const std::string& method, const json::Value& params) {
    if (method == "session.get") return snapshot();
    if (method == "ai.send") {
        sendAI(params);
        std::lock_guard lock(mutex_);
        return json::object({{"status", "working"},
                             {"requestId", json::str(params, "requestId")},
                             {"conversationId", !json::str(params, "conversationId").empty()
                                                    ? json::str(params, "conversationId")
                                                    : assistantHistory_.activeId()}});
    }
    std::unique_lock lock(mutex_);
    const auto requireRevision = [&](const char* field = "baseRevision") {
        if (document_.path.empty()) fail("No instrument is open");
        if (params[field].asInt() != static_cast<std::int64_t>(document_.revision))
            fail("The document changed. Refresh before applying this edit.");
        const auto id = json::str(params, "documentId");
        if (!id.empty() && id != document_.path) fail("This edit belongs to another document.");
    };
    const auto requestedDocument = json::str(params, "documentId");
    if (method == "keyboard.noteOff" && !requestedDocument.empty() && requestedDocument != document_.path) {
        // Activation has already released the old instrument. Its eventual
        // pointer/key release is harmless and must not report an edit conflict.
        lock.unlock();
        return snapshot();
    }
    if (method == "document.undo" || method == "document.redo" || method.starts_with("keyboard.") ||
        method.starts_with("probe.") || method.starts_with("parameter.") || method == "audio.playDemo") {
        if (!requestedDocument.empty() && requestedDocument != document_.path)
            fail("This action belongs to another instrument");
        if (document_.path.empty() && method != "keyboard.panic") fail("No instrument is open");
    }
    if (method.starts_with("sequencer.") && !requestedDocument.empty() && requestedDocument != document_.path &&
        requestedDocument != sequenceOwner_)
        fail("This sequencer action belongs to another instrument");
    if (method.starts_with("ai.conversations.")) {
        const auto action = method.substr(std::string("ai.conversations.").size());
        const auto id = json::str(params, "conversationId");
        if (action == "list") {
        } else if (action == "create")
            assistantHistory_.create(json::str(params, "title"));
        else if (action == "select")
            assistantHistory_.select(id);
        else if (action == "rename")
            assistantHistory_.rename(id, json::str(params, "title"));
        else if (action == "archive")
            assistantHistory_.archive(id);
        else if (action == "delete")
            assistantHistory_.remove(id);
        else if (action == "restore")
            assistantHistory_.restore(id);
        else if (action == "purge")
            assistantHistory_.purge(id);
        else
            fail("Unknown conversation action: " + action);
        if (action == "delete" || action == "purge" || action == "archive")
            std::erase_if(proposals_, [&](const auto& item) { return item.second.conversationId == id; });
        return conversationViewLocked();
    }
    if (method == "workspace.import") {
        const auto folder = workspaceFolder(workspace_, json::str(params, "folder"));
        const auto name = json::str(params, "name");
        if (name.empty() || name.front() == '.' || text::containsAnyOf(name, "/\\:")) fail("Use a file name without a path");
        const auto& encoded = params["data"].string();
        if (encoded.size() > 90 * 1024 * 1024) fail("Import files of at most 64 MiB");
        const auto bytes = text::base64Decode(encoded);
        if (!bytes || bytes->size() > 64 * 1024 * 1024) fail("Invalid imported file data");
        if (!files::createDirectories(folder)) fail("Could not create destination folder");
        auto target = files::nonexistentSibling(folder / name);
        if (files::hasExtension(name, "flows")) {
            if (bytes->size() > maximumSourceBytes) fail("Instrument source exceeds 1 MiB");
            if (!text::isValidUtf8(*bytes)) fail("Instrument source must be UTF-8");
            if (!files::writeNew(target, *bytes)) fail("Could not publish imported instrument without overwriting a file");
        } else {
            if (!files::hasExtension(name, "wav;aif;aiff;aifc;flac;mp3;ogg")) fail("Drop a .flows instrument or an audio file");
            // Preserve the original name and extension for format detection.
            const auto staging = fs::temp_directory_path() / ("flows-import-" + text::randomId());
            struct Cleanup {
                fs::path path;
                ~Cleanup() { files::removeAll(path); }
            } cleanup{staging};
            if (!files::createDirectories(staging)) fail("Could not stage imported audio");
            const auto staged = staging / name;
            try {
                files::writeAtomic(staged, *bytes);
            } catch (const std::exception&) {
                fail("Could not read imported audio");
            }
            target = workspace_ / sampleAssets_.importAudio(staged, json::str(params, "folder"));
        }
        refreshFilesLocked();
        samples_ = sampleAssets_.catalogue();
        const auto imported = files::relative(workspace_, target);
        lock.unlock();
        auto result = snapshot();
        result.set("importedPath", imported);
        return result;
    }
    if (method == "document.new") {
        if (documentOrder_.size() >= maximumOpenDocuments)
            fail("Close an instrument before opening another (32 open instruments maximum)");
        auto name = text::trim(json::str(params, "name"));
        if (name.empty()) name = "Instrument.flows";
        if (text::containsAnyOf(name, "/\\") || name.front() == '.') fail("Use a file name inside this workspace");
        const auto folder = workspaceFolder(
            workspace_, json::str(params, "folder").empty() ? std::string("examples") : json::str(params, "folder"));
        if (!files::createDirectories(folder)) fail("Could not create instrument folder");
        auto file = files::nonexistentSibling(folder / files::withExtension(name, "flows"));
        const auto base = files::stem(file.filename().string());
        for (int suffix = 2; std::find(documentOrder_.begin(), documentOrder_.end(), files::relative(workspace_, file)) !=
                             documentOrder_.end();
             ++suffix)
            file = files::nonexistentSibling(folder / (base + " " + std::to_string(suffix) + ".flows"));
        const auto samplePath = json::str(params, "samplePath");
        std::string content;
        if (!samplePath.empty()) {
            if (!sampleAssets_.isManaged(samplePath)) fail("Choose an imported sample from the Samples library");
            content = samplerSource(samplePath, true, {});
        } else {
            const auto* bundledTemplate = bundled("sine-reference.flows");
            if (!bundledTemplate) fail("Could not create instrument");
            content = std::string(bundledTemplate->bytes);
        }
        try {
            files::writeAtomic(file, content);
        } catch (const std::exception&) {
            fail(samplePath.empty() ? "Could not create instrument" : "Could not create sampler instrument");
        }
        refreshFilesLocked();
        lock.unlock();
        return request("document.open", json::object({{"path", files::relative(workspace_, file)}}));
    } else if (method == "document.rename") {
        if (document_.path.empty()) fail("No instrument is open");
        auto name = text::trim(json::str(params, "name"));
        if (name.empty() || text::containsAnyOf(name, "/\\") || name.front() == '.')
            fail("Use a file name inside this workspace");
        const auto id = json::str(params, "documentId");
        if (!id.empty() && id != document_.path) fail("The current document changed");
        const auto original = workspace_ / document_.path;
        const auto target = original.parent_path() / files::withExtension(name, "flows");
        const auto nextPath = files::relative(workspace_, target);
        if (target != original && (files::exists(target) || inactiveDocuments_.contains(nextPath)))
            fail("An instrument with this name already exists");
        const bool prepareSequence = target != original && !document_.needsSave;
        auto renamedSound = documentSequenceSound_;
        renamedSound.rename(document_.path, nextPath);
        struct MetadataBackup {
            fs::path file;
            bool existed = false;
            std::string bytes;
        };
        std::vector<MetadataBackup> metadata;
        const auto stage = [&](const std::string& path, const SequencePattern& pattern, const SequenceSound& sound) {
            MetadataBackup backup;
            backup.file = sequenceFile(path);
            backup.existed = files::exists(backup.file);
            if (backup.existed) {
                const auto saved = files::isFile(backup.file) ? files::tryRead(backup.file, maximumSequenceBytes) : std::nullopt;
                if (!saved) fail("Could not preserve sequencer metadata before rename");
                backup.bytes = *saved;
            }
            metadata.push_back(std::move(backup));
            saveSequenceLocked(path, pattern, sound);
        };
        try {
            if (prepareSequence) stage(nextPath, sequencePattern_, renamedSound);
            for (const auto& [path, opened] : inactiveDocuments_)
                if (!opened.document.needsSave && opened.sequenceSound.references(document_.path)) {
                    auto sound = opened.sequenceSound;
                    sound.rename(document_.path, nextPath);
                    stage(path, opened.sequencer, sound);
                }
            if (sequenceSound_.references(document_.path) && sequenceOwner_ != document_.path &&
                !inactiveDocuments_.contains(sequenceOwner_) && files::isFile(workspace_ / sequenceOwner_)) {
                auto sound = sequenceSound_;
                sound.rename(document_.path, nextPath);
                stage(sequenceOwner_, pinnedSequencePattern_, sound);
            }
            if (target != original && files::isFile(original) && !files::move(original, target))
                fail("Could not rename instrument");
        } catch (...) {
            for (auto item = metadata.rbegin(); item != metadata.rend(); ++item) {
                if (item->existed) try {
                        files::writeAtomic(item->file, item->bytes);
                    } catch (...) {
                    }
                else
                    files::remove(item->file);
            }
            throw;
        }
        for (auto& path : documentOrder_)
            if (path == document_.path) path = nextPath;
        const auto previousPath = document_.path;
        document_.path = nextPath;
        documentSequenceSound_ = std::move(renamedSound);
        for (auto& [path, opened] : inactiveDocuments_) opened.sequenceSound.rename(previousPath, nextPath);
        if (sequenceOwner_ == previousPath) sequenceOwner_ = nextPath;
        sequenceSound_.rename(previousPath, nextPath);
        documentSequenceSound_.rename(previousPath, nextPath);
        retainSequenceStateLocked();
        if (previousPath != nextPath) files::remove(sequenceFile(previousPath));
        refreshFilesLocked();
        updateCheckpointLocked();
    } else if (method == "document.open") {
        const auto path = json::str(params, "path");
        const auto file = files::child(workspace_, path);
        if (!file || !files::hasExtension(path, "flows")) fail("Instrument is outside this workspace or cannot be opened");
        const auto id = files::relative(workspace_, *file);
        if (id != document_.path && !inactiveDocuments_.contains(id)) {
            if (!files::isFile(*file) || files::size(*file) > static_cast<std::int64_t>(maximumSourceBytes))
                fail("Instrument cannot be opened");
            if (documentOrder_.size() >= maximumOpenDocuments)
                fail("Close an instrument before opening another (32 open instruments maximum)");
            OpenDocument opened;
            opened.document.path = id;
            opened.sequencer = loadSequenceLocked(id);
            opened.sequenceSound = loadSequenceSoundLocked(id);
            opened.document.text = readSourceFile(*file);
            opened.document.savedText = opened.document.text;
            opened.compiled.graph = emptyGraph();
            opened.macroValues.fill(.5f);
            inactiveDocuments_[id] = std::move(opened);
            documentOrder_.push_back(id);
        }
        activateLocked(id);
    } else if (method == "document.activate") {
        const auto id = json::str(params, "documentId");
        if (id.empty()) fail("Select an open instrument");
        activateLocked(id);
    } else if (method == "document.close") {
        const auto id = !requestedDocument.empty() ? requestedDocument : document_.path;
        const auto position = std::find(documentOrder_.begin(), documentOrder_.end(), id);
        if (position == documentOrder_.end()) fail("This instrument is not open");
        const bool active = id == document_.path;
        auto& target = active ? document_ : inactiveDocuments_.at(id).document;
        if (target.dirty()) {
            if (params["save"].asBool())
                saveLocked(target);
            else if (!params["discard"].asBool())
                fail("This instrument has unsaved changes. Save or discard before closing.");
        }
        const auto index = static_cast<std::size_t>(std::distance(documentOrder_.begin(), position));
        documentOrder_.erase(position);
        if (active) {
            // Do not put the just-closed tab back into the inactive cache.
            document_ = {};
            compiled_ = {};
            const auto next = documentOrder_.empty() ? std::string() : documentOrder_[std::min(index, documentOrder_.size() - 1)];
            activateLocked(next);
        } else {
            inactiveDocuments_.erase(id);
            updateCheckpointLocked();
        }
    } else if (method == "document.apply") {
        requireRevision();
        applyLocked(json::str(params, "text"), json::str(params, "gestureId"), json::str(params, "gesturePhase"));
    } else if (method == "document.save") {
        if (requestedDocument.empty() || requestedDocument == document_.path)
            saveLocked(document_);
        else {
            const auto it = inactiveDocuments_.find(requestedDocument);
            if (it == inactiveDocuments_.end()) fail("This instrument is not open");
            saveLocked(it->second.document);
        }
        updateCheckpointLocked();
    } else if (method == "document.undo" || method == "document.redo") {
        auto& from = method == "document.undo" ? document_.undo : document_.redo;
        auto& to = method == "document.undo" ? document_.redo : document_.undo;
        if (!from.empty()) {
            to.push_back({document_.text});
            document_.text = from.back().text;
            from.pop_back();
            document_.gesture.clear();
            document_.revision = ++nextRevision_;
            requestedRevision_ = document_.revision;
            compiled_.status = "pending";
            updateCheckpointLocked();
        }
    } else if (method == "compile") {
        requireRevision("revision");
        preparedRevision_ = 0;
        requestedRevision_ = document_.revision;
        compiled_.status = "pending";
    } else if (method == "keyboard.noteOn" || method == "keyboard.noteOff") {
        const auto pitch = params["pitch"].asDouble();
        const auto velocity = method == "keyboard.noteOn" ? params["velocity"].asDouble() : 0;
        const auto id = params["noteId"].asInt();
        if (id < 0 || !std::isfinite(pitch) || pitch < 0 || pitch > 127 || !std::isfinite(velocity) || velocity < 0 ||
            velocity > 1)
            fail("Invalid keyboard event");
        // A key pressed while this source is pending/invalid is not queued for
        // a later program. Releases remain safe for already-held identities.
        if (method == "keyboard.noteOn" && (compiled_.status != "ready" || compiled_.revision != document_.revision)) {
            lock.unlock();
            return snapshot();
        }
        if (!runtime_.keyboard.push({{0, method == "keyboard.noteOn" ? EventType::note_on : EventType::note_off,
                                      uiNoteMask | static_cast<std::uint64_t>(id), pitch, velocity},
                                     runtime_.audioDocumentRevision.load()})) {
            runtime_.panic.store(true);
            fail("Keyboard event queue overflow; voices were reset");
        }
    } else if (method == "keyboard.panic") {
        runtime_.panic.store(true);
        runtime_.sequencePanic.store(true);
        runtime_.sequenceSession.requestPanic();
        runtime_.demo.store(false);
        auditionSignal_.clear();
        runtime_.sequenceAuditionMuted.store(false);
        ++selectionVersion_;
    } else if (method == "audio.playDemo") {
        const bool play = !runtime_.demo.load();
        runtime_.demo.store(play);
        if (!play && sequenceSound_.pinned()) {
            runtime_.sequencePanic.store(true);
            runtime_.sequenceSession.requestPanic();
        }
    } else if (method == "sequencer.set" || method == "sequencer.sound") {
        if (params["baseRevision"].asInt() != static_cast<std::int64_t>(sequenceRevision_))
            fail("The sequencer changed. Refresh before applying this edit.");
        auto pattern = method == "sequencer.set" ? parseSequence(params["pattern"]) : displayedSequenceLocked();
        const auto& descriptor = method == "sequencer.set" ? params["pattern"]["sound"] : params["sound"];
        const bool sameDescriptor =
            descriptor.isObject() &&
            json::stringify(descriptor) == json::stringify(sequenceStateJSON(pattern, sequenceSound_, false)["sound"]);
        SequenceSound sound;
        if (descriptor.isNull() || (sameDescriptor && method == "sequencer.set"))
            sound = sequenceSound_;
        else if (json::str(descriptor, "kind") == "lanes" && sequenceSound_.kind == "lanes") {
            const auto* lanes = descriptor["lanes"].getArray();
            if (!lanes || lanes->size() != pattern.rowCount) fail("Every sequencer lane needs one sound");
            sound.kind = "lanes";
            for (const auto& lane : *lanes) {
                const auto playback = json::str(lane, "playback").empty() ? std::string("one_shot") : json::str(lane, "playback");
                const auto same = std::find_if(sequenceSound_.lanes.begin(), sequenceSound_.lanes.end(), [&](const auto& old) {
                    return old.kind == json::str(lane, "kind") && old.path == json::str(lane, "path") &&
                           (old.kind != "sample" || old.playback == playback);
                });
                if (same != sequenceSound_.lanes.end())
                    sound.lanes.push_back(*same);
                else {
                    if (json::str(lane, "kind") == "lanes") fail("Lane sounds cannot be nested");
                    sound.lanes.push_back(parseSequenceSound(lane, true));
                }
            }
        } else
            sound = parseSequenceSound(descriptor, true);
        if (sound.kind == "lanes" && sound.lanes.size() != pattern.rowCount) fail("Every sequencer lane needs one sound");
        auto owner = sequenceSound_.pinned() ? sequenceOwner_ : document_.path;
        if (!sound.pinned()) owner = document_.path;
        if (owner.empty()) fail("Open an instrument before editing a sequencer pattern");
        bool needsSave = document_.path == owner ? document_.needsSave : false;
        if (const auto found = inactiveDocuments_.find(owner); found != inactiveDocuments_.end())
            needsSave = found->second.document.needsSave;
        // Commit to the owning document only after complete validation/persistence.
        if (!needsSave) saveSequenceLocked(owner, pattern, sound);
        selectSequenceSoundLocked(std::move(sound), pattern, owner);
        ++sequenceRevision_;
        updateCheckpointLocked();
    } else if (method == "sequencer.play") {
        const auto& value = params["playing"];
        if (!value.isBool()) fail("Sequencer playing must be true or false");
        const bool play = value.asBool();
        runtime_.demo.store(play);
        if (!play && sequenceSound_.pinned()) {
            runtime_.sequencePanic.store(true);
            runtime_.sequenceSession.requestPanic();
        }
    } else if (method == "probe.listen") {
        const auto name = json::str(params, "signal");
        if (params["enabled"].asBool()) {
            if (!compiled_.program || compiled_.status != "ready" || auditionValue(*compiled_.program, name) == invalid_value)
                fail("Select a compiled audio signal or direct audio probe to audition");
            auditionSignal_ = name;
        } else
            auditionSignal_.clear();
        runtime_.sequenceAuditionMuted.store(!auditionSignal_.empty());
        ++selectionVersion_;
    } else if (method == "probe.set") {
        std::vector<std::string> selected;
        const auto* list = params["signals"].getArray();
        if (!list || list->size() > 128) fail("Observe at most 128 signal views");
        for (const auto& value : *list) {
            if (!value.isString()) fail("Signal names must be strings");
            const auto& name = value.string();
            if (std::find(selected.begin(), selected.end(), name) == selected.end()) selected.push_back(name);
        }
        const auto& voice = params["voice"];
        if ((voice.isString() && voice.string() != "mixed") || (!voice.isString() && !voice.isInt()))
            fail("Voice slot must be 0..31 or mixed");
        const auto requestedSlot = voice.isString() ? std::int64_t{-1} : voice.asInt();
        if (requestedSlot < -1 || requestedSlot > 31) fail("Voice slot must be 0..31 or mixed");
        const int slot = static_cast<int>(requestedSlot);
        if (!compiled_.program || compiled_.status != "ready") fail("Compile the active instrument before selecting probes");
        if (compiled_.program->voice_policy.mode == VoiceMode::mono && slot > 0) fail("Mono instruments have only voice slot 0");
        const auto projection = params.has("projection") ? parseProjection(params["projection"]) : projection_;
        const auto selection = selectionForNames(*compiled_.program, compiled_.revision, selected, slot, projection);
        if (!selection) fail(selection.error);
        if (selected != selectedSignals_ || slot != selectedVoice_ || projection != projection_) {
            selectedSignals_ = std::move(selected);
            selectedVoice_ = slot;
            projection_ = projection;
            auditionSignal_.clear();
            runtime_.sequenceAuditionMuted.store(false);
            ++selectionVersion_;
        }
    } else if (method == "parameter.export") {
        const auto slot = params["slot"].asInt() - 1;
        if (slot < 0 || slot >= 32) fail("Macro slot must be 1..32");
        const auto id = json::str(params, "bindingId");
        const ParameterDescriptor* match = nullptr;
        if (compiled_.program)
            for (const auto& p : compiled_.program->parameters)
                if (p.binding_id == id && p.live) match = &p;
        if (!match) fail("This source binding cannot be exported as a live parameter");
        exports_[static_cast<std::size_t>(slot)] = {id, match->name, true, bindingStructure(*compiled_.program)};
        const auto value = compiled_.program->nodes[match->value].initial_value;
        const double normalized = match->logarithmic && match->min > 0
                                      ? std::log(std::max(match->min, value) / match->min) / std::log(match->max / match->min)
                                      : (value - match->min) / (match->max - match->min);
        parameters_.set(static_cast<int>(slot), static_cast<float>(std::clamp(normalized, 0.0, 1.0)));
        updateMacrosLocked();
        updateCheckpointLocked();
    } else if (method == "parameter.unexport") {
        const auto slot = params["slot"].asInt() - 1;
        if (slot < 0 || slot >= 32) fail("Macro slot must be 1..32");
        exports_[static_cast<std::size_t>(slot)] = {};
        updateMacrosLocked();
        updateCheckpointLocked();
    } else if (method == "transport.set") {
        const auto& value = params["bpm"];
        const double bpm = value.asDouble();
        if (!value.isNumber() || !std::isfinite(bpm) || bpm < 20 || bpm > 300) fail("Tempo must be between 20 and 300 BPM");
        preferences_ = preferencesWithPatch(preferences_, json::object({{"bpm", bpm}}));
        runtime_.bpm.store(bpm);
        savePreferencesLocked();
        updateCheckpointLocked();
    } else if (method == "preferences.set") {
        preferences_ = preferencesWithPatch(preferences_, params);
        runtime_.bpm.store(preferences_["bpm"].asDouble());
        savePreferencesLocked();
        updateCheckpointLocked();
    } else if (method == "ai.configure" || method == "ai.models.save" || method == "ai.models.select" ||
               method == "ai.models.remove") {
        if (aiBusy_.load()) fail("Wait for the current reply before changing models.");
        if (method == "ai.models.select")
            aiModels_.select(json::str(params, "id"));
        else if (method == "ai.models.remove")
            aiModels_.remove(json::str(params, "id"));
        else
            aiModels_.save(params);
        refreshAI(true);
    } else if (method == "ai.cancel") {
        const auto id = json::str(params, "requestId");
        if (!id.empty() && id != runningRequestId_)
            fail("This AI request is no longer running. Refresh the conversation.");
        if (aiBusy_.load()) {
            aiGeneration_.fetch_add(1);
            aiWorker_.request_stop();
        }
    } else if (method == "ai.apply" || method == "ai.create") {
        auto it = proposals_.find(json::str(params, "requestId"));
        if (it == proposals_.end() || !proposalCurrentLocked(it->second) || it->second.text != json::str(params, "text"))
            fail("This proposal is stale or was not returned by the current request");
        if (!it->second.workspace) requireRevision();
        if (method == "ai.apply" && (it->second.documentId.empty() || it->second.documentId != document_.path))
            fail("Open the target instrument to review and apply this edit.");
        if (!assistantHistory_.proposalAvailable(it->second.conversationId, json::str(params, "requestId"), it->second.text))
            fail("Restore this conversation and refresh the proposal before applying it.");
        if (method == "ai.create") {
            if (documentOrder_.size() >= maximumOpenDocuments) fail("Close an instrument before opening another");
            const auto file = files::nonexistentSibling(workspaceFolder(workspace_, "AI-gen") / "AI instrument.flows");
            try {
                files::writeAtomic(file, it->second.text);
            } catch (const std::exception&) {
                fail("Could not save the generated instrument");
            }
            proposals_.erase(it);
            refreshFilesLocked();
            lock.unlock();
            return request("document.open", json::object({{"path", files::relative(workspace_, file)}}));
        }
        applyLocked(it->second.text);
        proposals_.erase(it);
    } else if (method == "project.importText") {
        if (document_.dirty()) {
            const auto recovery = workspace_ / ".recovery";
            files::createDirectories(recovery);
            try {
                files::writeAtomic(recovery / (files::fileName(document_.path) + ".flowsinst"),
                                   {recoveryPrefix(packageLocked(false)), *sampleAssets_.serializedBundle(packageSourcesLocked()), "}"});
            } catch (const std::exception&) {
                fail("Could not write instrument recovery");
            }
        }
        loadPackageLocked(json::parseBounded(json::str(params, "text"), maximumInstrumentPackageBytes), true);
        refreshFilesLocked();
    } else if (method == "project.exportText")
        return json::Value(json::stringify(packageLocked()));
    else
        fail("Unknown native method: " + method);
    lock.unlock();
    return snapshot();
}
}  // namespace flows::app
