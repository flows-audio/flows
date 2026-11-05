#include "FlowsProcessor.hpp"

#include "FlowsEditor.hpp"

#include <algorithm>

namespace flows::app {
namespace {
juce::File applicationData() {
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("Flows");
}
juce::File environmentFolder(const char* name, const juce::File& fallback) {
    const auto value = juce::SystemStats::getEnvironmentVariable(name, {});
    return value.isNotEmpty() ? juce::File(value) : fallback;
}
}  // namespace

FlowsProcessor::FlowsProcessor(bool isStandalone, AISecrets secrets)
    : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      standalone(isStandalone) {
    for (int i = 0; i < 32; ++i) {
        auto suffix = juce::String(i + 1).paddedLeft('0', 2);
        hostParams_[static_cast<std::size_t>(i)] = new juce::AudioParameterFloat(
            juce::ParameterID("macro" + suffix, 1), "Macro " + suffix, juce::NormalisableRange<float>(0, 1), 0.5f);
        addParameter(hostParams_[static_cast<std::size_t>(i)]);
    }
    macroParameters_ = std::make_unique<JuceMacroParameters>(hostParams_);
    CoreOptions options;
    const auto workspace = environmentFolder("FLOWS_WORKSPACE", applicationData().getChildFile("Workspace"));
    options.workspace = workspace.getFullPathName().toStdString();
    options.workspaceIdentity = workspace.getFullPathName().toStdString();
    options.settingsDirectory = environmentFolder("FLOWS_AI_CONFIG_DIR", applicationData()).getFullPathName().toStdString();
    const bool liveBridge = juce::SystemStats::getEnvironmentVariable("FLOWS_DISABLE_LIVE_BRIDGE", {}) != "1";
    if (liveBridge)
        options.liveBridgeDirectory =
            environmentFolder("FLOWS_LIVE_SYNTH_DIR", applicationData().getChildFile("LiveBridge")).getFullPathName().toStdString();
    options.standalone = isStandalone;
    options.bundled = bundledResources();
    options.secrets = secrets.read ? std::move(secrets) : systemAISecrets();
    options.http = std::make_shared<JuceHttpClient>();
    options.decoder = std::make_shared<JuceAudioDecoder>();
    options.parameters = macroParameters_.get();
    options.capabilities = json::object({{"host", isStandalone ? "macos-standalone" : "macos-plugin"},
                                         {"audioSettings", isStandalone ? "native-dialog" : "plugin-host"},
                                         {"midiInput", isStandalone ? "native" : "plugin-host"},
                                         {"storage", "filesystem"},
                                         {"secrets", "keychain"},
                                         {"fileDialogs", "native"},
                                         {"clipboard", "native"},
                                         {"externalLinks", "native"},
                                         {"liveBridge", liveBridge},
                                         {"ai", true}});
    core_ = std::make_unique<AppCore>(std::move(options));
}
FlowsProcessor::~FlowsProcessor() { core_.reset(); }

bool FlowsProcessor::isBusesLayoutSupported(const BusesLayout& l) const {
    return l.getMainInputChannelSet().isDisabled() && (l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo() ||
                                                        l.getMainOutputChannelSet() == juce::AudioChannelSet::mono());
}
void FlowsProcessor::prepareToPlay(double rate, int block) { core_->audio().prepare(rate, block); }
void FlowsProcessor::releaseResources() { core_->audio().release(); }
void FlowsProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) {
    if (buffer.getNumChannels() == 0) {
        midi.clear();
        return;
    }
    std::size_t count = 0;
    // One past the runtime's 256-message budget is enough to report overflow.
    for (const auto metadata : midi) {
        if (count == midi_.size()) break;
        midi_[count++] = {metadata.data, metadata.numBytes, metadata.samplePosition};
    }
    HostPosition position;
    const HostPosition* hostPosition = nullptr;
    // AudioProcessorPlayer supplies a device-clock playhead in standalone,
    // with no musical transport fields. Its default isPlaying=false must not
    // stop the local pattern or transport LFO; only plugins follow host state.
    if (auto* playHead = standalone ? nullptr : getPlayHead())
        if (const auto info = playHead->getPosition()) {
            position.playing = info->getIsPlaying();
            if (const auto bpm = info->getBpm()) position.bpm = *bpm;
            if (const auto ppq = info->getPpqPosition()) position.ppq = *ppq;
            if (const auto sample = info->getTimeInSamples()) position.timeInSamples = *sample;
            hostPosition = &position;
        }
    AudioBlock block;
    block.left = buffer.getWritePointer(0);
    block.right = buffer.getNumChannels() > 1 ? buffer.getWritePointer(1) : nullptr;
    block.frames = buffer.getNumSamples();
    block.midi = std::span<const RawMidi>(midi_.data(), count);
    block.position = hostPosition;
    core_->audio().process(block);
    for (int channel = 2; channel < buffer.getNumChannels(); ++channel) buffer.clear(channel, 0, buffer.getNumSamples());
    midi.clear();
}
juce::AudioProcessorEditor* FlowsProcessor::createEditor() { return new FlowsEditor(*this); }

