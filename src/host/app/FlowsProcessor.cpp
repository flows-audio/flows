#include "FlowsProcessor.hpp"

#include <juce_cryptography/juce_cryptography.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <set>

#include "AIEndpoint.hpp"
#include "Analyzer.hpp"
#include "BundledExamples.hpp"
#include "DataflowGraph.hpp"
#include "FlowsAssets.h"
#include "FlowsEditor.hpp"
#include "SequencerJson.hpp"
#include "WorkspaceLayout.hpp"

namespace flows::app {
namespace {
using namespace juce;
constexpr std::uint64_t uiNoteMask = 1ull << 62;
constexpr int maximumSourceBytes = 1024 * 1024;
constexpr std::size_t maximumOpenDocuments = 32;
constexpr std::size_t maximumSequenceBytes = 8 * 1024 * 1024;
constexpr std::size_t maximumRecoveryBytes = 192 * 1024 * 1024;
var array(const std::vector<float>& values) {
    Array<var> a;
    for (auto f : values) a.add(f);
    return a;
}
String fail(const String& message) {
    throw std::runtime_error(message.toStdString());
}
void writePatternFile(const File& file, const void* bytes, std::size_t size) {
    if (file.isDirectory() || !file.getParentDirectory().createDirectory())
        fail("Could not save the sequencer pattern");
    TemporaryFile temporary(file);
    auto output = temporary.getFile().createOutputStream();
    if (!output || !output->write(bytes, size)) fail("Could not write the sequencer pattern");
    output->flush();
    if (output->getStatus().failed()) fail("Could not flush the sequencer pattern");
    output.reset();
    if (!temporary.overwriteTargetFileWithTemporary())
        fail("Could not replace the sequencer pattern");
}
void writeAssetState(const File& file, const String& prefix, const juce::MemoryBlock& assets) {
    if (!file.getParentDirectory().createDirectory()) fail("Could not create the recovery folder");
    TemporaryFile temporary(file);
    auto stream = temporary.getFile().createOutputStream();
    if (!stream || !stream->write(prefix.toRawUTF8(), prefix.getNumBytesAsUTF8()) ||
        !stream->write(assets.getData(), assets.getSize()) || !stream->writeByte('}'))
        fail("Could not write instrument recovery");
    stream->flush();
    if (stream->getStatus().failed()) fail("Could not flush instrument recovery");
    stream.reset();
    if (!temporary.overwriteTargetFileWithTemporary()) fail("Could not finish instrument recovery");
}
String readSourceFile(const File& file) {
    auto input = file.createInputStream();
    if (!input) return fail("Could not read instrument: " + file.getFileName());
    juce::MemoryBlock bytes;
    input->readIntoMemoryBlock(bytes, maximumSourceBytes + 1);
    if (bytes.getSize() > maximumSourceBytes)
        return fail("Instrument exceeds the 1 MiB document limit: " + file.getFileName());
    auto text = String::createStringFromData(bytes.getData(), static_cast<int>(bytes.getSize()));
    if (text.getNumBytesAsUTF8() > maximumSourceBytes)
        return fail("Instrument exceeds the 1 MiB document limit: " + file.getFileName());
    return text;
}
bool audioNode(const Program& p, ValueId id) {
    std::vector<bool> audio(p.nodes.size());
    for (ValueId i = 0; i <= id; ++i) {
        const auto& n = p.nodes[i];
        audio[i] = n.op == Op::sine || n.op == Op::saw || n.op == Op::unison_sine ||
                   n.op == Op::unison_saw || n.op == Op::phasor || n.op == Op::sampler;
        if (n.op != Op::lfo && n.op != Op::adsr && n.op != Op::key_trigger)
            for (auto input : n.inputs) audio[i] = audio[i] || audio[input];
    }
    return audio[id];
}
ValueId auditionValue(const Program& program, const std::string& name) {
    for (const auto& value : program.named_values)
        if (value.name == name)
            return audioNode(program, value.value) ? value.value : invalid_value;
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
                                       const std::vector<std::string>& names, int voice,
                                       ProbeProjection projection = ProbeProjection::mid) {
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
        case ProbeProjection::mid:
            return "mid";
        case ProbeProjection::left:
            return "left";
        case ProbeProjection::right:
            return "right";
        case ProbeProjection::side:
            return "side";
    }
    return "mid";
}
ProbeProjection parseProjection(const juce::var& value) {
    const auto name = value.toString();
    if (name == "mid") return ProbeProjection::mid;
    if (name == "left") return ProbeProjection::left;
    if (name == "right") return ProbeProjection::right;
    if (name == "side") return ProbeProjection::side;
    fail("Audio projection must be mid, left, right or side");
    return ProbeProjection::mid;
}
const char* analysisShape(AnalysisShape shape) {
    switch (shape) {
        case AnalysisShape::waveform:
            return "waveform";
        case AnalysisShape::scalar:
            return "scalar";
        case AnalysisShape::spectrum:
            return "spectrum";
        case AnalysisShape::mel_spectrum:
            return "mel_spectrum";
    }
    return "waveform";
}
double sourceScale(const std::string& text) {
    if (text.ends_with("khz")) return 1000;
    if (text.ends_with("ms")) return .001;
    return 1;
}
juce::String bindingStructure(const Program& program) {
    const auto key = dsp_structure_key(program);
    return juce::SHA256(key.data(), key.size()).toHexString();
}
var graphMetadata(const DataflowGraph& graph, const std::string& source) {
    Array<var> nodes, edges;
    for (const auto& node : graph.nodes) {
        Array<var> parameters;
        for (const auto& id : node.parameterIds) parameters.add(String(id));
        Array<var> choices;
        for (const auto& id : node.choiceIds) choices.add(String(id));
        nodes.add(object({{"id", node.id},
                          {"label", node.label},
                          {"op", node.op},
                          {"kind", node.kind},
                          {"signal", node.signal},
                          {"from", utf16Offset(source, node.span.offset)},
                          {"to", utf16Offset(source, node.span.offset + node.span.length)},
                          {"parameterIds", parameters},
                          {"choiceIds", choices}}));
    }
    for (const auto& edge : graph.edges)
        edges.add(object({{"id", edge.id},
                          {"source", edge.source},
                          {"target", edge.target},
                          {"kind", edge.kind},
                          {"label", edge.label}}));
    return object({{"nodes", nodes}, {"edges", edges}, {"output", graph.output}});
}
var preferencesWithPatch(const var& current, const var& patch = {}) {
    const auto value = [&](const char* name) {
        return patch.hasProperty(name) ? get(patch, name) : get(current, name);
    };
    const auto text = [&](const char* name, const char* fallback) {
        auto v = value(name);
        return v.isString() && v.toString().isNotEmpty() ? v.toString().substring(0, 256)
                                                         : String(fallback);
    };
    const auto number = [&](const char* name, double fallback, double minimum, double maximum) {
        const auto v = value(name);
        if (!(v.isInt() || v.isInt64() || v.isDouble()) || !std::isfinite(static_cast<double>(v)))
            return fallback;
        return juce::jlimit(minimum, maximum, static_cast<double>(v));
    };
    const auto visibility = [&](const char* name) {
        const auto patched = get(patch, name), saved = get(current, name);
        if (patched.isBool()) return static_cast<bool>(patched);
        return saved.isBool() ? static_cast<bool>(saved) : true;
    };
    // Old settings stay explicitly version 1 so the UI can migrate their small
    // font once. A new workspace already has the complete version 2 defaults.
    const auto layoutVersion = number("layoutVersion", current.isObject() ? 1 : 2, 1, 2);
    return object({{"theme", text("theme", "graphite")},
                   {"opacity", number("opacity", 1, .2, 1)},
                   {"split", text("split", "none")},
                   {"fontFamily", text("fontFamily", "SFMono-Regular")},
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
}  // namespace
FlowsProcessor::FlowsProcessor(bool isStandalone, AISecrets secrets)
    : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      standalone(isStandalone),
      aiModels_(std::move(secrets)) {
    for (int i = 0; i < 32; ++i) {
        auto suffix = juce::String(i + 1).paddedLeft('0', 2);
        hostParams_[i] =
            new juce::AudioParameterFloat(juce::ParameterID("macro" + suffix, 1), "Macro " + suffix,
                                          juce::NormalisableRange<float>(0, 1), 0.5f);
        previousMacro_[i] = .5f;
        addParameter(hostParams_[i]);
    }
    configureWorkspace();
    updateCheckpointLocked();
    worker_ = std::jthread([this](std::stop_token stop) { worker(stop); });
    companionId_ = juce::Uuid().toString();
    if (juce::SystemStats::getEnvironmentVariable("FLOWS_DISABLE_LIVE_BRIDGE", {}) != "1")
        companionWorker_ = std::jthread([this](std::stop_token stop) { companionLoop(stop); });
}
FlowsProcessor::~FlowsProcessor() {
    companionWorker_.request_stop();
    if (companionWorker_.joinable()) companionWorker_.join();
    aiGeneration_.fetch_add(1);
    aiWorker_.request_stop();
    worker_.request_stop();
    if (aiWorker_.joinable()) aiWorker_.join();
    if (worker_.joinable()) worker_.join();
    if (standalone) try {
            const auto prefix =
                juce::JSON::toString(recoveryLocked(false), true).dropLastCharacters(1) +
                ",\"assets\":";
            writeAssetState(workspace_.getChildFile(".session.flowsinst"), prefix,
                            *sampleAssets_.serializedBundle(recoverySourcesLocked()));
        } catch (...) { /* Keep the last complete recovery file if an asset cannot be read. */
        }
}
void FlowsProcessor::configureWorkspace() {
    auto overridePath = juce::SystemStats::getEnvironmentVariable("FLOWS_WORKSPACE", {});
    workspace_ = overridePath.isNotEmpty()
                     ? juce::File(overridePath)
                     : juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                           .getChildFile("Flows/Workspace");
    workspace_.createDirectory();
    sampleAssets_.setRoot(workspace_);
    for (int i = 0; i < FlowsAssets::namedResourceListSize; ++i) {
        auto name = juce::File(juce::String(FlowsAssets::originalFilenames[i])).getFileName();
        if (name.endsWith(".wav")) {
            int size = 0;
            const auto* bytes =
                FlowsAssets::getNamedResource(FlowsAssets::namedResourceList[i], size);
            try {
                sampleAssets_.installBundled("Samples/" + name, bytes,
                                             static_cast<std::size_t>(size));
            } catch (...) {
            }
            continue;
        }
        if (!name.endsWith(".flows")) continue;
        auto file = workspace_.getChildFile("examples").getChildFile(name);
        file.getParentDirectory().createDirectory();
        if (workspace_.getChildFile(name).existsAsFile()) continue;
        if (!file.exists()) {
            int size = 0;
            auto* bytes = FlowsAssets::getNamedResource(FlowsAssets::namedResourceList[i], size);
            file.replaceWithData(bytes, static_cast<std::size_t>(size));
        }
    }
    organizeWorkspace(workspace_);
    assistantHistory_.setWorkspace(workspace_);
    settingsFile_ = workspace_.getChildFile(".flows-settings.json");
    juce::var settings;
    try {
        if (settingsFile_.existsAsFile() && settingsFile_.getSize() < 65536)
            settings = parseJSONBounded(settingsFile_.loadFileAsString(), 65536);
    } catch (...) {
    }
    preferences_ = preferencesWithPatch(get(settings, "preferences"));
    compiled_.graph = graphMetadata({}, {});
    try {
        aiModels_.migrate(str(settings, "endpoint"), str(settings, "model"));
    } catch (const std::exception& error) {
        aiSettingsError_ = error.what();
    }
    refreshAI(true);
    document_.path = "examples/trance-rolling-bass.flows";
    sequencePattern_ = loadSequenceLocked(document_.path);
    documentSequenceSound_ = loadSequenceSoundLocked(document_.path);
    selectSequenceSoundLocked(documentSequenceSound_, sequencePattern_, document_.path);
    document_.text = workspace_.getChildFile(document_.path).loadFileAsString();
    document_.savedText = document_.text;
    documentOrder_.push_back(document_.path);
    if (standalone) try {
            const auto recoveryFile = workspace_.getChildFile(".session.flowsinst");
            if (recoveryFile.existsAsFile() &&
                recoveryFile.getSize() <= static_cast<juce::int64>(maximumRecoveryBytes)) {
                const auto recovery =
                    parseJSONBounded(recoveryFile.loadFileAsString(), maximumRecoveryBytes);
                if (str(recovery, "format") == "flows.workspace")
                    loadRecoveryLocked(recovery);
                else
                    loadPackageLocked(recovery);
            }
        } catch (...) { /* Keep the bundled instrument if recovery is corrupt. */
        }
    bpm_.store(static_cast<double>(get(preferences_, "bpm")));
    samples_ = sampleAssets_.catalogue();
    files_ = workspaceFiles(workspace_);
}
bool FlowsProcessor::isBusesLayoutSupported(const BusesLayout& l) const {
    return l.getMainInputChannelSet().isDisabled() &&
           (l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo() ||
            l.getMainOutputChannelSet() == juce::AudioChannelSet::mono());
}
void FlowsProcessor::prepareToPlay(double rate, int block) {
    sequenceRouting_ = {};
    sequenceRouteEnabled_ = false;
    rate_.store(rate);
    blockSize_.store(block);
    running_.store(true);
    panic_.store(true);
}
void FlowsProcessor::releaseResources() {
    running_.store(false);
    panic_.store(true);
    sequencePanic_.store(true);
    session_.requestPanic();
    sequenceSession_.requestPanic();
}
void FlowsProcessor::beginHostAutomation() noexcept {
    hostPointCount_ = 0;
    rawHostPoints_ = true;
    hostPointOverflow_ = false;
}
void FlowsProcessor::hostParameter(int slot, int offset, float value) noexcept {
    if (slot < 0 || slot >= 32 || offset < 0 || !std::isfinite(value)) return;
    if (hostPointCount_ >= hostPoints_.size()) {
        hostPointOverflow_ = true;
        return;
    }
    hostPoints_[hostPointCount_++] = {slot, offset, std::clamp(value, 0.0f, 1.0f)};
}
void FlowsProcessor::auParameter(int slot, int offset, unsigned int duration, float start,
                                 float end, bool ramp) noexcept {
    if (slot < 0 || slot >= 32 || !std::isfinite(start) || !std::isfinite(end)) {
        auOverflow_.store(true);
        return;
    }
    if (!auPoints_.push({slot, offset, std::clamp(end, 0.0f, 1.0f), duration,
                         std::clamp(start, 0.0f, 1.0f), ramp ? 2 : 1}))
        auOverflow_.store(true);
}
void captureAUParameter(juce::AudioProcessor* p, int slot, unsigned int offset,
                        float value) noexcept {
    static_cast<FlowsProcessor*>(p)->auParameter(
        slot, static_cast<int>(std::min(offset, 2147483647u)), 0, value, value, false);
}
void captureAURamp(juce::AudioProcessor* p, int slot, int offset, unsigned int duration,
                   float start, float end) noexcept {
    static_cast<FlowsProcessor*>(p)->auParameter(slot, offset, duration, start, end, true);
}
void beginHostAutomation(juce::AudioProcessor* p) noexcept {
    static_cast<FlowsProcessor*>(p)->beginHostAutomation();
}
void captureHostParameter(juce::AudioProcessor* p, int slot, int offset, float value) noexcept {
    static_cast<FlowsProcessor*>(p)->hostParameter(slot, offset, value);
}
void FlowsProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) {
    juce::ScopedNoDenormals guard;
    const auto started = std::chrono::steady_clock::now();
    buffer.clear();
    const int total = buffer.getNumSamples();
    if (total == 0) {
        midi.clear();
        rawHostPoints_ = false;
        hostPointCount_ = 0;
        return;
    }
    const auto clock = session_.status.samples.load();
    const bool pinned = sequencePinned_.load(std::memory_order_acquire);
    const bool resetSequence = sequencePanic_.exchange(false);
    if (resetSequence) sequenceSession_.requestPanic();
    Transport transport{bpm_.load(), fallbackPpq_, true, true};
    bool hostTempo = false;
    // AudioProcessorPlayer supplies a device-clock playhead in standalone,
    // with no musical transport fields. Its default isPlaying=false must not
    // stop our local pattern or transport LFO; only plugins follow host state.
    if (auto* playHead = standalone ? nullptr : getPlayHead())
        if (auto position = playHead->getPosition()) {
            transport.playing = position->getIsPlaying();
            if (const auto tempo = position->getBpm();
                tempo && std::isfinite(*tempo) && *tempo > 0 && *tempo <= 1000) {
                transport.bpm = *tempo;
                hostTempo = true;
            }
            if (const auto beat = position->getPpqPosition(); beat && std::isfinite(*beat))
                transport.ppq = *beat;
            if (const auto sample = position->getTimeInSamples();
                sample && position->getIsPlaying()) {
                if (nextTransportSample_ && *sample != *nextTransportSample_) {
                    panic_.store(true);
                    sequencePanic_.store(true);
                    sequenceSession_.requestPanic();
                }
                nextTransportSample_ = *sample + total;
            } else
                nextTransportSample_.reset();
        }
    reportedBpm_.store(transport.bpm);
    hostTempo_.store(hostTempo);
    HostPoint auPoint;
    for (std::size_t drained = 0; drained < 256 && auPoints_.pop(auPoint); ++drained) {
        if (hostPointCount_ < hostPoints_.size())
            hostPoints_[hostPointCount_++] = auPoint;
        else
            hostPointOverflow_ = true;
    }
    if (auPoints_.hasPending()) hostPointOverflow_ = true;
    const bool automationOverflow = hostPointOverflow_ || auOverflow_.exchange(false);
    if (automationOverflow) hostOverflows_.fetch_add(1);
    if (panic_.exchange(false) || automationOverflow) {
        session_.requestPanic();
        for (auto& h : midiHeld_) h.used = false;
        KeyboardEvent discarded;
        for (std::size_t i = 0; i < 256 && keyboard_.pop(discarded); ++i) {
        }
        if (!pinned) {
            sequencer_.reset();
            sequencePlayhead_.store(-1);
        }
        if (hostPointOverflow_) hostPointCount_ = 0;
    }
    hostPointOverflow_ = false;
    std::size_t events = 0;
    auto append = [&](Event e) {
        if (events == blockEvents_.size()) {
            panic_.store(true);
            return false;
        }
        blockEvents_[events++] = e;
        return true;
    };
    KeyboardEvent uiEvent;
    for (std::size_t drained = 0; drained < 256 && keyboard_.pop(uiEvent); ++drained) {
        if (uiEvent.documentRevision != audioDocumentRevision_.load(std::memory_order_acquire))
            continue;
        uiEvent.event.sample = clock;
        if (!append(uiEvent.event)) break;
    }
    std::size_t midiMessages = 0;
    for (const auto metadata : midi) {
        if (++midiMessages > 256) {
            panic_.store(true);
            hostOverflows_.fetch_add(1);
            break;
        }
        const auto* bytes = metadata.data;
        if (metadata.numBytes < 3) continue;
        const int kind = bytes[0] & 0xf0, channel = (bytes[0] & 0x0f) + 1, pitch = bytes[1] & 127;
        if (kind == 0xb0 && (bytes[1] == 120 || bytes[1] == 123)) {
            panic_.store(true);
            continue;
        }
        if (kind != 0x80 && kind != 0x90) continue;
        const auto sample =
            clock + static_cast<std::uint64_t>(std::clamp(metadata.samplePosition, 0, total - 1));
        if (kind == 0x90 && bytes[2] != 0) {
            auto free = std::find_if(midiHeld_.begin(), midiHeld_.end(),
                                     [](const auto& h) { return !h.used; });
            if (free == midiHeld_.end()) {
                panic_.store(true);
                continue;
            }
            *free = {true, channel, pitch, nextNote_++, ++midiOrder_};
            append({sample, EventType::note_on, free->id, static_cast<double>(pitch),
                    (bytes[2] & 127) / 127.0});
        } else {
            MidiHeld* first = nullptr;
            for (auto& h : midiHeld_)
                if (h.used && h.channel == channel && h.pitch == pitch &&
                    (!first || h.order < first->order))
                    first = &h;
            if (first) {
                append({sample, EventType::note_off, first->id, static_cast<double>(pitch), 0});
                first->used = false;
            }
        }
    }
    midi.clear();
    // Release after this block's MIDI, including a note-on already in the
    // block. The collector reset drops anything not yet copied here. The
    // note-off is stamped on the last sample and stays after that note-on
    // because the block sort is stable. UI keys and the sequencer are not in
    // midiHeld_. A full event buffer sets panic_ and the later panic path
    // resets the instrument, including those other notes.
    if (midiRelease_.exchange(false)) {
        for (auto& held : midiHeld_)
            if (held.used) {
                append({clock + static_cast<std::uint64_t>(total - 1), EventType::note_off, held.id,
                        static_cast<double>(held.pitch), 0});
                held.used = false;
            }
    }
    if (resetSequence || pinned != sequenceAudioPinned_) {
        // Release only the old scheduler's identities in the cursor instrument;
        // switching to a pinned source must preserve held keyboard notes.
        if (!sequenceAudioPinned_ && pinned) {
            if (!sequencer_.stop(clock, append)) panic_.store(true);
        } else
            sequencer_.reset();
        sequenceAudioPinned_ = pinned;
        sequencePlayhead_.store(-1);
    }
    SequencePattern sequence;
    if (sequencePatterns_.pop(sequence)) sequencer_.configure(sequence);
    sequenceSession_.beginBlock();
    const bool unitReady =
        !pinned || (sequenceSourcePlayable_.load(std::memory_order_acquire) &&
                    sequenceSession_.targetRevision() >=
                        sequenceMinimumRevision_.load(std::memory_order_acquire));
    // A newly prepared sound starts with fresh note identities; events skipped
    // during preparation must never later release nonexistent voices.
    if (unitReady && !sequenceWasReady_) sequencer_.reset();
    sequenceWasReady_ = unitReady;
    std::size_t sequenceEvents = 0;
    const auto sequenceAppend = [&](Event event) {
        if (!unitReady) return true;
        if (!pinned) return append(event);
        if (sequenceEvents == sequenceEvents_.size()) return false;
        sequenceEvents_[sequenceEvents++] = event;
        return true;
    };
    if (!sequencer_.process(demo_.load(), clock, static_cast<std::uint32_t>(total), rate_.load(),
                            transport, sequenceAppend)) {
        if (pinned) {
            sequenceSession_.requestPanic();
            sequenceEvents = 0;
            sequencer_.reset();
        } else
            panic_.store(true);
        hostOverflows_.fetch_add(1);
    }
    sequencePlayhead_.store(sequencer_.playhead());
    std::array<OutputGainGroup, sequencerRows> laneGains;
    for (std::size_t i = 0; i < sequencerRows; ++i) {
        const auto& lane = sequencer_.pattern().rows[i];
        laneGains[i] = {(1ull << 61) | (7ull << 56), (1ull << 61) | (i << 56),
                        i >= sequencer_.pattern().rowCount || lane.mute
                            ? 0.f
                            : static_cast<float>(std::pow(10., lane.volume / 20))};
    }
    session_.setOutputGains(laneGains);
    sequenceSession_.setOutputGains(laneGains);
    session_.setContinuousObservation(demo_.load() && transport.playing);
    // Stable insertion sort preserves host order at identical offsets without heap allocation.
    for (std::size_t i = 1; i < events; ++i) {
        auto e = blockEvents_[i];
        auto j = i;
        while (j > 0 && blockEvents_[j - 1].sample > e.sample) {
            blockEvents_[j] = blockEvents_[j - 1];
            --j;
        }
        blockEvents_[j] = e;
    }
    if (panic_.exchange(false)) {
        session_.requestPanic();
        events = 0;
        for (auto& h : midiHeld_) h.used = false;
        if (!pinned) {
            sequencer_.reset();
            sequencePlayhead_.store(-1);
        }
    }
    session_.beginBlock(std::min<std::size_t>(256, hostPointCount_ + 32));
    MacroMap map;
    if (macroMaps_.pop(map)) waitingMacros_ = map;
    const auto activeRevision = session_.targetRevision();
    const bool currentInstrument =
        sourcePlayable_.load(std::memory_order_acquire) &&
        activeRevision >= audioDocumentRevision_.load(std::memory_order_acquire);
    if (!currentInstrument) {
        session_.requestPanic();
        events = 0;
        for (auto& held : midiHeld_) held.used = false;
        if (!pinned) {
            sequencer_.reset();
            sequencePlayhead_.store(-1);
        }
    }
    std::array<bool, 32> changedMapping{};
    if (waitingMacros_.revision == activeRevision)
        for (std::size_t i = 0; i < 32; ++i) {
            const auto& old = macros_.bindings[i];
            const auto& next = waitingMacros_.bindings[i];
            changedMapping[i] = old.value != next.value || old.min != next.min ||
                                old.max != next.max || old.logarithmic != next.logarithmic;
        }
    if (waitingMacros_.revision == activeRevision) macros_ = waitingMacros_;
    std::size_t params = 0;
    std::array<bool, 32> received{};
    auto canonical = [](const MacroBinding& b, double normalized) {
        return b.logarithmic && b.min > 0 ? b.min * std::pow(b.max / b.min, normalized)
                                          : b.min + normalized * (b.max - b.min);
    };
    auto addParam = [&](int slot, int offset, float normalized, unsigned int duration = 0,
                        std::optional<double> start = {}) {
        const auto& b = macros_.bindings[static_cast<std::size_t>(slot)];
        if (b.value == invalid_value || macros_.revision != activeRevision) return;
        if (offset < 0 || offset >= total || params == blockParameters_.size()) {
            panic_.store(true);
            return;
        }
        ParameterUpdate update{clock + static_cast<std::uint64_t>(offset), b.value,
                               canonical(b, normalized), duration, b.logarithmic};
        if (start) update.ramp_start = canonical(b, *start);
        blockParameters_[params++] = update;
    };
    std::array<int, 32> previousOffset;
    previousOffset.fill(-1);
    for (std::size_t i = 0; i < hostPointCount_; ++i) {
        const auto point = hostPoints_[i];
        const int slot = point.slot;
        received[slot] = true;
        if (point.kind == 0) {
            // VST3 queues interpolate from an implicit point at offset -1.
            const int from = previousOffset[slot];
            if (point.offset < from || point.offset >= total) {
                panic_.store(true);
                continue;
            }
            if (point.offset == 0 || point.offset == from)
                addParam(slot, point.offset, point.value);
            else if (from < 0) {
                const double start =
                    previousMacro_[slot] +
                    (point.value - previousMacro_[slot]) / static_cast<double>(point.offset + 1);
                addParam(slot, 0, point.value, static_cast<unsigned int>(point.offset), start);
            } else
                addParam(slot, from, point.value, static_cast<unsigned int>(point.offset - from));
            previousOffset[slot] = point.offset;
        } else if (point.kind == 1 || point.duration == 0)
            addParam(slot, std::max(0, point.offset), point.value);
        else {
            const auto elapsed =
                point.offset < 0
                    ? static_cast<std::uint64_t>(-static_cast<std::int64_t>(point.offset))
                    : 0;
            const auto remaining = elapsed >= point.duration
                                       ? 0u
                                       : point.duration - static_cast<unsigned int>(elapsed);
            const double start = remaining == 0 ? point.value
                                                : point.start + (point.value - point.start) *
                                                                    static_cast<double>(elapsed) /
                                                                    point.duration;
            addParam(slot, std::max(0, point.offset), point.value, remaining, start);
        }
        previousMacro_[slot] = point.value;
    }
    for (int i = 0; i < 32; ++i)
        if (!received[i]) {
            const auto value = hostParams_[i]->get();
            if (changedMapping[i] || value != previousMacro_[i]) addParam(i, 0, value);
            previousMacro_[i] = value;
        }
    if (panic_.exchange(false)) {
        session_.requestPanic();
        events = 0;
        params = 0;
        for (auto& h : midiHeld_) h.used = false;
        if (!pinned) {
            sequencer_.reset();
            sequencePlayhead_.store(-1);
        }
    }
    for (std::size_t i = 1; i < params; ++i) {
        auto p = blockParameters_[i];
        auto j = i;
        while (j > 0 && blockParameters_[j - 1].sample > p.sample) {
            blockParameters_[j] = blockParameters_[j - 1];
            --j;
        }
        blockParameters_[j] = p;
    }
    if (!currentInstrument) params = 0;
    std::size_t e = 0, p = 0, se = 0;
    float mixedPeak = 0;
    bool mixedInvalid = false;
    const bool mixSequence =
        pinned && unitReady && demo_.load() && transport.playing && !sequenceAuditionMuted_.load();
    const auto routeFade =
        static_cast<std::uint32_t>(std::max(1.0, std::round(rate_.load() * 0.003)));
    for (int offset = 0; offset < total; offset += static_cast<int>(maxFrames)) {
        const auto count =
            static_cast<std::size_t>(std::min<int>(static_cast<int>(maxFrames), total - offset));
        const auto end = clock + offset + count;
        auto eb = e, pb = p, sb = se;
        while (e < events && blockEvents_[e].sample < end) ++e;
        while (p < params && blockParameters_[p].sample < end) ++p;
        while (se < sequenceEvents && sequenceEvents_[se].sample < end) ++se;
        auto positioned = transport;
        if (positioned.playing)
            positioned.ppq += double(offset) * positioned.bpm / (60.0 * rate_.load());
        session_.setTransport(positioned);
        session_.processStereo(std::span(blockEvents_).subspan(eb, e - eb),
                               std::span(mono_).first(count), std::span(right_).first(count),
                               std::span(blockParameters_).subspan(pb, p - pb), offset == 0);
        sequenceSession_.setTransport(positioned);
        sequenceSession_.processStereo(std::span(sequenceEvents_).subspan(sb, se - sb),
                                       std::span(sequenceLeft_).first(count),
                                       std::span(sequenceRight_).first(count), {}, offset == 0);
        std::fill_n(clickLeft_.begin(), count, 0);
        std::fill_n(clickRight_.begin(), count, 0);
        metronome_.process(demo_.load(), sequencer_.pattern().metronome, rate_.load(), positioned,
                           std::span(clickLeft_).first(count), std::span(clickRight_).first(count));
        const bool mixActive =
            sourcePlayable_.load(std::memory_order_acquire) &&
            activeRevision >= audioDocumentRevision_.load(std::memory_order_acquire);
        if (mixSequence != sequenceRouteEnabled_) {
            sequenceRouting_.mark();
            sequenceRouteEnabled_ = mixSequence;
        }
        for (std::size_t i = 0; i < count; ++i) {
            const auto sequenceAudio = sequenceRouting_.tick(
                mixSequence ? sequenceLeft_[i] : 0, mixSequence ? sequenceRight_[i] : 0, routeFade);
            float left = (mixActive ? mono_[i] : 0) + sequenceAudio[0] + clickLeft_[i];
            float right = (mixActive ? right_[i] : 0) + sequenceAudio[1] + clickRight_[i];
            if (!std::isfinite(left) || !std::isfinite(right)) {
                left = right = 0;
                mixedInvalid = true;
            }
            buffer.setSample(0, offset + static_cast<int>(i), left);
            if (buffer.getNumChannels() > 1)
                buffer.setSample(1, offset + static_cast<int>(i), right);
            mixedPeak = std::max(mixedPeak, std::max(std::abs(left), std::abs(right)));
        }
    }
    fallbackPpq_ = transport.ppq +
                   (transport.playing ? double(total) * transport.bpm / (60.0 * rate_.load()) : 0);
    if (!currentInstrument) {
        session_.status.peak.store(0);
        session_.status.voices.store(0);
    }
    if (mixedInvalid) {
        // A ramp may contain finite but enormous samples before it overflows.
        // Reject the whole callback and discard the routing correction as well.
        buffer.clear();
        mixedPeak = 0;
        sequenceRouting_ = {};
        panic_.store(true);
        sequencePanic_.store(true);
        session_.requestPanic();
        sequenceSession_.requestPanic();
        session_.status.errors.fetch_add(1);
    }
    mixedPeak_.store(mixedPeak);
    mixedVoices_.store((currentInstrument ? session_.status.voices.load() : 0) +
                       (mixSequence ? sequenceSession_.status.voices.load() : 0));
    rawHostPoints_ = false;
    hostPointCount_ = 0;
    auto elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    session_.status.cpu.store(static_cast<float>(100 * elapsed * rate_.load() / total));
}
juce::AudioProcessorEditor* FlowsProcessor::createEditor() {
    return new FlowsEditor(*this);
}

void FlowsProcessor::cacheActiveLocked() {
    if (document_.path.isEmpty()) return;
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
    for (std::size_t i = 0; i < 32; ++i) cached.macroValues[i] = hostParams_[i]->get();
    auto id = cached.document.path;
    inactiveDocuments_[id] = std::move(cached);
    document_ = {};
}
void FlowsProcessor::resetActiveAudioLocked() {
    bpm_.store(static_cast<double>(get(preferences_, "bpm")));
    const bool clean = !document_.dirty();
    document_.revision = ++nextRevision_;
    if (clean) document_.savedRevision = document_.revision;
    requestedRevision_ = document_.revision;
    preparedRevision_ = 0;
    compiled_.status = document_.path.isEmpty() ? "idle" : "pending";
    if (document_.path.isEmpty()) {
        compiled_.revision = 0;
        compiled_.graph = graphMetadata({}, {});
    }
    // A program from a different tab is never the fallback for this tab.
    audioDocumentRevision_.store(document_.revision, std::memory_order_release);
    sourcePlayable_.store(false, std::memory_order_release);
    panic_.store(true);
    session_.requestPanic();
    if (!sequenceSound_.pinned()) {
        demo_.store(false);
        sequencePatterns_.push(sequencePattern_);
        sequencePanic_.store(true);
        sequencePlayhead_.store(-1);
    }
    ++sequenceRevision_;
    auditionSignal_.clear();
    sequenceAuditionMuted_.store(false);
    ++selectionVersion_;
    latestTelemetry_ = object({{"programRevision", 0},
                               {"signals", juce::Array<juce::var>()},
                               {"gap", true},
                               {"analysisReady", false},
                               {"status", "waiting for selected instrument"}});
}
void FlowsProcessor::activateLocked(const juce::String& id) {
    if (id.isNotEmpty() && id == document_.path) return;
    if (id.isNotEmpty() && inactiveDocuments_.find(id) == inactiveDocuments_.end())
        fail("This instrument is not open");
    cacheActiveLocked();
    if (id.isNotEmpty()) {
        auto it = inactiveDocuments_.find(id);
        auto next = std::move(it->second);
        inactiveDocuments_.erase(it);
        document_ = std::move(next.document);
        compiled_ = std::move(next.compiled);
        exports_ = std::move(next.exports);
        playableSource_ = std::move(next.playableSource);
        sequencePattern_ = next.sequencer;
        documentSequenceSound_ = std::move(next.sequenceSound);
        selectedSignals_ = std::move(next.selectedSignals);
        selectedVoice_ = next.selectedVoice;
        projection_ = next.projection;
        for (std::size_t i = 0; i < 32; ++i)
            hostParams_[i]->setValueNotifyingHost(next.macroValues[i]);
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
        for (auto* p : hostParams_) p->setValueNotifyingHost(.5f);
    }
    if (!sequenceSound_.pinned())
        selectSequenceSoundLocked(documentSequenceSound_, sequencePattern_, document_.path);
    resetActiveAudioLocked();
    updateCheckpointLocked();
}
FlowsProcessor::SequenceSound FlowsProcessor::parseSequenceSound(const juce::var& value,
                                                                 bool capture) const {
    if (value.isVoid()) return {};
    if (!value.isObject()) fail("Sequencer sound must be an object");
    SequenceSound sound;
    sound.kind = str(value, "kind");
    if (sound.kind == "active") return sound;
    if (sound.kind == "lanes") {
        const auto* lanes = get(value, "lanes").getArray();
        if (!lanes || lanes->isEmpty() || lanes->size() > static_cast<int>(sequencerRows))
            fail("Choose one sound for each of 1–8 lanes");
        for (const auto& lane : *lanes) {
            if (str(lane, "kind") == "lanes") fail("Lane sounds cannot be nested");
            sound.lanes.push_back(parseSequenceSound(lane, capture));
        }
        return sound;
    }
    if (sound.kind != "instrument" && sound.kind != "sample")
        fail("Choose active, instrument or sample as the sequencer sound kind");
    sound.path = str(value, "path");
    if (sound.path.isEmpty() || sound.path.length() > 2048 || sound.path.containsAnyOf("\\\r\n") ||
        sound.path.startsWithChar('/') || sound.path.startsWithChar('.') ||
        sound.path.contains("/../") || sound.path.contains("/./"))
        fail("Choose a sound file inside this workspace");
    const auto file = workspace_.getChildFile(sound.path);
    if (!file.isAChildOf(workspace_) ||
        (sound.kind == "instrument" && !file.hasFileExtension("flows")) ||
        (sound.kind == "sample" && (!file.hasFileExtension("wav;aif;aiff;aifc"))))
        fail("Invalid sequencer sound path");
    if (sound.kind == "sample") {
        sound.playback = str(value, "playback");
        if (sound.playback.isEmpty()) sound.playback = "one_shot";
        if (sound.playback != "one_shot" && sound.playback != "gated")
            fail("Sample playback must be one_shot or gated");
        if (capture && !sampleAssets_.isManaged(sound.path))
            fail("Choose an imported sample from the Samples library");
        sound.source =
            "import standard as std\nfrom lib.sample import ClassicSampler\nfrom lib.env import "
            "ClassicADSR\n\nInst.sigstream(std.event.keys.poly()) as key:\n    sampler = "
            "ClassicSampler.make()\n    raw = sampler(file: " +
            juce::JSON::toString(juce::var(sound.path)) +
            ", trigger: key.trigger, pitch: key.pitch, root: C3, level: -6db)\n";
        if (sound.playback == "gated")
            sound.source +=
                "    env = ClassicADSR.make()\n    amp = env(gate: key.gate, trigger: key.trigger, "
                "A: 0ms, D: 0ms, S: 1, R: 5ms)\n    return raw * amp * key.velocity\n";
        else
            sound.source += "    return raw * key.velocity\n";
    } else if (capture) {
        {
            std::error_code status;
            const auto root = std::filesystem::weakly_canonical(
                workspace_.getFullPathName().toStdString(), status);
            if (status) fail("Could not resolve the sequencer workspace");
            const auto resolved =
                std::filesystem::weakly_canonical(file.getFullPathName().toStdString(), status);
            if (status || !juce::File(juce::String(resolved.string()))
                               .isAChildOf(juce::File(juce::String(root.string()))))
                fail("Sequencer sound escapes this workspace through a symbolic link");
            if (sound.path == document_.path)
                sound.source = document_.text;
            else if (const auto found = inactiveDocuments_.find(sound.path);
                     found != inactiveDocuments_.end())
                sound.source = found->second.document.text;
            else
                sound.source = readSourceFile(file);
        }
    } else
        sound.source = str(value, "source");
    if (sound.source.isEmpty() || sound.source.getNumBytesAsUTF8() > maximumSourceBytes)
        fail("Sequencer sound needs a source snapshot of at most 1 MiB");
    return sound;
}
juce::var FlowsProcessor::sequenceStateJSON(const SequencePattern& pattern,
                                            const SequenceSound& sound, bool includeSource) const {
    auto result = sequenceJSON(pattern);
    const auto encode = [&](const auto& self, const SequenceSound& item) -> juce::var {
        auto unit = object({{"kind", item.kind}});
        if (item.kind == "lanes") {
            juce::Array<juce::var> lanes;
            for (const auto& lane : item.lanes) lanes.add(self(self, lane));
            unit.getDynamicObject()->setProperty("lanes", lanes);
        } else if (item.pinned()) {
            if (item.kind == "sample")
                unit.getDynamicObject()->setProperty("playback", item.playback);
            unit.getDynamicObject()->setProperty("path", item.path);
            if (includeSource) unit.getDynamicObject()->setProperty("source", item.source);
        }
        return unit;
    };
    result.getDynamicObject()->setProperty("sound", encode(encode, sound));
    return result;
}

SequencePattern FlowsProcessor::loadSequenceLocked(const juce::String& path) const {
    const auto file = workspace_.getChildFile(".sequencers").getChildFile(path + ".json");
    try {
        if (file.isAChildOf(workspace_) && file.existsAsFile() &&
            file.getSize() <= static_cast<juce::int64>(maximumSequenceBytes))
            return parseSequence(parseJSONBounded(file.loadFileAsString(), maximumSequenceBytes));
    } catch (...) { /* A damaged optional pattern must not prevent source recovery. */
    }
    return rollingBassPattern();
}
FlowsProcessor::SequenceSound FlowsProcessor::loadSequenceSoundLocked(
    const juce::String& path) const {
    const auto file = workspace_.getChildFile(".sequencers").getChildFile(path + ".json");
    try {
        if (file.isAChildOf(workspace_) && file.existsAsFile() &&
            file.getSize() <= static_cast<juce::int64>(maximumSequenceBytes))
            return parseSequenceSound(
                get(parseJSONBounded(file.loadFileAsString(), maximumSequenceBytes), "sound"),
                false);
    } catch (...) {
    }
    return {};
}
void FlowsProcessor::saveSequenceLocked(const juce::String& path, const SequencePattern& sequence,
                                        const SequenceSound& sound) const {
    const auto file = workspace_.getChildFile(".sequencers").getChildFile(path + ".json");
    if (path.isEmpty() || !file.isAChildOf(workspace_))
        fail("Could not save the sequencer pattern");
    const auto text = juce::JSON::toString(sequenceStateJSON(sequence, sound), true);
    if (text.getNumBytesAsUTF8() > maximumSequenceBytes)
        fail("Sequencer pattern and source exceed the 8 MiB metadata limit");
    writePatternFile(file, text.toRawUTF8(), text.getNumBytesAsUTF8());
}
void FlowsProcessor::retainSequenceStateLocked() {
    if (!sequenceSound_.pinned()) {
        documentSequenceSound_ = {};
        return;
    }
    if (sequenceOwner_ == document_.path) {
        sequencePattern_ = pinnedSequencePattern_;
        documentSequenceSound_ = sequenceSound_;
    } else if (auto found = inactiveDocuments_.find(sequenceOwner_);
               found != inactiveDocuments_.end()) {
        found->second.sequencer = pinnedSequencePattern_;
        found->second.sequenceSound = sequenceSound_;
    }
}
void FlowsProcessor::selectSequenceSoundLocked(SequenceSound sound, const SequencePattern& pattern,
                                               juce::String owner) {
    const bool newUnit =
        juce::JSON::toString(get(sequenceStateJSON(pattern, sound, false), "sound")) !=
        juce::JSON::toString(get(sequenceStateJSON(pattern, sequenceSound_, false), "sound"));
    const bool changed =
        newUnit ||
        juce::JSON::toString(get(sequenceStateJSON(pattern, sound), "sound")) !=
            juce::JSON::toString(get(sequenceStateJSON(pattern, sequenceSound_), "sound"));
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
    sequencePinned_.store(sequenceSound_.pinned(), std::memory_order_release);
    sequencePatterns_.push(displayedSequenceLocked());
    if (changed) {
        ++sequenceUnitRevision_;
        if (newUnit) {
            ++sequenceUnitGeneration_;
            sequenceMinimumRevision_.store(sequenceUnitRevision_, std::memory_order_release);
            sequenceTargetRevision_.store(sequenceUnitRevision_, std::memory_order_release);
            sequencePanic_.store(true);
            sequenceSession_.requestPanic();
        }
        sequenceSoundError_.clear();
        sequenceSoundStatus_ = sequenceSound_.pinned() ? "pending" : "ready";
    }
}
void FlowsProcessor::remapSequenceSound(SequenceSound& sound, const SamplePathMap& paths) const {
    for (auto& lane : sound.lanes) remapSequenceSound(lane, paths);
    sound.source = remapSamplePaths(sound.source, paths);
    if (sound.kind == "sample")
        if (const auto found = paths.find(sound.path); found != paths.end())
            sound.path = found->second;
    if (sound.source.getNumBytesAsUTF8() > maximumSourceBytes)
        fail("Remapped sequencer source exceeds 1 MiB");
}
std::vector<juce::String> FlowsProcessor::packageSourcesLocked() const {
    std::vector<juce::String> sources{playableSource_, document_.text};
    (sequenceSound_.pinned() ? sequenceSound_ : documentSequenceSound_).collect(sources);
    return sources;
}
void FlowsProcessor::saveLocked(Document& document) {
    if (document.path.isEmpty()) fail("No instrument is open");
    auto file = workspace_.getChildFile(document.path);
    if (!file.isAChildOf(workspace_) ||
        !file.replaceWithData(document.text.toRawUTF8(), document.text.getNumBytesAsUTF8()))
        fail("Could not save instrument");
    const auto cached = inactiveDocuments_.find(document.path);
    saveSequenceLocked(
        document.path,
        document.path == document_.path ? sequencePattern_ : cached->second.sequencer,
        document.path == document_.path ? documentSequenceSound_ : cached->second.sequenceSound);
    document.savedText = document.text;
    document.savedRevision = document.revision;
    document.needsSave = false;
}
void FlowsProcessor::applyLocked(juce::String text, juce::String gesture, juce::String phase) {
    if (text.getNumBytesAsUTF8() > maximumSourceBytes)
        fail("Source exceeds the 1 MiB document limit");
    if (phase == "cancel" && gesture == document_.gesture) {
        text = document_.gestureStart;
        if (!document_.undo.empty()) document_.undo.pop_back();
    } else if (text != document_.text && (gesture.isEmpty() || gesture != document_.gesture)) {
        document_.undo.push_back({document_.text});
        if (document_.undo.size() > 256) document_.undo.erase(document_.undo.begin());
        document_.redo.clear();
        document_.gestureStart = document_.text;
    }
    if (text != document_.text) {
        document_.text = std::move(text);
        document_.revision = ++nextRevision_;
        requestedRevision_ = document_.revision;
        compiled_.status = "pending";
    }
    document_.gesture = phase.isNotEmpty() ? juce::String() : gesture;
    updateCheckpointLocked();
}
juce::var FlowsProcessor::packageLocked(bool includeAssets) const {
    juce::Array<juce::var> slots;
    for (int i = 0; i < 32; ++i)
        slots.add(object({{"slot", i + 1},
                          {"bindingId", exports_[i].binding},
                          {"name", exports_[i].name},
                          {"value", hostParams_[i]->get()},
                          {"structure", exports_[i].structure}}));
    auto package =
        object({{"format", "flows.instrument"},
                {"version", 1},
                {"empty", document_.path.isEmpty()},
                {"path", document_.path},
                {"source", document_.text},
                {"playableSource", playableSource_},
                {"macros", slots},
                {"preferences", preferences_},
                {"sequencer", sequenceStateJSON(displayedSequenceLocked(), sequenceSound_)}});
    if (includeAssets)
        package.getDynamicObject()->setProperty("assets",
                                                sampleAssets_.bundle(packageSourcesLocked(), true));
    return package;
}
void FlowsProcessor::updateCheckpointLocked() {
    try {
        auto assets = sampleAssets_.serializedBundle(packageSourcesLocked());
        auto text =
            juce::JSON::toString(packageLocked(false), true).dropLastCharacters(1) + ",\"assets\":";
        auto checkpoint = std::make_shared<Checkpoint>();
        checkpoint->prefix = juce::MemoryBlock(text.toRawUTF8(), text.getNumBytesAsUTF8());
        checkpoint->assets = std::move(assets);
        std::atomic_store(&checkpoint_, std::shared_ptr<const Checkpoint>(std::move(checkpoint)));
        checkpointError_.clear();
    } catch (const std::exception& e) {
        const juce::String error = juce::String("State assets could not be packed: ") + e.what();
        if (checkpointError_ != error) {
            checkpointError_ = error;
            agentEvents_.push_back(object({{"status", "failed"}, {"error", error}}));
        }
    }
}
juce::var FlowsProcessor::recoveryLocked(bool includeAssets) const {
    juce::Array<juce::var> documents;
    std::vector<juce::String> sources;
    auto append = [&](const Document& d, const juce::String& playable,
                      const std::array<Exported, 32>& exports, const std::array<float, 32>& values,
                      const SequencePattern& sequence, const SequenceSound& sound) {
        sources.push_back(playable);
        sources.push_back(d.text);
        sources.push_back(d.savedText);
        sources.push_back(sound.source);
        juce::Array<juce::var> slots;
        for (std::size_t i = 0; i < 32; ++i)
            slots.add(object({{"slot", static_cast<int>(i + 1)},
                              {"bindingId", exports[i].binding},
                              {"name", exports[i].name},
                              {"value", values[i]},
                              {"structure", exports[i].structure}}));
        documents.add(object({{"path", d.path},
                              {"source", d.text},
                              {"savedSource", d.savedText},
                              {"needsSave", d.needsSave},
                              {"playableSource", playable},
                              {"macros", slots},
                              {"sequencer", sequenceStateJSON(sequence, sound)}}));
    };
    for (const auto& id : documentOrder_) {
        if (id == document_.path) {
            std::array<float, 32> values{};
            for (std::size_t i = 0; i < 32; ++i) values[i] = hostParams_[i]->get();
            append(document_, playableSource_, exports_, values, sequencePattern_,
                   documentSequenceSound_);
        } else if (const auto it = inactiveDocuments_.find(id); it != inactiveDocuments_.end()) {
            const auto& d = it->second;
            append(d.document, d.playableSource, d.exports, d.macroValues, d.sequencer,
                   d.sequenceSound);
        }
    }
    auto recovery = object({{"format", "flows.workspace"},
                            {"version", 1},
                            {"activeDocument", document_.path},
                            {"documents", documents},
                            {"preferences", preferences_}});
    if (sequenceSound_.pinned()) {
        recovery.getDynamicObject()->setProperty(
            "pinnedSequencer",
            object({{"ownerDocumentId", sequenceOwner_},
                    {"pattern", sequenceStateJSON(pinnedSequencePattern_, sequenceSound_)}}));
        sequenceSound_.collect(sources);
    }
    if (includeAssets)
        recovery.getDynamicObject()->setProperty("assets", sampleAssets_.bundle(sources));
    return recovery;
}
std::vector<juce::String> FlowsProcessor::recoverySourcesLocked() const {
    std::vector<juce::String> sources;
    for (const auto& id : documentOrder_) {
        if (id == document_.path) {
            sources.push_back(playableSource_);
            sources.push_back(document_.text);
            sources.push_back(document_.savedText);
            documentSequenceSound_.collect(sources);
        } else if (const auto found = inactiveDocuments_.find(id);
                   found != inactiveDocuments_.end()) {
            sources.push_back(found->second.playableSource);
            sources.push_back(found->second.document.text);
            sources.push_back(found->second.document.savedText);
            found->second.sequenceSound.collect(sources);
        }
    }
    if (sequenceSound_.pinned()) sequenceSound_.collect(sources);
    return sources;
}
void FlowsProcessor::loadRecoveryLocked(const juce::var& recovery) {
    if (str(recovery, "format") != "flows.workspace" ||
        static_cast<int>(get(recovery, "version")) != 1)
        fail("Unsupported workspace recovery");
    const auto list = get(recovery, "documents");
    const auto* entries = list.getArray();
    if (!entries || entries->size() > static_cast<int>(maximumOpenDocuments))
        fail("Invalid workspace document list");
    // Reject malformed document metadata before publishing any embedded files.
    std::set<juce::String> recoveredPaths;
    for (const auto& entry : *entries) {
        const auto path = str(entry, "path");
        const auto file = workspace_.getChildFile(path);
        if (path.isEmpty() || !file.isAChildOf(workspace_) || !file.hasFileExtension("flows") ||
            !recoveredPaths.insert(file.getRelativePathFrom(workspace_)).second ||
            str(entry, "source").getNumBytesAsUTF8() > maximumSourceBytes ||
            str(entry, "savedSource").getNumBytesAsUTF8() > maximumSourceBytes ||
            str(entry, "playableSource").getNumBytesAsUTF8() > maximumSourceBytes)
            fail("Invalid recovered instrument metadata");
        if (entry.hasProperty("sequencer")) {
            (void)parseSequence(get(entry, "sequencer"));
            (void)parseSequenceSound(get(get(entry, "sequencer"), "sound"), false);
        }
    }
    const auto pin = get(recovery, "pinnedSequencer");
    SequenceSound recoveredSound;
    SequencePattern recoveredPattern;
    if (pin.isObject()) {
        recoveredPattern = parseSequence(get(pin, "pattern"));
        recoveredSound = parseSequenceSound(get(get(pin, "pattern"), "sound"), false);
    }
    const auto assetPaths = sampleAssets_.restore(get(recovery, "assets"));
    remapSequenceSound(recoveredSound, assetPaths);
    std::map<juce::String, OpenDocument> documents;
    std::vector<juce::String> order;
    for (const auto& entry : *entries) {
        OpenDocument opened;
        auto& d = opened.document;
        if (entry.hasProperty("sequencer")) {
            opened.sequencer = parseSequence(get(entry, "sequencer"));
            opened.sequenceSound = parseSequenceSound(get(get(entry, "sequencer"), "sound"), false);
            remapSequenceSound(opened.sequenceSound, assetPaths);
        }
        d.path = str(entry, "path");
        d.text = str(entry, "source");
        d.savedText = str(entry, "savedSource");
        d.needsSave = static_cast<bool>(get(entry, "needsSave"));
        const auto originalSource = d.text;
        d.text = remapSamplePaths(d.text, assetPaths);
        d.needsSave = d.needsSave || d.text != originalSource;
        opened.playableSource = remapSamplePaths(str(entry, "playableSource"), assetPaths);
        opened.macroValues.fill(.5f);
        const auto file = workspace_.getChildFile(d.path);
        if (d.path.isEmpty() || !file.isAChildOf(workspace_) || !file.hasFileExtension("flows") ||
            d.text.getNumBytesAsUTF8() > maximumSourceBytes ||
            d.savedText.getNumBytesAsUTF8() > maximumSourceBytes ||
            opened.playableSource.getNumBytesAsUTF8() > maximumSourceBytes)
            fail("Invalid recovered instrument");
        d.path = file.getRelativePathFrom(workspace_);
        if (documents.contains(d.path)) fail("Duplicate recovered instrument");
        if (!d.dirty()) {
            if (file.existsAsFile()) {
                try {
                    auto current = readSourceFile(file);
                    if (current != d.text) opened.playableSource.clear();
                    d.text = std::move(current);
                    d.savedText = d.text;
                } catch (const std::exception& error) {
                    // Keep the complete recovered source if the disk file is
                    // unreadable or oversized; never load a truncated prefix.
                    d.needsSave = true;
                    agentEvents_.push_back(object(
                        {{"status", "failed"},
                         {"error", juce::String("Recovered source retained: ") + error.what()}}));
                }
            } else
                d.needsSave = true;
        }
        d.revision = ++nextRevision_;
        d.savedRevision = !d.dirty() ? d.revision : 0;
        opened.compiled.graph = graphMetadata({}, {});
        const auto macros = get(entry, "macros");
        if (const auto* slots = macros.getArray())
            for (const auto& slot : *slots) {
                const int i = static_cast<int>(get(slot, "slot")) - 1;
                if (i < 0 || i >= 32) continue;
                opened.exports[i] = {str(slot, "bindingId"), str(slot, "name"), false,
                                     str(slot, "structure")};
                const auto value = static_cast<float>(get(slot, "value"));
                opened.macroValues[i] =
                    std::isfinite(value) ? juce::jlimit(0.0f, 1.0f, value) : .5f;
            }
        order.push_back(d.path);
        const auto path = d.path;
        documents[path] = std::move(opened);
    }
    auto active = str(recovery, "activeDocument");
    if (!order.empty() && !documents.contains(active)) active = order.front();
    document_ = {};
    compiled_ = {};
    inactiveDocuments_ = std::move(documents);
    documentOrder_ = std::move(order);
    const auto prefs = get(recovery, "preferences");
    if (prefs.isObject()) preferences_ = preferencesWithPatch(prefs);
    sequenceSound_ = {};
    sequencePinned_.store(false);
    demo_.store(false);
    sequencePanic_.store(true);
    sequenceSession_.requestPanic();
    activateLocked(active);
    if (recoveredSound.pinned()) {
        auto owner = str(pin, "ownerDocumentId");
        if (owner.isEmpty() || !workspace_.getChildFile(owner).isAChildOf(workspace_))
            owner = active;
        selectSequenceSoundLocked(std::move(recoveredSound), recoveredPattern, owner);
    }
}
void FlowsProcessor::loadPackageLocked(const juce::var& package, bool asNewDocument) {
    if (str(package, "format") != "flows.instrument" ||
        static_cast<int>(get(package, "version")) != 1)
        fail("Unsupported InstrumentPackage");
    if (static_cast<bool>(get(package, "empty"))) {
        if (asNewDocument) fail("This package contains no instrument");
        document_ = {};
        compiled_ = {};
        inactiveDocuments_.clear();
        documentOrder_.clear();
        const auto prefs = get(package, "preferences");
        if (prefs.isObject()) preferences_ = preferencesWithPatch(prefs);
        sequenceSound_ = {};
        sequencePinned_.store(false);
        demo_.store(false);
        sequencePanic_.store(true);
        sequenceSession_.requestPanic();
        activateLocked({});
        return;
    }
    OpenDocument opened;
    auto& d = opened.document;
    if (package.hasProperty("sequencer")) {
        opened.sequencer = parseSequence(get(package, "sequencer"));
        opened.sequenceSound = parseSequenceSound(get(get(package, "sequencer"), "sound"), false);
    }
    d.text = str(package, "source");
    opened.playableSource = str(package, "playableSource");
    if (d.text.getNumBytesAsUTF8() > maximumSourceBytes ||
        opened.playableSource.getNumBytesAsUTF8() > maximumSourceBytes)
        fail("Instrument source exceeds 1 MiB");
    if (asNewDocument && documentOrder_.size() >= maximumOpenDocuments)
        fail("Close an instrument before opening another (32 open instruments maximum)");
    d.path = juce::File(str(package, "path")).getFileName();
    if (!d.path.endsWith(".flows")) d.path = "Imported.flows";
    if (asNewDocument &&
        (workspace_.getChildFile(d.path).exists() ||
         std::find(documentOrder_.begin(), documentOrder_.end(), d.path) != documentOrder_.end())) {
        const auto base = workspace_.getChildFile(d.path);
        for (int suffix = 1;; ++suffix) {
            const auto candidate =
                base.getFileNameWithoutExtension() + " (import " + juce::String(suffix) + ").flows";
            if (!workspace_.getChildFile(candidate).exists() &&
                std::find(documentOrder_.begin(), documentOrder_.end(), candidate) ==
                    documentOrder_.end()) {
                d.path = candidate;
                break;
            }
        }
    }
    opened.sequenceSound.rename(str(package, "path"), d.path);
    // Explicit import opens a separate draft. DAW state recall replaces the
    // session with its portable active instrument instead of accumulating tabs.
    d.savedText.clear();
    d.savedRevision = 0;
    d.needsSave = true;
    opened.compiled.graph = graphMetadata({}, {});
    opened.macroValues.fill(.5f);
    auto savedMacros = get(package, "macros");
    if (const auto* macros = savedMacros.getArray())
        for (const auto& slot : *macros) {
            const int i = static_cast<int>(get(slot, "slot")) - 1;
            if (i < 0 || i >= 32) continue;
            opened.exports[i] = {str(slot, "bindingId"), str(slot, "name"), false,
                                 str(slot, "structure")};
            const auto value = static_cast<float>(get(slot, "value"));
            opened.macroValues[i] = std::isfinite(value) ? juce::jlimit(0.0f, 1.0f, value) : .5f;
        }
    const auto assetPaths = sampleAssets_.restore(get(package, "assets"));
    d.text = remapSamplePaths(d.text, assetPaths);
    opened.playableSource = remapSamplePaths(opened.playableSource, assetPaths);
    remapSequenceSound(opened.sequenceSound, assetPaths);
    if (d.text.getNumBytesAsUTF8() > maximumSourceBytes ||
        opened.playableSource.getNumBytesAsUTF8() > maximumSourceBytes)
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
    auto prefs = get(package, "preferences");
    if (prefs.isObject()) preferences_ = preferencesWithPatch(prefs);
    sequenceSound_ = {};
    sequencePinned_.store(false);
    demo_.store(false);
    sequencePanic_.store(true);
    sequenceSession_.requestPanic();
    activateLocked(id);
}
void FlowsProcessor::getStateInformation(juce::MemoryBlock& dest) {
    if (auto snapshot = std::atomic_load(&checkpoint_)) {
        // Source/dependencies are prebuilt off audio. Only a bounded 32-float
        // runtime overlay is sampled at the instant the DAW asks to save.
        dest.reset();
        juce::MemoryOutputStream stream(dest, false);
        const auto size = snapshot->prefix.getSize() + snapshot->assets->getSize() + 1;
        stream.writeInt(0x464c5731);
        stream.writeInt(static_cast<int>(size));
        for (auto* parameter : hostParams_) stream.writeFloat(parameter->get());
        stream.write(snapshot->prefix.getData(), snapshot->prefix.getSize());
        stream.write(snapshot->assets->getData(), snapshot->assets->getSize());
        stream.writeByte('}');
    }
}
void FlowsProcessor::setStateInformation(const void* data, int size) {
    if (size > 0 && static_cast<std::size_t>(size) <= maximumInstrumentPackageBytes + 136)
        std::atomic_store(&restore_, std::make_shared<const juce::MemoryBlock>(
                                         data, static_cast<std::size_t>(size)));
}
void FlowsProcessor::updateMacrosLocked() {
    MacroMap map;
    map.revision = compiled_.revision;
    const auto structure =
        compiled_.program ? bindingStructure(*compiled_.program) : juce::String();
    for (std::size_t i = 0; i < exports_.size(); ++i) {
        auto& e = exports_[i];
        e.bound = false;
        if (e.structure.isEmpty()) e.structure = structure;
        if (e.structure != structure) continue;
        if (compiled_.program)
            for (const auto& p : compiled_.program->parameters)
                if (p.binding_id == e.binding.toStdString() && p.live) {
                    e.bound = true;
                    map.bindings[i] = {p.value, p.min, p.max, p.logarithmic};
                    break;
                }
    }
    macroMaps_.push(map);
}
void FlowsProcessor::refreshAI(bool force) {
    const auto now = juce::Time::getMillisecondCounterHiRes();
    if (!force && now < aiRefreshAt_) return;
    aiRefreshAt_ = now + 500;
    juce::var settings;
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
        aiProfiles_ = get(settings, "profiles");
        aiSelectedId_ = str(settings, "selectedId");
    }
}
juce::var FlowsProcessor::snapshot() {
    std::lock_guard lock(mutex_);
    refreshAI();
    auto compileView = [](const CompileView& view) {
        return object(
            {{"status", view.status},
             {"revision", static_cast<juce::int64>(view.revision)},
             {"diagnostics", view.diagnostics},
             {"signals", view.signals},
             {"parameters", view.parameters},
             {"choices", view.choices},
             {"graph", view.graph},
             {"voiceLimit",
              view.program && view.program->voice_policy.mode == VoiceMode::mono ? 1 : 32},
             {"outputChannels",
              view.program ? static_cast<int>(view.program->nodes[view.program->output].channels)
                           : 1}});
    };
    auto documentView = [&](const Document& d, const CompileView& view) {
        return object({{"id", d.path},
                       {"path", d.path},
                       {"text", d.text},
                       {"revision", static_cast<juce::int64>(d.revision)},
                       {"savedRevision", static_cast<juce::int64>(d.savedRevision)},
                       {"dirty", d.dirty()},
                       {"compile", compileView(view)}});
    };
    juce::Array<juce::var> documents;
    for (const auto& id : documentOrder_) {
        if (id == document_.path)
            documents.add(documentView(document_, compiled_));
        else if (const auto it = inactiveDocuments_.find(id); it != inactiveDocuments_.end())
            documents.add(documentView(it->second.document, it->second.compiled));
    }
    juce::Array<juce::var> slots;
    for (int i = 0; i < 32; ++i)
        slots.add(object({{"slot", i + 1},
                          {"bindingId", exports_[i].binding},
                          {"name", exports_[i].name},
                          {"bound", exports_[i].bound},
                          {"value", hostParams_[i]->get()}}));
    const auto audioRevision = session_.status.revision.load();
    const bool selectedAudio = sourcePlayable_.load() && document_.path.isNotEmpty() &&
                               audioRevision >= audioDocumentRevision_.load();
    juce::Array<juce::var> observed;
    for (const auto& name : selectedSignals_) observed.add(juce::String(name));
    auto sequence = sequenceStateJSON(displayedSequenceLocked(), sequenceSound_, false);
    sequence.getDynamicObject()->setProperty(
        "ownerDocumentId", sequenceSound_.pinned() ? sequenceOwner_ : document_.path);
    const auto unitStatus =
        sequenceSound_.pinned() && sequenceSoundStatus_ == "ready" &&
                sequenceSession_.status.revision.load() < sequenceTargetRevision_.load()
            ? juce::String("pending")
            : sequenceSoundStatus_;
    sequence.getDynamicObject()->setProperty("soundStatus", unitStatus);
    sequence.getDynamicObject()->setProperty("soundError", sequenceSoundError_);
    sequence.getDynamicObject()->setProperty("playing", demo_.load());
    sequence.getDynamicObject()->setProperty("revision",
                                             static_cast<juce::int64>(sequenceRevision_));
    return object(
        {{"sequencer", sequence},
         {"sessionId", juce::String::toHexString(reinterpret_cast<juce::pointer_sized_int>(this))},
         {"workspaceId", juce::SHA256(workspace_.getFullPathName().toRawUTF8(),
                                      workspace_.getFullPathName().getNumBytesAsUTF8())
                             .toHexString()},
         {"document", documentView(document_, compiled_)},
         {"documents", documents},
         {"files", files_},
         {"samples", samples_},
         {"compile", compileView(compiled_)},
         {"observation", object({{"voice", selectedVoice_},
                                 {"signals", observed},
                                 {"projection", projectionName(projection_)}})},
         {"audio",
          object({{"bpm", hostTempo_.load() ? reportedBpm_.load() : bpm_.load()},
                  {"tempoSource", hostTempo_.load() ? "host" : "internal"},
                  {"running", running_.load()},
                  {"sampleRate", rate_.load()},
                  {"blockSize", blockSize_.load()},
                  {"voices",
                   sequenceSound_.pinned()
                       ? static_cast<int>(mixedVoices_.load())
                       : (selectedAudio ? static_cast<int>(session_.status.voices.load()) : 0)},
                  {"cpu", session_.status.cpu.load()},
                  {"peak", mixedPeak_.load()},
                  {"programRevision", static_cast<juce::int64>(selectedAudio ? audioRevision : 0)},
                  {"demoRunning", demo_.load()},
                  {"audition", auditionSignal_}})},
         {"ai", object({{"profiles", aiProfiles_},
                        {"selectedId", aiSelectedId_},
                        {"settingsError", aiSettingsError_},
                        {"configured", aiModel_.isNotEmpty() && isAllowedAIEndpoint(aiEndpoint_) &&
                                           (apiKey_.isNotEmpty() ||
                                            juce::URL(aiEndpoint_).getDomain() == "localhost" ||
                                            juce::URL(aiEndpoint_).getDomain() == "127.0.0.1")},
                        {"model", aiModel_},
                        {"endpoint", aiEndpoint_},
                        {"busy", aiBusy_.load()},
                        {"runningConversationId", runningConversationId_},
                        {"runningRequestId", runningRequestId_},
                        {"activeConversationId", assistantHistory_.activeId()},
                        {"historyError", assistantHistory_.error()}})},
         {"preferences", preferences_},
         {"exportedParameters", slots},
         {"capabilities", object({{"windowTransparency", windowTransparency.load()}})}});
}
juce::var FlowsProcessor::conversationViewLocked() {
    auto result = assistantHistory_.view();
    auto conversation = get(result, "conversation");
    if (auto* messages = get(conversation, "messages").getArray()) {
        for (auto& message : *messages) {
            const auto it = proposals_.find(str(message, "id"));
            const bool available = it != proposals_.end() && proposalCurrentLocked(it->second) &&
                                   it->second.conversationId == str(conversation, "id") &&
                                   it->second.text == str(message, "proposedSource") &&
                                   !static_cast<bool>(get(conversation, "deleted")) &&
                                   !static_cast<bool>(get(conversation, "archived"));
            message.getDynamicObject()->setProperty("proposalAvailable", available);
        }
    }
    result.getDynamicObject()->setProperty("runningConversationId", runningConversationId_);
    result.getDynamicObject()->setProperty("runningRequestId", runningRequestId_);
    return result;
}
juce::var FlowsProcessor::importSample(const juce::File& file) {
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
    result.getDynamicObject()->setProperty("sampleImportedPath", imported);
    return result;
}
juce::var FlowsProcessor::telemetry() {
    std::lock_guard lock(mutex_);
    return latestTelemetry_;
}
std::vector<juce::var> FlowsProcessor::takeAgentEvents() {
    std::lock_guard lock(mutex_);
    std::vector<juce::var> result;
    result.swap(agentEvents_);
    return result;
}
juce::var FlowsProcessor::request(const juce::String& method, const juce::var& params) {
    if (method == "session.get") return snapshot();
    if (method == "ai.send") {
        sendAI(params);
        std::lock_guard lock(mutex_);
        return object({{"status", "working"},
                       {"requestId", str(params, "requestId")},
                       {"conversationId", str(params, "conversationId").isNotEmpty()
                                              ? str(params, "conversationId")
                                              : assistantHistory_.activeId()}});
    }
    if (method == "audio.settings") {
        if (showAudioSettings)
            showAudioSettings();
        else
            fail("Select audio and MIDI devices in your plugin host.");
        return snapshot();
    }
    std::unique_lock lock(mutex_);
    auto requireRevision = [&](const char* field = "baseRevision") {
        if (document_.path.isEmpty()) fail("No instrument is open");
        if (static_cast<juce::int64>(get(params, field)) !=
            static_cast<juce::int64>(document_.revision))
            fail("The document changed. Refresh before applying this edit.");
        auto id = str(params, "documentId");
        if (id.isNotEmpty() && id != document_.path) fail("This edit belongs to another document.");
    };
    const auto requestedDocument = str(params, "documentId");
    if (method == "keyboard.noteOff" && requestedDocument.isNotEmpty() &&
        requestedDocument != document_.path) {
        // Activation has already released the old instrument. Its eventual
        // pointer/key release is harmless and must not report an edit conflict.
        lock.unlock();
        return snapshot();
    }
    if (method == "document.undo" || method == "document.redo" || method.startsWith("keyboard.") ||
        method.startsWith("probe.") || method.startsWith("parameter.") ||
        method == "audio.playDemo") {
        if (requestedDocument.isNotEmpty() && requestedDocument != document_.path)
            fail("This action belongs to another instrument");
        if (document_.path.isEmpty() && method != "keyboard.panic") fail("No instrument is open");
    }
    if (method.startsWith("sequencer.") && requestedDocument.isNotEmpty() &&
        requestedDocument != document_.path && requestedDocument != sequenceOwner_)
        fail("This sequencer action belongs to another instrument");
    if (method.startsWith("ai.conversations.")) {
        const auto action = method.fromFirstOccurrenceOf("ai.conversations.", false, false),
                   id = str(params, "conversationId");
        if (action == "list") {
        } else if (action == "create")
            assistantHistory_.create(str(params, "title"));
        else if (action == "select")
            assistantHistory_.select(id);
        else if (action == "rename")
            assistantHistory_.rename(id, str(params, "title"));
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
            std::erase_if(proposals_,
                          [&](const auto& item) { return item.second.conversationId == id; });
        return conversationViewLocked();
    }
    if (method == "workspace.import") {
        const auto folder = workspaceFolder(workspace_, str(params, "folder"));
        const auto name = str(params, "name");
        if (name.isEmpty() || name.startsWithChar('.') || name.containsAnyOf("/\\:"))
            fail("Use a file name without a path");
        const auto encoded = str(params, "data");
        if (encoded.length() > 90 * 1024 * 1024) fail("Import files of at most 64 MiB");
        juce::MemoryOutputStream bytes;
        if (!juce::Base64::convertFromBase64(bytes, encoded) ||
            bytes.getDataSize() > 64 * 1024 * 1024)
            fail("Invalid imported file data");
        if (!folder.createDirectory()) fail("Could not create destination folder");
        auto target = folder.getChildFile(name).getNonexistentSibling();
        if (target.hasFileExtension("flows")) {
            if (bytes.getDataSize() > maximumSourceBytes) fail("Instrument source exceeds 1 MiB");
            const auto* text = static_cast<const char*>(bytes.getData());
            if (!juce::CharPointer_UTF8::isValidString(text, static_cast<int>(bytes.getDataSize())))
                fail("Instrument source must be UTF-8");
            juce::TemporaryFile temp(target);
            if (!temp.getFile().replaceWithData(bytes.getData(), bytes.getDataSize()))
                fail("Could not write imported instrument");
            std::error_code error;
            std::filesystem::create_hard_link(temp.getFile().getFullPathName().toStdString(),
                                              target.getFullPathName().toStdString(), error);
            if (error) fail("Could not publish imported instrument without overwriting a file");
        } else {
            if (!target.hasFileExtension("wav;aif;aiff;aifc;flac;mp3;ogg"))
                fail("Drop a .flows instrument or an audio file");
            juce::TemporaryFile temp(target);
            if (!temp.getFile().replaceWithData(bytes.getData(), bytes.getDataSize()))
                fail("Could not read imported audio");
            // Preserve the original extension and name for format detection.
            const auto staging = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                     .getChildFile("flows-import-" + juce::Uuid().toString());
            staging.createDirectory();
            const auto staged = staging.getChildFile(name);
            struct Cleanup {
                juce::File path;
                ~Cleanup() { path.deleteRecursively(); }
            } cleanup{staging};
            if (!temp.getFile().copyFileTo(staged)) fail("Could not stage imported audio");
            target =
                workspace_.getChildFile(sampleAssets_.importAudio(staged, str(params, "folder")));
        }
        files_ = workspaceFiles(workspace_);
        samples_ = sampleAssets_.catalogue();
        const auto imported = target.getRelativePathFrom(workspace_);
        lock.unlock();
        auto result = snapshot();
        result.getDynamicObject()->setProperty("importedPath", imported);
        return result;
    }
    if (method == "document.new") {
        if (documentOrder_.size() >= maximumOpenDocuments)
            fail("Close an instrument before opening another (32 open instruments maximum)");
        auto name = str(params, "name").trim();
        if (name.isEmpty()) name = "Instrument.flows";
        if (name.containsAnyOf("/\\") || name.startsWithChar('.'))
            fail("Use a file name inside this workspace");
        auto folder =
            workspaceFolder(workspace_, str(params, "folder").isEmpty() ? juce::String("examples")
                                                                        : str(params, "folder"));
        if (!folder.createDirectory()) fail("Could not create instrument folder");
        auto file = folder.getChildFile(name).withFileExtension("flows").getNonexistentSibling();
        const auto base = file.getFileNameWithoutExtension();
        for (int suffix = 2;
             std::find(documentOrder_.begin(), documentOrder_.end(),
                       file.getRelativePathFrom(workspace_)) != documentOrder_.end();
             ++suffix)
            file = folder.getChildFile(base + " " + juce::String(suffix) + ".flows")
                       .getNonexistentSibling();
        const auto samplePath = str(params, "samplePath");
        if (samplePath.isNotEmpty()) {
            if (!sampleAssets_.isManaged(samplePath))
                fail("Choose an imported sample from the Samples library");
            const auto source =
                "import standard as std\nfrom lib.sample import "
                "ClassicSampler\n\nInst.sigstream(std.event.keys.poly()) as key:\n    sampler = "
                "ClassicSampler.make()\n    raw = sampler(file: " +
                juce::JSON::toString(juce::var(samplePath)) +
                ", trigger: key.trigger, pitch: key.pitch, root: C3, level: -6db)\n    p_raw = "
                "probe(raw)\n    return raw * key.velocity\n";
            if (!file.replaceWithText(source)) fail("Could not create sampler instrument");
            lock.unlock();
            return request("document.open",
                           object({{"path", file.getRelativePathFrom(workspace_)}}));
        }
        int length = 0;
        const char* bytes = nullptr;
        for (int i = 0; i < FlowsAssets::namedResourceListSize; ++i)
            if (juce::String(FlowsAssets::originalFilenames[i]).endsWith("sine-reference.flows"))
                bytes = FlowsAssets::getNamedResource(FlowsAssets::namedResourceList[i], length);
        if (!bytes || !file.replaceWithData(bytes, static_cast<std::size_t>(length)))
            fail("Could not create instrument");
        lock.unlock();
        return request("document.open", object({{"path", file.getRelativePathFrom(workspace_)}}));
    } else if (method == "document.rename") {
        if (document_.path.isEmpty()) fail("No instrument is open");
        auto name = str(params, "name").trim();
        if (name.isEmpty() || name.containsAnyOf("/\\") || name.startsWithChar('.'))
            fail("Use a file name inside this workspace");
        const auto id = str(params, "documentId");
        if (id.isNotEmpty() && id != document_.path) fail("The current document changed");
        auto original = workspace_.getChildFile(document_.path),
             target = original.getParentDirectory().getChildFile(name).withFileExtension("flows");
        const auto nextPath = target.getRelativePathFrom(workspace_);
        if (target != original && (target.exists() || inactiveDocuments_.contains(nextPath)))
            fail("An instrument with this name already exists");
        const bool prepareSequence = target != original && !document_.needsSave;
        auto renamedSound = documentSequenceSound_;
        renamedSound.rename(document_.path, nextPath);
        struct MetadataBackup {
            juce::File file;
            bool existed = false;
            juce::MemoryBlock bytes;
        };
        std::vector<MetadataBackup> metadata;
        const auto stage = [&](const juce::String& path, const SequencePattern& pattern,
                               const SequenceSound& sound) {
            MetadataBackup backup;
            backup.file = workspace_.getChildFile(".sequencers").getChildFile(path + ".json");
            backup.existed = backup.file.exists();
            if (backup.existed) {
                if (!backup.file.existsAsFile() ||
                    backup.file.getSize() > static_cast<juce::int64>(maximumSequenceBytes))
                    fail("Could not preserve sequencer metadata before rename");
                auto input = backup.file.createInputStream();
                if (!input) fail("Could not preserve sequencer metadata before rename");
                input->readIntoMemoryBlock(backup.bytes, maximumSequenceBytes + 1);
                if (input->getStatus().failed() || backup.bytes.getSize() > maximumSequenceBytes)
                    fail("Could not preserve sequencer metadata before rename");
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
                !inactiveDocuments_.contains(sequenceOwner_) &&
                workspace_.getChildFile(sequenceOwner_).existsAsFile()) {
                auto sound = sequenceSound_;
                sound.rename(document_.path, nextPath);
                stage(sequenceOwner_, pinnedSequencePattern_, sound);
            }
            if (target != original && original.existsAsFile() && !original.moveFileTo(target))
                fail("Could not rename instrument");
        } catch (...) {
            for (auto item = metadata.rbegin(); item != metadata.rend(); ++item) {
                if (item->existed)
                    writePatternFile(item->file, item->bytes.getData(), item->bytes.getSize());
                else
                    item->file.deleteFile();
            }
            throw;
        }
        for (auto& path : documentOrder_)
            if (path == document_.path) path = nextPath;
        const auto previousPath = document_.path;
        document_.path = nextPath;
        documentSequenceSound_ = std::move(renamedSound);
        for (auto& [path, opened] : inactiveDocuments_)
            opened.sequenceSound.rename(previousPath, nextPath);
        if (sequenceOwner_ == previousPath) sequenceOwner_ = nextPath;
        sequenceSound_.rename(previousPath, nextPath);
        documentSequenceSound_.rename(previousPath, nextPath);
        retainSequenceStateLocked();
        if (previousPath != nextPath)
            workspace_.getChildFile(".sequencers")
                .getChildFile(previousPath + ".json")
                .deleteFile();
        updateCheckpointLocked();
    } else if (method == "document.open") {
        auto file = workspace_.getChildFile(str(params, "path"));
        if (!file.isAChildOf(workspace_) || !file.hasFileExtension("flows"))
            fail("Instrument is outside this workspace or cannot be opened");
        const auto id = file.getRelativePathFrom(workspace_);
        if (id != document_.path && !inactiveDocuments_.contains(id)) {
            if (!file.existsAsFile() || file.getSize() > maximumSourceBytes)
                fail("Instrument cannot be opened");
            if (documentOrder_.size() >= maximumOpenDocuments)
                fail("Close an instrument before opening another (32 open instruments maximum)");
            OpenDocument opened;
            opened.document.path = id;
            opened.sequencer = loadSequenceLocked(id);
            opened.sequenceSound = loadSequenceSoundLocked(id);
            opened.document.text = readSourceFile(file);
            opened.document.savedText = opened.document.text;
            opened.compiled.graph = graphMetadata({}, {});
            opened.macroValues.fill(.5f);
            inactiveDocuments_[id] = std::move(opened);
            documentOrder_.push_back(id);
        }
        activateLocked(id);
    } else if (method == "document.activate") {
        const auto id = str(params, "documentId");
        if (id.isEmpty()) fail("Select an open instrument");
        activateLocked(id);
    } else if (method == "document.close") {
        const auto id = requestedDocument.isNotEmpty() ? requestedDocument : document_.path;
        const auto position = std::find(documentOrder_.begin(), documentOrder_.end(), id);
        if (position == documentOrder_.end()) fail("This instrument is not open");
        const bool active = id == document_.path;
        auto& target = active ? document_ : inactiveDocuments_.at(id).document;
        if (target.dirty()) {
            if (static_cast<bool>(get(params, "save")))
                saveLocked(target);
            else if (!static_cast<bool>(get(params, "discard")))
                fail("This instrument has unsaved changes. Save or discard before closing.");
        }
        const auto index =
            static_cast<std::size_t>(std::distance(documentOrder_.begin(), position));
        documentOrder_.erase(position);
        if (active) {
            // Do not put the just-closed tab back into the inactive cache.
            document_ = {};
            compiled_ = {};
            const auto next = documentOrder_.empty()
                                  ? juce::String()
                                  : documentOrder_[std::min(index, documentOrder_.size() - 1)];
            activateLocked(next);
        } else {
            inactiveDocuments_.erase(id);
            updateCheckpointLocked();
        }
    } else if (method == "document.apply") {
        requireRevision();
        applyLocked(str(params, "text"), str(params, "gestureId"), str(params, "gesturePhase"));
    } else if (method == "document.save") {
        if (requestedDocument.isEmpty() || requestedDocument == document_.path)
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
        const auto pitch = static_cast<double>(get(params, "pitch"));
        const auto velocity =
            method == "keyboard.noteOn" ? static_cast<double>(get(params, "velocity")) : 0;
        auto id = static_cast<juce::int64>(get(params, "noteId"));
        if (id < 0 || !std::isfinite(pitch) || pitch < 0 || pitch > 127 ||
            !std::isfinite(velocity) || velocity < 0 || velocity > 1)
            fail("Invalid keyboard event");
        // A key pressed while this source is pending/invalid is not queued for
        // a later program. Releases remain safe for already-held identities.
        if (method == "keyboard.noteOn" &&
            (compiled_.status != "ready" || compiled_.revision != document_.revision)) {
            lock.unlock();
            return snapshot();
        }
        if (!keyboard_.push(
                {{0, method == "keyboard.noteOn" ? EventType::note_on : EventType::note_off,
                  uiNoteMask | static_cast<std::uint64_t>(id), pitch, velocity},
                 audioDocumentRevision_.load()})) {
            panic_.store(true);
            fail("Keyboard event queue overflow; voices were reset");
        }
    } else if (method == "keyboard.panic") {
        panic_.store(true);
        sequencePanic_.store(true);
        sequenceSession_.requestPanic();
        demo_.store(false);
        auditionSignal_.clear();
        sequenceAuditionMuted_.store(false);
        ++selectionVersion_;
    } else if (method == "audio.playDemo") {
        const bool play = !demo_.load();
        demo_.store(play);
        if (!play && sequenceSound_.pinned()) {
            sequencePanic_.store(true);
            sequenceSession_.requestPanic();
        }
    } else if (method == "sequencer.set" || method == "sequencer.sound") {
        if (static_cast<juce::int64>(get(params, "baseRevision")) !=
            static_cast<juce::int64>(sequenceRevision_))
            fail("The sequencer changed. Refresh before applying this edit.");
        auto pattern = method == "sequencer.set" ? parseSequence(get(params, "pattern"))
                                                 : displayedSequenceLocked();
        const auto descriptor =
            method == "sequencer.set" ? get(get(params, "pattern"), "sound") : get(params, "sound");
        const bool sameDescriptor =
            descriptor.isObject() &&
            juce::JSON::toString(descriptor) ==
                juce::JSON::toString(
                    get(sequenceStateJSON(pattern, sequenceSound_, false), "sound"));
        SequenceSound sound;
        if (descriptor.isVoid() || (sameDescriptor && method == "sequencer.set"))
            sound = sequenceSound_;
        else if (str(descriptor, "kind") == "lanes" && sequenceSound_.kind == "lanes") {
            const auto* lanes = get(descriptor, "lanes").getArray();
            if (!lanes || lanes->size() != static_cast<int>(pattern.rowCount))
                fail("Every sequencer lane needs one sound");
            sound.kind = "lanes";
            for (const auto& lane : *lanes) {
                const auto same = std::find_if(
                    sequenceSound_.lanes.begin(), sequenceSound_.lanes.end(), [&](const auto& old) {
                        return old.kind == str(lane, "kind") && old.path == str(lane, "path") &&
                               (old.kind != "sample" ||
                                old.playback == (str(lane, "playback").isEmpty()
                                                     ? juce::String("one_shot")
                                                     : str(lane, "playback")));
                    });
                if (same != sequenceSound_.lanes.end())
                    sound.lanes.push_back(*same);
                else {
                    if (str(lane, "kind") == "lanes") fail("Lane sounds cannot be nested");
                    sound.lanes.push_back(parseSequenceSound(lane, true));
                }
            }
        } else
            sound = parseSequenceSound(descriptor, true);
        if (sound.kind == "lanes" && sound.lanes.size() != pattern.rowCount)
            fail("Every sequencer lane needs one sound");
        auto owner = sequenceSound_.pinned() ? sequenceOwner_ : document_.path;
        if (!sound.pinned()) owner = document_.path;
        if (owner.isEmpty()) fail("Open an instrument before editing a sequencer pattern");
        bool needsSave = document_.path == owner ? document_.needsSave : false;
        if (const auto found = inactiveDocuments_.find(owner); found != inactiveDocuments_.end())
            needsSave = found->second.document.needsSave;
        // Commit to the owning document only after complete validation/persistence.
        if (!needsSave) saveSequenceLocked(owner, pattern, sound);
        selectSequenceSoundLocked(std::move(sound), pattern, owner);
        ++sequenceRevision_;
        updateCheckpointLocked();
    } else if (method == "sequencer.play") {
        const auto value = get(params, "playing");
        if (!value.isBool()) fail("Sequencer playing must be true or false");
        const bool play = static_cast<bool>(value);
        demo_.store(play);
        if (!play && sequenceSound_.pinned()) {
            sequencePanic_.store(true);
            sequenceSession_.requestPanic();
        }
    } else if (method == "probe.listen") {
        const auto name = str(params, "signal").toStdString();
        if (static_cast<bool>(get(params, "enabled"))) {
            if (!compiled_.program || compiled_.status != "ready" ||
                auditionValue(*compiled_.program, name) == invalid_value)
                fail("Select a compiled audio signal or direct audio probe to audition");
            auditionSignal_ = name;
        } else
            auditionSignal_.clear();
        sequenceAuditionMuted_.store(!auditionSignal_.empty());
        ++selectionVersion_;
    } else if (method == "probe.set") {
        std::vector<std::string> selected;
        auto names = get(params, "signals");
        const auto* list = names.getArray();
        if (!list || list->size() > 128) fail("Observe at most 128 signal views");
        for (const auto& value : *list) {
            if (!value.isString()) fail("Signal names must be strings");
            const auto name = value.toString().toStdString();
            if (std::find(selected.begin(), selected.end(), name) == selected.end())
                selected.push_back(name);
        }
        auto voice = get(params, "voice");
        if ((voice.isString() && voice.toString() != "mixed") ||
            (!voice.isString() && !voice.isInt() && !voice.isInt64()))
            fail("Voice slot must be 0..31 or mixed");
        const auto requestedSlot =
            voice.isString() ? juce::int64{-1} : static_cast<juce::int64>(voice);
        if (requestedSlot < -1 || requestedSlot > 31) fail("Voice slot must be 0..31 or mixed");
        const int slot = static_cast<int>(requestedSlot);
        if (!compiled_.program || compiled_.status != "ready")
            fail("Compile the active instrument before selecting probes");
        if (compiled_.program->voice_policy.mode == VoiceMode::mono && slot > 0)
            fail("Mono instruments have only voice slot 0");
        const auto projection = params.hasProperty("projection")
                                    ? parseProjection(get(params, "projection"))
                                    : projection_;
        const auto selection =
            selectionForNames(*compiled_.program, compiled_.revision, selected, slot, projection);
        if (!selection) fail(selection.error);
        if (selected != selectedSignals_ || slot != selectedVoice_ || projection != projection_) {
            selectedSignals_ = std::move(selected);
            selectedVoice_ = slot;
            projection_ = projection;
            auditionSignal_.clear();
            sequenceAuditionMuted_.store(false);
            ++selectionVersion_;
        }
    } else if (method == "parameter.export") {
        int slot = static_cast<int>(get(params, "slot")) - 1;
        if (slot < 0 || slot >= 32) fail("Macro slot must be 1..32");
        auto id = str(params, "bindingId");
        const ParameterDescriptor* match = nullptr;
        if (compiled_.program)
            for (const auto& p : compiled_.program->parameters)
                if (p.binding_id == id.toStdString() && p.live) match = &p;
        if (!match) fail("This source binding cannot be exported as a live parameter");
        exports_[slot] = {id, match->name, true, bindingStructure(*compiled_.program)};
        const auto value = compiled_.program->nodes[match->value].initial_value;
        double normalized = match->logarithmic && match->min > 0
                                ? std::log(std::max(match->min, value) / match->min) /
                                      std::log(match->max / match->min)
                                : (value - match->min) / (match->max - match->min);
        hostParams_[slot]->setValueNotifyingHost(
            static_cast<float>(std::clamp(normalized, 0.0, 1.0)));
        updateMacrosLocked();
        updateCheckpointLocked();
    } else if (method == "parameter.unexport") {
        int slot = static_cast<int>(get(params, "slot")) - 1;
        if (slot < 0 || slot >= 32) fail("Macro slot must be 1..32");
        exports_[slot] = {};
        updateMacrosLocked();
        updateCheckpointLocked();
    } else if (method == "transport.set") {
        const auto value = get(params, "bpm");
        const double bpm = static_cast<double>(value);
        if (!(value.isInt() || value.isInt64() || value.isDouble()) || !std::isfinite(bpm) ||
            bpm < 20 || bpm > 300)
            fail("Tempo must be between 20 and 300 BPM");
        preferences_ = preferencesWithPatch(preferences_, object({{"bpm", bpm}}));
        bpm_.store(bpm);
        settingsFile_.replaceWithText(
            juce::JSON::toString(object({{"preferences", preferences_}})));
        updateCheckpointLocked();
    } else if (method == "preferences.set") {
        preferences_ = preferencesWithPatch(preferences_, params);
        bpm_.store(static_cast<double>(get(preferences_, "bpm")));
        settingsFile_.replaceWithText(
            juce::JSON::toString(object({{"preferences", preferences_}})));
        updateCheckpointLocked();
    } else if (method == "ai.configure" || method == "ai.models.save" ||
               method == "ai.models.select" || method == "ai.models.remove") {
        if (aiBusy_.load()) fail("Wait for the current reply before changing models.");
        if (method == "ai.models.select")
            aiModels_.select(str(params, "id"));
        else if (method == "ai.models.remove")
            aiModels_.remove(str(params, "id"));
        else
            aiModels_.save(params);
        refreshAI(true);
    } else if (method == "ai.cancel") {
        const auto id = str(params, "requestId");
        if (id.isNotEmpty() && id != runningRequestId_)
            fail("This AI request is no longer running. Refresh the conversation.");
        if (aiBusy_.load()) {
            aiGeneration_.fetch_add(1);
            aiWorker_.request_stop();
        }
    } else if (method == "ai.apply" || method == "ai.create") {
        auto it = proposals_.find(str(params, "requestId"));
        if (it == proposals_.end() || !proposalCurrentLocked(it->second) ||
            it->second.text != str(params, "text"))
            fail("This proposal is stale or was not returned by the current request");
        if (!it->second.workspace) requireRevision();
        if (method == "ai.apply" &&
            (it->second.documentId.isEmpty() || it->second.documentId != document_.path))
            fail("Open the target instrument to review and apply this edit.");
        if (!assistantHistory_.proposalAvailable(it->second.conversationId,
                                                 str(params, "requestId"), it->second.text))
            fail("Restore this conversation and refresh the proposal before applying it.");
        if (method == "ai.create") {
            if (documentOrder_.size() >= maximumOpenDocuments)
                fail("Close an instrument before opening another");
            auto file = workspaceFolder(workspace_, "AI-gen")
                            .getChildFile("AI instrument.flows")
                            .getNonexistentSibling();
            if (!file.replaceWithText(it->second.text, false, false, "\n"))
                fail("Could not save the generated instrument");
            proposals_.erase(it);
            files_ = workspaceFiles(workspace_);
            lock.unlock();
            return request("document.open",
                           object({{"path", file.getRelativePathFrom(workspace_)}}));
        }
        applyLocked(it->second.text);
        proposals_.erase(it);
    } else if (method == "project.importText") {
        if (document_.dirty()) {
            auto recovery = workspace_.getChildFile(".recovery");
            recovery.createDirectory();
            const auto prefix =
                juce::JSON::toString(packageLocked(false), true).dropLastCharacters(1) +
                ",\"assets\":";
            writeAssetState(
                recovery.getChildFile(juce::File(document_.path).getFileName() + ".flowsinst"),
                prefix, *sampleAssets_.serializedBundle(packageSourcesLocked()));
        }
        loadPackageLocked(parseJSONBounded(str(params, "text"), maximumInstrumentPackageBytes),
                          true);
    } else if (method == "project.exportText")
        return juce::JSON::toString(packageLocked());
    else
        fail("Unknown native method: " + method);
    lock.unlock();
    return snapshot();
}

void FlowsProcessor::worker(std::stop_token stop) {
    juce::String preparedUnitKey;
    Analyzer analyzer;
    std::uint64_t analyzerRevision = 0, lastSelection = ~std::uint64_t{0}, telemetrySequence = 0;
    std::optional<Program> analysisProgram;
    std::string selectionError;
    std::vector<std::string> analyzerSignalNames;
    auto nextTelemetry = std::chrono::steady_clock::now();
    auto nextCheckpoint = nextTelemetry;
    juce::String lastRecoveryPrefix;
    std::shared_ptr<const juce::MemoryBlock> lastRecoveryAssets;
    while (!stop.stop_requested()) {
        if (auto data =
                std::atomic_exchange(&restore_, std::shared_ptr<const juce::MemoryBlock>{})) {
            try {
                juce::String source;
                juce::MemoryInputStream stream(*data, false);
                std::array<float, 32> values{};
                bool overlay = false;
                if (data->getSize() >= 136 && stream.readInt() == 0x464c5731) {
                    const int size = stream.readInt();
                    if (size < 0 || static_cast<std::size_t>(size) != data->getSize() - 136)
                        fail("Invalid state checkpoint size");
                    for (auto& value : values) {
                        value = stream.readFloat();
                        if (!std::isfinite(value)) fail("Invalid saved macro value");
                    }
                    source = juce::String::fromUTF8(static_cast<const char*>(data->getData()) + 136,
                                                    size);
                    overlay = true;
                } else
                    source = juce::String::fromUTF8(static_cast<const char*>(data->getData()),
                                                    static_cast<int>(data->getSize()));
                std::lock_guard lock(mutex_);
                loadPackageLocked(parseJSONBounded(source, maximumInstrumentPackageBytes));
                if (overlay)
                    for (std::size_t i = 0; i < 32; ++i)
                        hostParams_[i]->setValueNotifyingHost(std::clamp(values[i], 0.0f, 1.0f));
            } catch (const std::exception& e) {
                std::lock_guard lock(mutex_);
                agentEvents_.push_back(
                    object({{"status", "failed"},
                            {"error", juce::String("Restore failed: ") + e.what()}}));
            }
        }
        juce::String text, path;
        std::uint64_t revision, documentGeneration;
        double sampleRate = rate_.load();
        bool needsCompile;
        {
            std::lock_guard lock(mutex_);
            revision = requestedRevision_;
            text = document_.text;
            path = document_.path;
            documentGeneration = audioDocumentRevision_.load();
            needsCompile =
                path.isNotEmpty() && (revision != preparedRevision_ || sampleRate != preparedRate_);
        }
        if (needsCompile) {
            auto source = text.toStdString();
            auto result = compile(source, path.toStdString());
            std::unique_ptr<PreparedProgram> prepared;
            auto* program = result.program ? &*result.program : nullptr;
            if (program) {
                try {
                    // Bound all simultaneously prepared/retired programs, not just
                    // active voice state. Five such programs fit a 128 MiB session budget.
                    if (program->nodes.size() > 2048 || program->states.size() > 256 ||
                        program->completion.size() > 4096 || program->analysis.size() > 2048)
                        throw std::runtime_error(
                            "Native instrument exceeds its graph budget (2048 nodes / 256 states / "
                            "4096 completion nodes / 2048 analysis nodes)");
                    auto samples = sampleAssets_.bank(*program);
                    prepared = std::make_unique<PreparedProgram>(*program, source, revision,
                                                                 sampleRate, std::move(samples));
                    prepared->documentGeneration = documentGeneration;
                } catch (const std::exception& error) {
                    SourceSpan span{};
                    for (const auto& resource : program->samples)
                        if (std::string(error.what()).find(resource.path) != std::string::npos) {
                            span = resource.span;
                            break;
                        }
                    result.diagnostics.push_back(
                        {span, std::string("Cannot prepare audio: ") + error.what()});
                    result.program.reset();
                }
            }
            auto graph = graphMetadata({}, {});
            if (result.program) {
                try {
                    graph = graphMetadata(makeDataflowGraph(*result.program), source);
                } catch (const std::exception& error) {
                    graph.getDynamicObject()->setProperty("error", error.what());
                }
            }
            std::lock_guard lock(mutex_);
            if (revision == requestedRevision_ && path == document_.path &&
                text == document_.text && sampleRate == rate_.load()) {
                preparedRevision_ = revision;
                preparedRate_ = sampleRate;
                compiled_.revision = revision;
                compiled_.status = result.program ? "ready" : "error";
                compiled_.graph = std::move(graph);
                compiled_.diagnostics.clear();
                compiled_.parameters.clear();
                compiled_.signals.clear();
                compiled_.choices.clear();
                if (!prepared) compiled_.program.reset();
                if (!prepared) {
                    sourcePlayable_.store(false, std::memory_order_release);
                    audioDocumentRevision_.store(revision, std::memory_order_release);
                    panic_.store(true);
                    session_.requestPanic();
                    analysisProgram.reset();
                    analyzerSignalNames.clear();
                    auditionSignal_.clear();
                    sequenceAuditionMuted_.store(false);
                    ++selectionVersion_;
                }
                // Follow the open source, including invalid drafts. The
                // independent bank's revision gate silences a failed unit.
                if (sequenceSound_.references(path)) {
                    auto sound = sequenceSound_;
                    const auto refresh = [&](const auto& self, SequenceSound& unit) -> void {
                        if (unit.kind == "instrument" && unit.path == path) unit.source = text;
                        for (auto& lane : unit.lanes) self(self, lane);
                    };
                    refresh(refresh, sound);
                    selectSequenceSoundLocked(std::move(sound), pinnedSequencePattern_,
                                              sequenceOwner_);
                    ++sequenceRevision_;
                }
                if (sequenceSound_.kind == "lanes" &&
                    std::any_of(sequenceSound_.lanes.begin(), sequenceSound_.lanes.end(),
                                [](const auto& lane) { return lane.kind == "active"; })) {
                    ++sequenceUnitRevision_;
                    sequenceSoundStatus_ = "pending";
                }
                if (!prepared &&
                    (sequenceSound_.references(path) ||
                     (sequenceSound_.kind == "lanes" &&
                      std::any_of(sequenceSound_.lanes.begin(), sequenceSound_.lanes.end(),
                                  [](const auto& lane) { return lane.kind == "active"; })))) {
                    sequenceSourcePlayable_.store(false, std::memory_order_release);
                    sequencePanic_.store(true);
                    sequenceSession_.requestPanic();
                }
                for (const auto& d : result.diagnostics)
                    compiled_.diagnostics.add(
                        object({{"message", d.message},
                                {"severity", "error"},
                                {"from", utf16Offset(source, d.span.offset)},
                                {"to", utf16Offset(source, d.span.offset + d.span.length)}}));
                if (result.program) {
                    compiled_.program = *result.program;
                    playableSource_ = text;
                    for (const auto& value : result.program->named_values) {
                        const bool audio = audioNode(*result.program, value.value);
                        juce::Array<juce::var> roots;
                        roots.add(static_cast<int>(value.value));
                        compiled_.signals.add(object(
                            {{"name", value.name},
                             {"kind", audio ? "audio" : "control"},
                             {"shape", "waveform"},
                             {"unit", unit_name(result.program->nodes[value.value].unit)},
                             {"channels",
                              static_cast<int>(result.program->nodes[value.value].channels)},
                             {"captureRoots", roots},
                             {"auditionable", audio},
                             {"from", utf16Offset(source, value.span.offset)},
                             {"to", utf16Offset(source, value.span.offset + value.span.length)}}));
                    }
                    for (const auto& value : result.program->named_analysis) {
                        const auto& node = result.program->analysis[value.value];
                        juce::Array<juce::var> roots;
                        int channels = 1;
                        for (auto root : captureRoots(*result.program, value.value)) {
                            roots.add(static_cast<int>(root));
                            channels = std::max(
                                channels, static_cast<int>(result.program->nodes[root].channels));
                        }
                        compiled_.signals.add(object(
                            {{"name", value.name},
                             {"kind", "analysis"},
                             {"shape", analysisShape(node.shape)},
                             {"unit", unit_name(node.unit)},
                             {"channels", channels},
                             {"captureRoots", roots},
                             {"auditionable",
                              auditionValue(*result.program, value.name) != invalid_value},
                             {"from", utf16Offset(source, value.span.offset)},
                             {"to", utf16Offset(source, value.span.offset + value.span.length)}}));
                    }
                    for (const auto& p : result.program->parameters) {
                        const auto scale = sourceScale(source.substr(p.span.offset, p.span.length));
                        compiled_.parameters.add(
                            object({{"id", p.binding_id},
                                    {"name", p.name},
                                    {"from", utf16Offset(source, p.span.offset)},
                                    {"to", utf16Offset(source, p.span.offset + p.span.length)},
                                    {"value", result.program->nodes[p.value].initial_value / scale},
                                    {"unit", unit_name(p.unit)},
                                    {"min", p.min / scale},
                                    {"max", p.max / scale},
                                    {"step", p.step / scale},
                                    {"discrete", p.discrete},
                                    {"hasRange", p.has_range},
                                    {"scale", p.logarithmic ? "log" : "linear"},
                                    {"update", p.live ? "live" : "rebuild"},
                                    {"response", p.control_scale},
                                    {"responseOffset", p.control_offset / scale},
                                    {"hasDefault", p.has_default},
                                    {"defaultValue", p.default_value / scale}}));
                    }
                    for (const auto& choice : result.program->choices) {
                        Array<var> options;
                        for (const auto& option : choice.options)
                            options.add(object({{"value", option.value},
                                                {"enabled", option.enabled},
                                                {"reason", option.reason}}));
                        compiled_.choices.add(object(
                            {{"id", choice.binding_id},
                             {"name", choice.name},
                             {"value", choice.selected},
                             {"from", utf16Offset(source, choice.span.offset)},
                             {"to", utf16Offset(source, choice.span.offset + choice.span.length)},
                             {"implicit", choice.implicit},
                             {"prefix", choice.prefix},
                             {"suffix", choice.suffix},
                             {"options", options}}));
                    }
                    updateMacrosLocked();
                    updateCheckpointLocked();
                }
                if (prepared) {
                    for (std::size_t i = 0; i < exports_.size(); ++i)
                        if (exports_[i].bound && exports_[i].binding.isNotEmpty()) {
                            for (const auto& p : prepared->program.parameters)
                                if (p.live && p.binding_id == exports_[i].binding.toStdString()) {
                                    const auto normal = static_cast<double>(hostParams_[i]->get());
                                    const auto value = p.logarithmic && p.min > 0
                                                           ? p.min * std::pow(p.max / p.min, normal)
                                                           : p.min + normal * (p.max - p.min);
                                    prepared->initialParameters.push_back(
                                        {0, p.value, value, 0, p.logarithmic});
                                }
                        }
                    analysisProgram = prepared->program;
                    analyzerRevision = revision;
                    if (analysisProgram->voice_policy.mode == VoiceMode::mono && selectedVoice_ > 0)
                        selectedVoice_ = 0;
                    // Source edits can remove views or expand their dependencies. Keep
                    // the surviving selection within the same bounded capture budget.
                    std::vector<std::string> retained;
                    for (const auto& name : selectedSignals_) {
                        auto candidate = retained;
                        candidate.push_back(name);
                        if (selectionForNames(*analysisProgram, revision, candidate, selectedVoice_,
                                              projection_))
                            retained = std::move(candidate);
                    }
                    selectedSignals_ = std::move(retained);
                    auto selection = selectionForNames(*analysisProgram, revision, selectedSignals_,
                                                       selectedVoice_, projection_);
                    analyzer.configure(*analysisProgram, revision, selection.analysisNames, false);
                    selectionError.clear();
                    analyzerSignalNames = selectedSignals_;
                    if (!auditionSignal_.empty()) {
                        selection.selection.audition =
                            auditionValue(*analysisProgram, auditionSignal_);
                        if (selection.selection.audition == invalid_value) auditionSignal_.clear();
                    }
                    prepared->probes = selection.selection;
                    session_.submit(std::move(prepared));
                    sourcePlayable_.store(true, std::memory_order_release);
                    lastSelection = ~std::uint64_t{0};
                }
            }
        }
        // Each lane gets independent DSP state, including mono kernels and samples.
        SequenceSound unitSound;
        std::uint64_t unitRevision = 0, unitGeneration = 0, sourceRevision = 0;
        bool prepareUnit = false;
        juce::String unitKey;
        {
            std::lock_guard lock(mutex_);
            unitRevision = sequenceUnitRevision_;
            unitGeneration = sequenceUnitGeneration_;
            unitSound = sequenceSound_;
            sourceRevision = document_.revision;
            for (auto& lane : unitSound.lanes)
                if (lane.kind == "active") {
                    lane.source = document_.text;
                    lane.path = document_.path;
                }
            unitKey = juce::JSON::toString(
                sequenceStateJSON(displayedSequenceLocked(), unitSound).getProperty("sound", {}));
            for (const auto& lane : unitSound.lanes)
                if (lane.kind == "active")
                    unitKey += juce::JSON::toString(juce::var(lane.source)) +
                               juce::JSON::toString(juce::var(lane.path));
            prepareUnit = sequenceSound_.pinned() &&
                          (unitRevision != preparedSequenceUnitRevision_ ||
                           sampleRate != preparedSequenceRate_ || unitKey != preparedUnitKey);
            sequenceAuditionMuted_.store(!auditionSignal_.empty());
        }
        if (prepareUnit) {
            std::vector<std::unique_ptr<PreparedProgram>> prepared;
            juce::String unitError;
            try {
                auto units = unitSound.lanes;
                if (unitSound.kind != "lanes") units = {unitSound};
                for (const auto& unit : units) {
                    auto result = compile(unit.source.toStdString(), unit.path.toStdString());
                    if (!result.program)
                        fail(result.diagnostics.empty()
                                 ? juce::String("Lane sound did not compile")
                                 : juce::String(result.diagnostics.front().message));
                    const auto& program = *result.program;
                    if (program.nodes.size() > 2048 || program.states.size() > 256 ||
                        program.completion.size() > 4096 || program.analysis.size() > 2048)
                        fail("Lane sound exceeds the native graph budget");
                    auto p = std::make_unique<PreparedProgram>(program, unit.source.toStdString(),
                                                               unitRevision, sampleRate,
                                                               sampleAssets_.bank(program));
                    p->documentGeneration = unitGeneration;
                    prepared.push_back(std::move(p));
                }
            } catch (const std::exception& exception) {
                unitError = exception.what();
                prepared.clear();
            }
            std::lock_guard lock(mutex_);
            if (sequenceSound_.pinned() && unitRevision == sequenceUnitRevision_ &&
                sampleRate == rate_.load() &&
                std::all_of(unitSound.lanes.begin(), unitSound.lanes.end(), [&](const auto& lane) {
                    return lane.kind != "active" ||
                           (lane.path == document_.path && sourceRevision == document_.revision);
                })) {
                preparedSequenceUnitRevision_ = unitRevision;
                preparedSequenceRate_ = sampleRate;
                preparedUnitKey = unitKey;
                sequenceSoundStatus_ = prepared.empty() ? "error" : "ready";
                sequenceSoundError_ = unitError;
                if (!prepared.empty()) {
                    sequenceSession_.submit(std::move(prepared));
                    sequenceTargetRevision_.store(unitRevision, std::memory_order_release);
                    sequenceSourcePlayable_.store(true, std::memory_order_release);
                } else {
                    sequenceSourcePlayable_.store(false, std::memory_order_release);
                    sequencePanic_.store(true);
                    sequenceSession_.requestPanic();
                }
            }
        }
        if (analysisProgram) {
            std::lock_guard lock(mutex_);
            if (lastSelection != selectionVersion_ && analyzerRevision == compiled_.revision) {
                auto selection = selectionForNames(*analysisProgram, analyzerRevision,
                                                   selectedSignals_, selectedVoice_, projection_);
                if (selection && !auditionSignal_.empty()) {
                    selection.selection.audition = auditionValue(*analysisProgram, auditionSignal_);
                    if (selection.selection.audition == invalid_value) auditionSignal_.clear();
                }
                if (selection && session_.setProbes(selection.selection)) {
                    analyzer.configure(*analysisProgram, analyzerRevision, selection.analysisNames,
                                       false);
                    selectionError.clear();
                    analyzerSignalNames = selectedSignals_;
                    lastSelection = selectionVersion_;
                } else if (!selection) {
                    selectionError = selection.error;
                    lastSelection = selectionVersion_;
                }
            }
        }
        while (auto* frame = session_.frames().readSlot()) {
            analyzer.consume(*frame);
            session_.frames().commitRead();
        }
        while (sequenceSession_.frames().readSlot()) sequenceSession_.frames().commitRead();
        const auto now = std::chrono::steady_clock::now();
        if (now >= nextCheckpoint) {
            auto files = workspaceFiles(workspace_);
            auto samples = sampleAssets_.catalogue();
            try {
                juce::var recovery;
                std::vector<juce::String> sources;
                {
                    std::lock_guard lock(mutex_);
                    files_ = std::move(files);
                    samples_ = std::move(samples);
                    updateCheckpointLocked();
                    if (standalone) {
                        recovery = recoveryLocked(false);
                        sources = recoverySourcesLocked();
                    }
                }
                if (standalone) {
                    // Large immutable asset segments are serialized once and
                    // written only when source/settings/assets actually change.
                    // Neither JSON serialization nor disk I/O holds the UI lock.
                    auto assets = sampleAssets_.serializedBundle(sources);
                    auto prefix =
                        juce::JSON::toString(recovery, true).dropLastCharacters(1) + ",\"assets\":";
                    if (prefix != lastRecoveryPrefix || assets != lastRecoveryAssets) {
                        writeAssetState(workspace_.getChildFile(".session.flowsinst"), prefix,
                                        *assets);
                        lastRecoveryPrefix = std::move(prefix);
                        lastRecoveryAssets = std::move(assets);
                    }
                }
            } catch (const std::exception& error) {
                std::lock_guard lock(mutex_);
                const auto message =
                    juce::String("Recovery assets could not be packed: ") + error.what();
                if (checkpointError_ != message) {
                    checkpointError_ = message;
                    agentEvents_.push_back(object({{"status", "failed"}, {"error", message}}));
                }
            }
            nextCheckpoint = now + std::chrono::milliseconds(500);
        }
        if (now >= nextTelemetry) {
            auto a = analyzer.snapshot();
            juce::Array<juce::var> signals;
            auto add = [&](const AnalysisView& v) {
                const auto kind = v.kind == "analysis" ? "analysis"
                                  : (v.kind == "mixed_audio" ||
                                     (analysisProgram &&
                                      [&] {
                                          for (const auto& n : analysisProgram->named_values)
                                              if (n.name == v.name)
                                                  return audioNode(*analysisProgram, n.value);
                                          return false;
                                      }()))
                                      ? "audio"
                                      : "control";
                auto values = v.values;
                if (v.shape == "scalar" && v.valid) values = {static_cast<float>(v.scalar)};
                signals.add(object({{"name", v.name},
                                    {"kind", kind},
                                    {"captureKind", v.kind},
                                    {"shape", v.shape},
                                    {"unit", v.unit},
                                    {"values", array(values)},
                                    {"signedSpectrum", v.signedSpectrum},
                                    {"detail", array(v.detail)},
                                    {"detailSeconds", v.detailSeconds},
                                    {"beatAvailable", v.beatAvailable},
                                    {"beatPhase", v.beatPhase},
                                    {"beatMin", array(v.beatMin)},
                                    {"beatMax", array(v.beatMax)},
                                    {"beatAge", array(v.beatAge)},
                                    {"sampleCount", static_cast<juce::int64>(v.sampleCount)},
                                    {"windowReady", v.windowReady},
                                    {"rms", v.rms},
                                    {"peak", v.peak},
                                    {"scalar", v.scalar},
                                    {"valid", v.valid},
                                    {"error", v.error},
                                    {"spectrum", v.shape == "spectrum" || v.shape == "mel_spectrum"
                                                     ? array(values)
                                                     : array(v.spectrum)},
                                    {"frequencies", array(v.frequencies)},
                                    {"spectrumFrequencies", array(v.spectrumFrequencies)}}));
            };
            for (const auto& v : a.signals) add(v);
            for (const auto& v : a.results) add(v);
            // Capture uses ValueId identity. A DSP assignment alias (copy = raw)
            // shares that channel but still needs telemetry under its authored
            // name. Materialize only subscribed aliases, not every alias in code.
            if (analysisProgram)
                for (const auto& name : analyzerSignalNames) {
                    const auto& named = analysisProgram->named_values;
                    const auto alias =
                        std::find_if(named.begin(), named.end(),
                                     [&](const auto& value) { return value.name == name; });
                    if (alias == named.end()) continue;
                    const auto canonical = std::find_if(
                        named.begin(), named.end(),
                        [&](const auto& value) { return value.value == alias->value; });
                    if (canonical == alias) continue;
                    const auto captured = std::find_if(
                        a.signals.begin(), a.signals.end(),
                        [&](const auto& view) { return view.name == canonical->name; });
                    if (captured != a.signals.end()) {
                        auto view = *captured;
                        view.name = name;
                        add(view);
                    }
                }
            std::lock_guard lock(mutex_);
            const bool selectedObservation = sourcePlayable_.load() &&
                                             document_.path.isNotEmpty() &&
                                             a.revision >= audioDocumentRevision_.load();
            if (!selectedObservation || !selectionError.empty()) signals.clear();
            latestTelemetry_ = object(
                {{"captureIdle", a.captureIdle},
                 {"captureHeld", a.captureHeld},
                 {"captureMode", a.continuous ? "transport" : "performance"},
                 {"keyEpoch", static_cast<juce::int64>(a.keyEpoch)},
                 {"sequence", static_cast<juce::int64>(++telemetrySequence)},
                 {"programRevision",
                  static_cast<juce::int64>(selectedObservation ? a.revision : 0)},
                 {"sampleRate", sampleRate},
                 {"signals", signals},
                 {"sequencerDocumentId", sequenceSound_.pinned() ? sequenceOwner_ : document_.path},
                 {"sequencerStep", demo_.load() ? sequencePlayhead_.load() : -1},
                 {"sequencerPlaying", demo_.load()},
                 {"captureVoice", a.voice},
                 {"captureProjection", projectionName(a.projection)},
                 {"error", selectionError},
                 {"outputPeak", mixedPeak_.load()},
                 {"voices",
                  sequenceSound_.pinned()
                      ? static_cast<int>(mixedVoices_.load())
                      : (selectedObservation ? static_cast<int>(session_.status.voices.load())
                                             : 0)},
                 {"cpu", session_.status.cpu.load()},
                 {"gap", !selectedObservation || !a.hasCapture || !selectionError.empty()},
                 {"analysisReady", selectedObservation && a.ready && selectionError.empty()},
                 {"status", !selectedObservation      ? "waiting for selected instrument"
                            : !selectionError.empty() ? selectionError
                                                      : a.status},
                 {"generation", static_cast<juce::int64>(a.generation)},
                 {"parameterEpoch", static_cast<juce::int64>(a.parameterEpoch)},
                 {"observationEpoch", static_cast<juce::int64>(a.observationEpoch)},
                 {"captureStart", static_cast<juce::int64>(a.start)},
                 {"captureEnd", static_cast<juce::int64>(a.end)},
                 {"captureResets", static_cast<juce::int64>(a.resets)},
                 {"captureDrops", static_cast<juce::int64>(session_.status.drops.load())},
                 {"processErrors", static_cast<juce::int64>(session_.status.errors.load() +
                                                            sequenceSession_.status.errors.load())},
                 {"hostOverflows", static_cast<juce::int64>(hostOverflows_.load())}});
            nextTelemetry = now + std::chrono::milliseconds(33);
        }
        session_.reap();
        sequenceSession_.reap();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    session_.reap();
    sequenceSession_.reap();
}
}  // namespace flows::app

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() {
    return new flows::app::FlowsProcessor;
}
