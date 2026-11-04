// Background worker: compiles and prepares the active instrument and sequencer
// sounds, feeds captured audio to the analyzer, publishes telemetry (~30 Hz)
// and writes recovery checkpoints (every 500 ms). Never runs on the audio thread.
#include "AppCore.hpp"

#include "CoreDetail.hpp"
#include "Files.hpp"
#include "SequencerJson.hpp"
#include "Text.hpp"
#include "WorkspaceLayout.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>

namespace flows::app {
using namespace detail;

void AppCore::worker(std::stop_token stop) {
    std::string preparedUnitKey;
    Analyzer analyzer;
    std::uint64_t analyzerRevision = 0, lastSelection = ~std::uint64_t{0}, telemetrySequence = 0;
    std::optional<Program> analysisProgram;
    std::string selectionError;
    std::vector<std::string> analyzerSignalNames;
    auto nextTelemetry = std::chrono::steady_clock::now();
    auto nextCheckpoint = nextTelemetry;
    std::string lastRecoveryPrefix;
    std::shared_ptr<const std::string> lastRecoveryAssets;
    auto& session = runtime_.session;
    auto& sequenceSession = runtime_.sequenceSession;
    while (!stop.stop_requested()) {
        std::shared_ptr<const std::string> data;
        {
            std::lock_guard lock(stateMutex_);
            data = std::exchange(restore_, nullptr);
        }
        if (data) {
            try {
                std::string source;
                std::array<float, 32> values{};
                bool overlay = false;
                const auto readInt = [&](std::size_t at) {
                    std::uint32_t value = 0;
                    for (int i = 0; i < 4; ++i)
                        value |= static_cast<std::uint32_t>(static_cast<unsigned char>((*data)[at + i])) << (8 * i);
                    return value;
                };
                if (data->size() >= 136 && readInt(0) == 0x464c5731) {
                    const auto size = static_cast<std::int32_t>(readInt(4));
                    if (size < 0 || static_cast<std::size_t>(size) != data->size() - 136) fail("Invalid state checkpoint size");
                    for (std::size_t i = 0; i < 32; ++i) {
                        values[i] = std::bit_cast<float>(readInt(8 + 4 * i));
                        if (!std::isfinite(values[i])) fail("Invalid saved macro value");
                    }
                    source = text::sanitizeUtf8(std::string_view(*data).substr(136, static_cast<std::size_t>(size)));
                    overlay = true;
                } else
                    source = text::sanitizeUtf8(*data);
                std::lock_guard lock(mutex_);
                loadPackageLocked(json::parseBounded(source, maximumInstrumentPackageBytes));
                if (overlay)
                    for (int i = 0; i < 32; ++i)
                        parameters_.set(i, std::clamp(values[static_cast<std::size_t>(i)], 0.0f, 1.0f));
            } catch (const std::exception& e) {
                std::lock_guard lock(mutex_);
                agentEvents_.push_back(
                    json::object({{"status", "failed"}, {"error", std::string("Restore failed: ") + e.what()}}));
            }
        }
        std::string sourceText, path;
        std::uint64_t revision = 0, documentGeneration = 0;
        const double sampleRate = runtime_.rate.load();
        bool needsCompile = false;
        {
            std::lock_guard lock(mutex_);
            revision = requestedRevision_;
            sourceText = document_.text;
            path = document_.path;
            documentGeneration = runtime_.audioDocumentRevision.load();
            needsCompile = !path.empty() && (revision != preparedRevision_ || sampleRate != preparedRate_);
        }
        if (needsCompile) {
            const auto& source = sourceText;
            auto result = compile(source, path);
            std::unique_ptr<PreparedProgram> prepared;
            auto* program = result.program ? &*result.program : nullptr;
            if (program) {
                try {
                    // Bound all simultaneously prepared/retired programs, not just
                    // active voice state. Five such programs fit a 128 MiB session budget.
                    if (program->nodes.size() > 2048 || program->states.size() > 256 ||
                        program->completion.size() > 4096 || program->analysis.size() > 2048)
                        throw std::runtime_error(
                            "Native instrument exceeds its graph budget (2048 nodes / 256 states / 4096 completion nodes / "
                            "2048 analysis nodes)");
                    auto samples = sampleAssets_.bank(*program);
                    prepared = std::make_unique<PreparedProgram>(*program, source, revision, sampleRate, std::move(samples));
                    prepared->documentGeneration = documentGeneration;
                } catch (const std::exception& error) {
                    SourceSpan span{};
                    for (const auto& resource : program->samples)
                        if (std::string(error.what()).find(resource.path) != std::string::npos) {
                            span = resource.span;
                            break;
                        }
                    result.diagnostics.push_back({span, std::string("Cannot prepare audio: ") + error.what()});
                    result.program.reset();
                }
            }
            auto graph = emptyGraph();
            if (result.program) {
                try {
                    graph = graphMetadata(makeDataflowGraph(*result.program), source);
                } catch (const std::exception& error) {
                    graph.set("error", error.what());
                }
            }
            std::lock_guard lock(mutex_);
            if (revision == requestedRevision_ && path == document_.path && sourceText == document_.text &&
                sampleRate == runtime_.rate.load()) {
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
                    runtime_.sourcePlayable.store(false, std::memory_order_release);
                    runtime_.audioDocumentRevision.store(revision, std::memory_order_release);
                    runtime_.panic.store(true);
                    session.requestPanic();
                    analysisProgram.reset();
                    analyzerSignalNames.clear();
                    auditionSignal_.clear();
                    runtime_.sequenceAuditionMuted.store(false);
                    ++selectionVersion_;
                }
                // Follow the open source, including invalid drafts. The
                // independent bank's revision gate silences a failed unit.
                if (sequenceSound_.references(path)) {
                    auto sound = sequenceSound_;
                    const auto refresh = [&](const auto& self, SequenceSound& unit) -> void {
                        if (unit.kind == "instrument" && unit.path == path) unit.source = sourceText;
                        for (auto& lane : unit.lanes) self(self, lane);
                    };
                    refresh(refresh, sound);
                    selectSequenceSoundLocked(std::move(sound), pinnedSequencePattern_, sequenceOwner_);
                    ++sequenceRevision_;
                }
                const auto activeLane = [&] {
                    return sequenceSound_.kind == "lanes" &&
                           std::any_of(sequenceSound_.lanes.begin(), sequenceSound_.lanes.end(),
                                       [](const auto& lane) { return lane.kind == "active"; });
                };
                if (activeLane()) {
                    ++sequenceUnitRevision_;
                    sequenceSoundStatus_ = "pending";
                }
                if (!prepared && (sequenceSound_.references(path) || activeLane())) {
                    runtime_.sequenceSourcePlayable.store(false, std::memory_order_release);
                    runtime_.sequencePanic.store(true);
                    sequenceSession.requestPanic();
                }
                for (const auto& d : result.diagnostics)
                    compiled_.diagnostics.push_back(
                        json::object({{"message", d.message},
                                      {"severity", "error"},
                                      {"from", text::utf16Offset(source, d.span.offset)},
                                      {"to", text::utf16Offset(source, d.span.offset + d.span.length)}}));
                if (result.program) {
                    compiled_.program = *result.program;
                    playableSource_ = sourceText;
                    const auto& compiledProgram = *result.program;
                    for (const auto& value : compiledProgram.named_values) {
                        const bool audio = audioNode(compiledProgram, value.value);
                        json::Array roots{json::Value(static_cast<int>(value.value))};
                        compiled_.signals.push_back(json::object(
                            {{"name", value.name},
                             {"kind", audio ? "audio" : "control"},
                             {"shape", "waveform"},
                             {"unit", unit_name(compiledProgram.nodes[value.value].unit)},
                             {"channels", static_cast<int>(compiledProgram.nodes[value.value].channels)},
                             {"captureRoots", std::move(roots)},
                             {"auditionable", audio},
                             {"from", text::utf16Offset(source, value.span.offset)},
                             {"to", text::utf16Offset(source, value.span.offset + value.span.length)}}));
                    }
                    for (const auto& value : compiledProgram.named_analysis) {
                        const auto& node = compiledProgram.analysis[value.value];
                        json::Array roots;
                        int channels = 1;
                        for (auto root : captureRoots(compiledProgram, value.value)) {
                            roots.emplace_back(static_cast<int>(root));
                            channels = std::max(channels, static_cast<int>(compiledProgram.nodes[root].channels));
                        }
                        compiled_.signals.push_back(json::object(
                            {{"name", value.name},
                             {"kind", "analysis"},
                             {"shape", analysisShape(node.shape)},
                             {"unit", unit_name(node.unit)},
                             {"channels", channels},
                             {"captureRoots", std::move(roots)},
                             {"auditionable", auditionValue(compiledProgram, value.name) != invalid_value},
                             {"from", text::utf16Offset(source, value.span.offset)},
                             {"to", text::utf16Offset(source, value.span.offset + value.span.length)}}));
                    }
                    for (const auto& p : compiledProgram.parameters) {
                        const auto scale = sourceScale(source.substr(p.span.offset, p.span.length));
                        compiled_.parameters.push_back(json::object(
                            {{"id", p.binding_id},
                             {"name", p.name},
                             {"from", text::utf16Offset(source, p.span.offset)},
                             {"to", text::utf16Offset(source, p.span.offset + p.span.length)},
                             {"value", compiledProgram.nodes[p.value].initial_value / scale},
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
                    for (const auto& choice : compiledProgram.choices) {
                        json::Array options;
                        for (const auto& option : choice.options)
                            options.push_back(json::object(
                                {{"value", option.value}, {"enabled", option.enabled}, {"reason", option.reason}}));
                        compiled_.choices.push_back(json::object(
                            {{"id", choice.binding_id},
                             {"name", choice.name},
                             {"value", choice.selected},
                             {"from", text::utf16Offset(source, choice.span.offset)},
                             {"to", text::utf16Offset(source, choice.span.offset + choice.span.length)},
                             {"implicit", choice.implicit},
                             {"prefix", choice.prefix},
                             {"suffix", choice.suffix},
                             {"options", std::move(options)}}));
                    }
                    updateMacrosLocked();
                    updateCheckpointLocked();
                }
                if (prepared) {
                    for (std::size_t i = 0; i < exports_.size(); ++i)
                        if (exports_[i].bound && !exports_[i].binding.empty()) {
                            for (const auto& p : prepared->program.parameters)
                                if (p.live && p.binding_id == exports_[i].binding) {
                                    const auto normal = static_cast<double>(parameters_.get(static_cast<int>(i)));
                                    const auto value = p.logarithmic && p.min > 0 ? p.min * std::pow(p.max / p.min, normal)
                                                                                  : p.min + normal * (p.max - p.min);
                                    prepared->initialParameters.push_back({0, p.value, value, 0, p.logarithmic});
                                }
                        }
                    analysisProgram = prepared->program;
                    analyzerRevision = revision;
                    if (analysisProgram->voice_policy.mode == VoiceMode::mono && selectedVoice_ > 0) selectedVoice_ = 0;
                    // Source edits can remove views or expand their dependencies. Keep
                    // the surviving selection within the same bounded capture budget.
                    std::vector<std::string> retained;
                    for (const auto& name : selectedSignals_) {
                        auto candidate = retained;
                        candidate.push_back(name);
                        if (selectionForNames(*analysisProgram, revision, candidate, selectedVoice_, projection_))
                            retained = std::move(candidate);
                    }
                    selectedSignals_ = std::move(retained);
                    auto selection = selectionForNames(*analysisProgram, revision, selectedSignals_, selectedVoice_, projection_);
                    analyzer.configure(*analysisProgram, revision, selection.analysisNames, false);
                    selectionError.clear();
                    analyzerSignalNames = selectedSignals_;
                    if (!auditionSignal_.empty()) {
                        selection.selection.audition = auditionValue(*analysisProgram, auditionSignal_);
                        if (selection.selection.audition == invalid_value) auditionSignal_.clear();
                    }
                    prepared->probes = selection.selection;
                    session.submit(std::move(prepared));
                    runtime_.sourcePlayable.store(true, std::memory_order_release);
                    lastSelection = ~std::uint64_t{0};
                }
            }
        }
        // Each lane gets independent DSP state, including mono kernels and samples.
        SequenceSound unitSound;
        std::uint64_t unitRevision = 0, unitGeneration = 0, sourceRevision = 0;
        bool prepareUnit = false;
        std::string unitKey;
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
            unitKey = json::stringify(sequenceStateJSON(displayedSequenceLocked(), unitSound)["sound"]);
            for (const auto& lane : unitSound.lanes)
                if (lane.kind == "active")
                    unitKey += json::stringify(json::Value(lane.source)) + json::stringify(json::Value(lane.path));
            prepareUnit = sequenceSound_.pinned() && (unitRevision != preparedSequenceUnitRevision_ ||
                                                      sampleRate != preparedSequenceRate_ || unitKey != preparedUnitKey);
            runtime_.sequenceAuditionMuted.store(!auditionSignal_.empty());
        }
        if (prepareUnit) {
            std::vector<std::unique_ptr<PreparedProgram>> prepared;
            std::string unitError;
            try {
                auto units = unitSound.lanes;
                if (unitSound.kind != "lanes") units = {unitSound};
                for (const auto& unit : units) {
                    auto result = compile(unit.source, unit.path);
                    if (!result.program)
                        fail(result.diagnostics.empty() ? std::string("Lane sound did not compile")
                                                        : result.diagnostics.front().message);
                    const auto& program = *result.program;
                    if (program.nodes.size() > 2048 || program.states.size() > 256 || program.completion.size() > 4096 ||
                        program.analysis.size() > 2048)
                        fail("Lane sound exceeds the native graph budget");
                    auto p = std::make_unique<PreparedProgram>(program, unit.source, unitRevision, sampleRate,
                                                               sampleAssets_.bank(program));
                    p->documentGeneration = unitGeneration;
                    prepared.push_back(std::move(p));
                }
            } catch (const std::exception& exception) {
                unitError = exception.what();
                prepared.clear();
            }
            std::lock_guard lock(mutex_);
            if (sequenceSound_.pinned() && unitRevision == sequenceUnitRevision_ && sampleRate == runtime_.rate.load() &&
                std::all_of(unitSound.lanes.begin(), unitSound.lanes.end(), [&](const auto& lane) {
                    return lane.kind != "active" || (lane.path == document_.path && sourceRevision == document_.revision);
                })) {
                preparedSequenceUnitRevision_ = unitRevision;
                preparedSequenceRate_ = sampleRate;
                preparedUnitKey = unitKey;
                sequenceSoundStatus_ = prepared.empty() ? "error" : "ready";
                sequenceSoundError_ = unitError;
                if (!prepared.empty()) {
                    sequenceSession.submit(std::move(prepared));
                    runtime_.sequenceTargetRevision.store(unitRevision, std::memory_order_release);
                    runtime_.sequenceSourcePlayable.store(true, std::memory_order_release);
                } else {
                    runtime_.sequenceSourcePlayable.store(false, std::memory_order_release);
                    runtime_.sequencePanic.store(true);
                    sequenceSession.requestPanic();
                }
            }
        }
        if (analysisProgram) {
            std::lock_guard lock(mutex_);
            if (lastSelection != selectionVersion_ && analyzerRevision == compiled_.revision) {
                auto selection = selectionForNames(*analysisProgram, analyzerRevision, selectedSignals_, selectedVoice_, projection_);
                if (selection && !auditionSignal_.empty()) {
                    selection.selection.audition = auditionValue(*analysisProgram, auditionSignal_);
                    if (selection.selection.audition == invalid_value) auditionSignal_.clear();
                }
                if (selection && session.setProbes(selection.selection)) {
                    analyzer.configure(*analysisProgram, analyzerRevision, selection.analysisNames, false);
                    selectionError.clear();
                    analyzerSignalNames = selectedSignals_;
                    lastSelection = selectionVersion_;
                } else if (!selection) {
                    selectionError = selection.error;
                    lastSelection = selectionVersion_;
                }
            }
        }
        while (auto* frame = session.frames().readSlot()) {
            analyzer.consume(*frame);
            session.frames().commitRead();
        }
        while (sequenceSession.frames().readSlot()) sequenceSession.frames().commitRead();
        const auto now = std::chrono::steady_clock::now();
        if (now >= nextCheckpoint) {
            auto files = workspaceFiles(workspace_);
            auto samples = sampleAssets_.catalogue();
            try {
                json::Value recovery;
                std::vector<std::string> sources;
                {
                    std::lock_guard lock(mutex_);
                    files_ = std::move(files);
                    samples_ = std::move(samples);
                    updateCheckpointLocked();
                    if (options_.standalone) {
                        recovery = recoveryLocked(false);
                        sources = recoverySourcesLocked();
                    }
                }
                if (options_.standalone) {
                    // Large immutable asset segments are serialized once and
                    // written only when source/settings/assets actually change.
                    // Neither JSON serialization nor disk I/O holds the UI lock.
                    auto assets = sampleAssets_.serializedBundle(sources);
                    auto prefix = recoveryPrefix(recovery);
                    if (prefix != lastRecoveryPrefix || assets != lastRecoveryAssets) {
                        writeRecovery(prefix, *assets);
                        lastRecoveryPrefix = std::move(prefix);
                        lastRecoveryAssets = std::move(assets);
                    }
                }
            } catch (const std::exception& error) {
                std::lock_guard lock(mutex_);
                const auto message = std::string("Recovery assets could not be packed: ") + error.what();
                if (checkpointError_ != message) {
                    checkpointError_ = message;
                    agentEvents_.push_back(json::object({{"status", "failed"}, {"error", message}}));
                }
            }
            nextCheckpoint = now + std::chrono::milliseconds(500);
        }
        if (now >= nextTelemetry) {
            auto a = analyzer.snapshot();
            json::Array signals;
            const auto add = [&](const AnalysisView& v) {
                const auto kind = v.kind == "analysis" ? "analysis"
                                  : (v.kind == "mixed_audio" || (analysisProgram && [&] {
                                         for (const auto& n : analysisProgram->named_values)
                                             if (n.name == v.name) return audioNode(*analysisProgram, n.value);
                                         return false;
                                     }()))
                                      ? "audio"
                                      : "control";
                auto values = v.values;
                if (v.shape == "scalar" && v.valid) values = {static_cast<float>(v.scalar)};
                const bool spectral = v.shape == "spectrum" || v.shape == "mel_spectrum";
                signals.push_back(json::object({{"name", v.name},
                                                {"kind", kind},
                                                {"captureKind", v.kind},
                                                {"shape", v.shape},
                                                {"unit", v.unit},
                                                {"values", floatArray(values)},
                                                {"signedSpectrum", v.signedSpectrum},
                                                {"detail", floatArray(v.detail)},
                                                {"detailSeconds", v.detailSeconds},
                                                {"beatAvailable", v.beatAvailable},
                                                {"beatPhase", v.beatPhase},
                                                {"beatMin", floatArray(v.beatMin)},
                                                {"beatMax", floatArray(v.beatMax)},
                                                {"beatAge", floatArray(v.beatAge)},
                                                {"sampleCount", static_cast<std::int64_t>(v.sampleCount)},
                                                {"windowReady", v.windowReady},
                                                {"rms", v.rms},
                                                {"peak", v.peak},
                                                {"scalar", v.scalar},
                                                {"valid", v.valid},
                                                {"error", v.error},
                                                {"spectrum", floatArray(spectral ? values : v.spectrum)},
                                                {"frequencies", floatArray(v.frequencies)},
                                                {"spectrumFrequencies", floatArray(v.spectrumFrequencies)}}));
            };
            for (const auto& v : a.signals) add(v);
            for (const auto& v : a.results) add(v);
            // Capture uses ValueId identity. A DSP assignment alias (copy = raw)
            // shares that channel but still needs telemetry under its authored
            // name. Materialize only subscribed aliases, not every alias in code.
            if (analysisProgram)
                for (const auto& name : analyzerSignalNames) {
                    const auto& named = analysisProgram->named_values;
                    const auto alias = std::find_if(named.begin(), named.end(), [&](const auto& value) { return value.name == name; });
                    if (alias == named.end()) continue;
                    const auto canonical =
                        std::find_if(named.begin(), named.end(), [&](const auto& value) { return value.value == alias->value; });
                    if (canonical == alias) continue;
                    const auto captured = std::find_if(a.signals.begin(), a.signals.end(),
                                                       [&](const auto& view) { return view.name == canonical->name; });
                    if (captured != a.signals.end()) {
                        auto view = *captured;
                        view.name = name;
                        add(view);
                    }
                }
            std::lock_guard lock(mutex_);
            const bool selectedObservation = runtime_.sourcePlayable.load() && !document_.path.empty() &&
                                             a.revision >= runtime_.audioDocumentRevision.load();
            if (!selectedObservation || !selectionError.empty()) signals.clear();
            const bool demo = runtime_.demo.load();
            const int voices = sequenceSound_.pinned() ? static_cast<int>(runtime_.mixedVoices.load())
                                                       : (selectedObservation ? static_cast<int>(session.status.voices.load()) : 0);
            latestTelemetry_ = json::object(
                {{"captureIdle", a.captureIdle},
                 {"captureHeld", a.captureHeld},
                 {"captureMode", a.continuous ? "transport" : "performance"},
                 {"keyEpoch", static_cast<std::int64_t>(a.keyEpoch)},
                 {"sequence", static_cast<std::int64_t>(++telemetrySequence)},
                 {"programRevision", static_cast<std::int64_t>(selectedObservation ? a.revision : 0)},
                 {"sampleRate", sampleRate},
                 {"signals", std::move(signals)},
                 {"sequencerDocumentId", sequenceSound_.pinned() ? sequenceOwner_ : document_.path},
                 {"sequencerStep", demo ? runtime_.sequencePlayhead.load() : -1},
                 {"sequencerPlaying", demo},
                 {"captureVoice", a.voice},
                 {"captureProjection", projectionName(a.projection)},
                 {"error", selectionError},
                 {"outputPeak", runtime_.mixedPeak.load()},
                 {"voices", voices},
                 {"cpu", session.status.cpu.load()},
                 {"gap", !selectedObservation || !a.hasCapture || !selectionError.empty()},
                 {"analysisReady", selectedObservation && a.ready && selectionError.empty()},
                 {"status", !selectedObservation ? std::string("waiting for selected instrument")
                                                 : !selectionError.empty() ? selectionError : a.status},
                 {"generation", static_cast<std::int64_t>(a.generation)},
                 {"parameterEpoch", static_cast<std::int64_t>(a.parameterEpoch)},
                 {"observationEpoch", static_cast<std::int64_t>(a.observationEpoch)},
                 {"captureStart", static_cast<std::int64_t>(a.start)},
                 {"captureEnd", static_cast<std::int64_t>(a.end)},
                 {"captureResets", static_cast<std::int64_t>(a.resets)},
                 {"captureDrops", static_cast<std::int64_t>(session.status.drops.load())},
                 {"processErrors", static_cast<std::int64_t>(session.status.errors.load() + sequenceSession.status.errors.load())},
                 {"hostOverflows", static_cast<std::int64_t>(runtime_.hostOverflows.load())}});
            nextTelemetry = now + std::chrono::milliseconds(33);
        }
        session.reap();
        sequenceSession.reap();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    session.reap();
    sequenceSession.reap();
}
}  // namespace flows::app