void FlowsProcessor::getStateInformation(juce::MemoryBlock& dest) {
    const auto state = core_->saveState();
    if (state.empty()) return;
    dest.replaceAll(state.data(), state.size());
}
void FlowsProcessor::setStateInformation(const void* data, int size) {
    if (size > 0 && static_cast<std::size_t>(size) <= maximumInstrumentPackageBytes + 136)
        core_->restoreState(std::string(static_cast<const char*>(data), static_cast<std::size_t>(size)));
}

json::Value FlowsProcessor::stamp(json::Value value) const {
    // Window transparency is decided by the editor after the window exists.
    if (auto* object = value.getObject())
        if (auto* capabilities = object->find("capabilities"); capabilities && capabilities->isObject())
            capabilities->set("windowTransparency", windowTransparency.load());
    return value;
}
json::Value FlowsProcessor::requestValue(const std::string& method, const json::Value& params) {
    if (method == "audio.settings") {
        if (!showAudioSettings) throw std::runtime_error("Select audio and MIDI devices in your plugin host.");
        showAudioSettings();
        return snapshotValue();
    }
    return stamp(core_->request(method, params));
}
juce::var FlowsProcessor::request(const juce::String& method, const juce::var& params) {
    return toJuce(requestValue(method.toStdString(), fromJuce(params)));
}
json::Value FlowsProcessor::snapshotValue() { return stamp(core_->snapshot()); }
juce::var FlowsProcessor::snapshot() { return toJuce(snapshotValue()); }
juce::var FlowsProcessor::companionRequest(const juce::String& method, const juce::var& params,
                                           const std::function<bool()>& cancelled) {
    return toJuce(core_->companionRequest(method.toStdString(), fromJuce(params), cancelled));
}
juce::var FlowsProcessor::importSample(const juce::File& file) {
    return toJuce(stamp(core_->importSample(file.getFullPathName().toStdString())));
}
juce::var FlowsProcessor::telemetry() { return toJuce(core_->telemetry()); }
std::vector<juce::var> FlowsProcessor::takeAgentEvents() {
    std::vector<juce::var> result;
    for (const auto& event : core_->takeAgentEvents()) result.push_back(toJuce(event));
    return result;
}
juce::String FlowsProcessor::importWildcard() const {
    juce::StringArray patterns;
    for (const auto& extension : juce::StringArray::fromTokens(juce::String(core_->importExtensions()), ";", {}))
        if (extension.isNotEmpty()) patterns.add("*." + extension);
    return patterns.joinIntoString(";");
}

void captureAUParameter(juce::AudioProcessor* p, int slot, unsigned int offset, float value) noexcept {
    static_cast<FlowsProcessor*>(p)->auParameter(slot, static_cast<int>(std::min(offset, 2147483647u)), 0, value, value, false);
}
void captureAURamp(juce::AudioProcessor* p, int slot, int offset, unsigned int duration, float start, float end) noexcept {
    static_cast<FlowsProcessor*>(p)->auParameter(slot, offset, duration, start, end, true);
}
void beginHostAutomation(juce::AudioProcessor* p) noexcept { static_cast<FlowsProcessor*>(p)->beginHostAutomation(); }
void captureHostParameter(juce::AudioProcessor* p, int slot, int offset, float value) noexcept {
    static_cast<FlowsProcessor*>(p)->hostParameter(slot, offset, value);
}
}  // namespace flows::app

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new flows::app::FlowsProcessor; }
