#include "RealtimeSession.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace flows::app {
PreparedProgram::PreparedProgram(Program p, std::string text, std::uint64_t rev, double rate, SampleBank samples)
    : program(std::move(p)),
      source(std::move(text)), structure(dsp_structure_key(program) + sample_bank_key(samples)), revision(rev), sampleRate(rate) {
    engine = std::make_unique<Engine>(program, EngineConfig{rate, maxFrames, 32, 0.003}, std::move(samples));
    audioValues.resize(program.nodes.size());
    for (std::size_t id = 0; id < program.nodes.size(); ++id) {
        const auto& node = program.nodes[id];
        bool audio = node.op == Op::phasor || node.op == Op::sampler || node.op == Op::sine || node.op == Op::saw || node.op == Op::unison_sine || node.op == Op::unison_saw;
        if (node.op != Op::lfo && node.op != Op::adsr && node.op != Op::key_trigger)
            for (auto input : node.inputs) audio = audio || audioValues[input];
        audioValues[id] = audio;
    }
}

RealtimeSession::RealtimeSession()
    : frames_(std::make_unique<SpscQueue<CaptureFrame, 128>>()), scratchFrame_(std::make_unique<CaptureFrame>()) {}
RealtimeSession::~RealtimeSession() { shutdown(); }
void RealtimeSession::submit(std::unique_ptr<PreparedProgram> p) {
    // Exchange transfers exclusive ownership: the producer destroys only an unconsumed candidate.
    delete pending_.exchange(p.release(), std::memory_order_acq_rel);
}
void RealtimeSession::reap() { PreparedProgram* p = nullptr; while (retired_.pop(p)) delete p; }
void RealtimeSession::shutdown() {
    delete pending_.exchange(nullptr); reap(); delete active_; delete outgoing_;
    active_ = outgoing_ = nullptr;
}
bool RealtimeSession::setProbes(ProbeSelection p) noexcept {
    if (p.count > captureChannels || p.voice < -1 || p.voice >= 32 || p.projection < ProbeProjection::mid || p.projection > ProbeProjection::side) return false;
    return selections_.push(p);
}
void RealtimeSession::select(ProbeSelection selected) noexcept {
    selected.count = std::min<std::uint32_t>(selected.count, captureChannels);
    if (selected.voice < -1 || selected.voice >= 32) selected.voice = -1;
    if (active_) {
        std::uint32_t kept = 0;
        for (std::uint32_t i = 0; i < selected.count; ++i) {
            if (selected.values[i] >= active_->program.nodes.size()) continue;
            selected.values[kept] = selected.values[i]; selected.audio[kept++] = selected.audio[i];
        }
        selected.count = kept;
        if (selected.audition >= active_->audioValues.size() || !active_->audioValues[selected.audition] ||
            (std::none_of(active_->program.named_values.begin(), active_->program.named_values.end(),
                [&](const NamedValue& value) { return value.value == selected.audition; }) &&
             std::none_of(active_->program.analysis.begin(), active_->program.analysis.end(),
                [&](const AnalysisNode& node) { return node.op == AnalysisOp::probe && node.signal == selected.audition; }))) selected.audition = invalid_value;
    }
    bool changed = selected.count != probes_.count || selected.voice != probes_.voice || selected.projection != probes_.projection;
    for (std::uint32_t i = 0; i < selected.count && !changed; ++i)
        changed = selected.values[i] != probes_.values[i] || selected.audio[i] != probes_.audio[i];
    if (changed && probes_.audition != invalid_value && selected.audition == probes_.audition)
        selected.audition = invalid_value; // A different probe/voice selection returns to the main instrument.
    changed = changed || selected.audition != probes_.audition;
    if (changed) ++observationEpoch_;
    probes_ = selected;
    auditionTarget_ = selected.audition;
    if (active_) {
        std::array<ValueId, captureChannels + 2> values{};
        std::copy_n(probes_.values.begin(), probes_.count, values.begin());
        auto count = probes_.count;
        values[count++] = auditionTarget_; values[count++] = auditionValue_;
        active_->engine->set_capture_values(std::span(values).first(count));
    }
}
void RealtimeSession::panic() noexcept {
    if (active_) active_->engine->panic();
    if (outgoing_) outgoing_->engine->panic();
    if (auditionValue_ != invalid_value || auditionTarget_ != invalid_value || auditionMix_ != 0.0) ++observationEpoch_;
    auditionValue_ = auditionTarget_ = outgoingAuditionValue_ = probes_.audition = invalid_value;
    auditionMix_ = 0.0;
    for (auto& h : held_) h.used = false;
    // Stop rendering an outgoing program after an error/panic, even if its mono code is ungated.
    fadeRemaining_ = 0;
    if (outgoing_ && retired_.push(outgoing_)) outgoing_ = nullptr;
}
void RealtimeSession::setTransport(Transport transport) noexcept {
    if (!std::isfinite(transport.bpm) || transport.bpm <= 0 ||
        (transport.positioned && !std::isfinite(transport.ppq))) return;
    if (haveTransport_ && (transport.bpm != transport_.bpm || transport.playing != transport_.playing ||
        transport.positioned != transport_.positioned ||
        (transport.positioned && std::abs(transport.ppq - transport_.ppq) > 1e-7))) ++observationEpoch_;
    transport_ = transport; haveTransport_ = true;
}
void RealtimeSession::advanceTransport(std::size_t frames) noexcept {
    if (transport_.positioned && transport_.playing)
        transport_.ppq += double(frames) * transport_.bpm / (60.0 * (active_ ? active_->sampleRate : 48000.0));
}
void RealtimeSession::finishSilent(std::size_t frames) noexcept {
    advanceTransport(frames);
    clock_ += frames;
    status.samples.store(clock_, std::memory_order_release);
    status.voices.store(0, std::memory_order_relaxed);
    status.peak.store(0, std::memory_order_relaxed);
}

void RealtimeSession::install(std::size_t parameterCapacity) noexcept {
    authoredUpdates_ = 0;
    if (outgoing_ || !retired_.writeSlot()) return;
    auto* next = pending_.exchange(nullptr, std::memory_order_acq_rel);
    if (!next) return;
    if (active_ && next->revision < active_->revision) { retired_.push(next); return; }
    next->engine->set_transport(transport_);
    const bool differentDocument = active_ && next->documentGeneration != active_->documentGeneration;
    if (differentDocument) panic();
    const bool same = active_ && !differentDocument && next->sampleRate == active_->sampleRate && next->structure == active_->structure;
    if (same) {
        for (ValueId id = 0; id < next->program.nodes.size(); ++id) {
            const auto& node = next->program.nodes[id];
            if (node.op != Op::parameter || node.initial_value == active_->program.nodes[id].initial_value) continue;
            // On a retained engine these entries identify host-owned bindings.
            // Its runtime value/ramp is newer than the worker's target snapshot;
            // neither that snapshot nor an authored edit may restart the ramp.
            if (std::any_of(next->initialParameters.begin(), next->initialParameters.end(),
                [&](const ParameterUpdate& update) { return update.parameter == id; })) continue;
            if (authoredUpdates_ == updates_.size()) {
                // Never reset voices just because a live edit exceeded the command budget.
                authoredUpdates_ = 0; status.errors.fetch_add(1); retired_.push(next); return;
            }
            updates_[authoredUpdates_++] = {active_->engine->stats().processed_samples, id, node.initial_value};
        }
        if (authoredUpdates_ > parameterCapacity) {
            // Retry when the host automation batch has room; retain a newer producer candidate if one arrived.
            PreparedProgram* empty = nullptr;
            if (!pending_.compare_exchange_strong(empty, next, std::memory_order_release, std::memory_order_relaxed))
                retired_.push(next);
            authoredUpdates_ = 0;
            return;
        }
        next->engine.swap(active_->engine);
        next->generation = active_->generation;
        retired_.push(active_); active_ = next;
    } else {
        std::size_t n = 0;
        for (const auto& h : held_) if (h.used && h.accepted && !h.completed) { replay_[n] = h.event; replay_[n++].sample = h.order; }
        std::sort(replay_.begin(), replay_.begin() + n, [](const Event& a, const Event& b) { return a.sample < b.sample; });
        for (std::size_t i = 0; i < n; ++i) replay_[i].sample = 0;
        next->generation = generation_ + 1;
        replayingHeld_ = true;
        const auto restored = next->engine->restore_held_notes(std::span(replay_).first(n), this, next->initialParameters);
        replayingHeld_ = false;
        if (!restored) { status.errors.fetch_add(1); retired_.push(next); return; }
        for (auto& held : held_) if (held.used) {
            held.resident = std::any_of(replay_.begin(), replay_.begin() + n,
                [&](const Event& event) { return event.note_id == held.event.note_id; });
        }
        generation_ = next->generation;
        outgoingAuditionValue_ = auditionValue_; outgoingAuditionVoice_ = auditionVoice_;
        outgoing_ = active_; active_ = next;
        if (!next->initialParameters.empty()) ++parameterEpoch_;
        fadeLength_ = std::max<std::size_t>(1, static_cast<std::size_t>(std::llround(0.020 * next->sampleRate)));
        // Switching instruments must never mix the old instrument's ungated
        // output into the selected tab or replay its held notes.
        fadeRemaining_ = outgoing_ && !differentDocument ? fadeLength_ : 0;
    }
    auto selected = next->probes; selected.revision = next->revision;
    select(selected);
    if (!same) { auditionValue_ = probes_.audition; auditionVoice_ = probes_.voice; }
}

void RealtimeSession::setOutputGains(std::span<const OutputGainGroup> groups,float fallback) noexcept {
    outputGroupCount_=std::min(groups.size(),outputGroups_.size());
    std::copy_n(groups.begin(),outputGroupCount_,outputGroups_.begin());defaultOutputGain_=fallback;
}
void RealtimeSession::beginBlock(std::size_t parameterReserve) noexcept {
    if (boundaryPrepared_) return;
    if (panicRequested_.exchange(false, std::memory_order_acq_rel)) panic();
    install(updates_.size() - std::min(parameterReserve, updates_.size()));
    if (hasDeferredSelection_ && active_ && deferredSelection_.revision <= active_->revision) {
        if (deferredSelection_.revision == active_->revision) select(deferredSelection_);
        hasDeferredSelection_ = false;
    }
    ProbeSelection selected;
    // The producer can refill the ring during this callback; capacity alone
    // does not bound a drain-until-empty loop.
    for (std::size_t consumed = 0; consumed < 16 && selections_.pop(selected); ++consumed) {
        if (active_ && selected.revision == active_->revision) select(selected);
        else if ((!active_ || selected.revision > active_->revision) &&
                 (!hasDeferredSelection_ || selected.revision >= deferredSelection_.revision)) {
            deferredSelection_ = selected; hasDeferredSelection_ = true;
        }
    }
    boundaryPrepared_ = true;
}

void RealtimeSession::process(std::span<const Event> input, std::span<float> output,
                              std::span<const ParameterUpdate> parameterInput, bool allowInstall) noexcept {
    auto right = std::span(legacyRight_).first(std::min(output.size(),maxFrames));
    processStereo(input,output,right,parameterInput,allowInstall);
    for (std::size_t i=0;i<right.size();++i) if (output[i]!=right[i]) output[i]=0.5f*output[i]+0.5f*right[i];
}
void RealtimeSession::processStereo(std::span<const Event> input, std::span<float> output, std::span<float> right,
                                    std::span<const ParameterUpdate> parameterInput, bool allowInstall) noexcept {
    std::fill(output.begin(), output.end(), 0.0f);
    std::fill(right.begin(), right.end(), 0.0f);
    if (output.empty()) {
        if (panicRequested_.exchange(false, std::memory_order_acq_rel)) panic();
        if (!input.empty() || !parameterInput.empty()) { panic(); status.errors.fetch_add(1); }
        return; // Keep explicitly prepared authored updates until a real sample can apply them.
    }
    struct BoundaryEnd { bool& prepared; ~BoundaryEnd() { prepared = false; } } boundaryEnd{boundaryPrepared_};
    if (output.size() != right.size() || output.size() > maxFrames || input.size() > events_.size() || parameterInput.size() > updates_.size() ||
        output.size() > std::numeric_limits<std::uint64_t>::max() - clock_) {
        panic(); status.errors.fetch_add(1); finishSilent(output.size()); return;
    }
    if (!boundaryPrepared_) {
        if (allowInstall) beginBlock(parameterInput.size());
        else { authoredUpdates_ = 0; boundaryPrepared_ = true; }
    }
    // The host may detect invalid automation after its explicit beginBlock().
    // Consume that request in this callback even when the boundary is prepared.
    if (panicRequested_.exchange(false, std::memory_order_acq_rel)) panic();
    if (authoredUpdates_ + parameterInput.size() > updates_.size()) {
        panic(); status.errors.fetch_add(1); finishSilent(output.size()); return;
    }
    if (!active_) {
        // Retain keys pressed while the first valid program is being prepared.
        // Admission is decided by that program when it is installed.
        for (std::size_t i = 0; i < input.size(); ++i) {
            const auto& note = input[i];
            if (note.sample < clock_ || note.sample >= clock_ + output.size() ||
                (i > 0 && note.sample < input[i - 1].sample)) {
                panic(); status.errors.fetch_add(1); break;
            }
            auto existing = std::find_if(held_.begin(), held_.end(), [&](const Held& held) {
                return held.used && held.event.note_id == note.note_id;
            });
            if (note.type == EventType::note_on) {
                auto free = std::find_if(held_.begin(), held_.end(), [](const Held& held) { return !held.used; });
                if (existing != held_.end() || free == held_.end() || !std::isfinite(note.pitch) ||
                    note.pitch < 0 || note.pitch > 127 || !std::isfinite(note.velocity) || note.velocity < 0 || note.velocity > 1) {
                    panic(); status.errors.fetch_add(1); break;
                }
                *free = {true, true, false, note, ++order_};
            } else if (note.type == EventType::note_off) {
                if (existing != held_.end()) existing->used = false;
            } else { panic(); status.errors.fetch_add(1); break; }
        }
        finishSilent(output.size()); return;
    }
    const auto engineStart = active_->engine->stats().processed_samples;
    if (auditionMix_ == 0.0) { auditionValue_ = auditionTarget_; auditionVoice_ = probes_.voice; }
    std::fill_n(auditionAudio_.begin(), output.size(), 0.0f);
    std::fill_n(auditionAudioRight_.begin(), output.size(), 0.0f);
    bool invalid = false;
    std::size_t eventCount = 0;
    if (!input.empty()) for (std::size_t i = 0; i < held_.size(); ++i) {
        shadowIds_[i] = held_[i].event.note_id;
        shadowUsed_[i] = held_[i].used; shadowResident_[i] = held_[i].resident;
    }
    for (std::size_t i = 0; i < input.size(); ++i) {
        const auto& event = input[i];
        if (event.sample < clock_ || event.sample >= clock_ + output.size() ||
            (i > 0 && event.sample < input[i - 1].sample)) { invalid = true; break; }
        std::size_t existing = held_.size(), free = held_.size();
        for (std::size_t j = 0; j < held_.size(); ++j) {
            if (shadowUsed_[j] && shadowIds_[j] == event.note_id) existing = j;
            else if (!shadowUsed_[j] && free == held_.size()) free = j;
        }
        if (event.type == EventType::note_on) {
            if (existing != held_.size() || free == held_.size()) { invalid = true; break; }
            shadowIds_[free] = event.note_id; shadowUsed_[free] = true; shadowResident_[free] = true;
        } else if (event.type == EventType::note_off) {
            if (existing == held_.size()) { invalid = true; break; }
            shadowUsed_[existing] = false;
            if (!shadowResident_[existing]) {
                for (auto& held : held_) if (held.used && held.event.note_id == event.note_id) held.used = false;
                continue; // Rejected under an older program; the new engine has never seen this ID.
            }
        } else { invalid = true; break; }
        events_[eventCount] = event;
        events_[eventCount++].sample = engineStart + event.sample - clock_;
    }
    for (std::size_t i = 0; i < parameterInput.size() && !invalid; ++i) {
        if (parameterInput[i].sample < clock_ || parameterInput[i].sample >= clock_ + output.size()) { invalid = true; break; }
        updates_[authoredUpdates_ + i] = parameterInput[i];
        updates_[authoredUpdates_ + i].sample = engineStart + parameterInput[i].sample - clock_;
    }
    if (invalid) { panic(); status.errors.fetch_add(1); finishSilent(output.size()); return; }

    capturing_ = probes_.count ? frames_->writeSlot() : nullptr;
    const bool publish = capturing_ != nullptr;
    if (probes_.count && !capturing_) { capturing_ = scratchFrame_.get(); status.drops.fetch_add(1); }
    if (capturing_) {
        auto& frame = *capturing_;
        frame.revision = active_->revision; frame.generation = active_->generation;
        frame.transport = transport_;frame.continuous=continuousObservation_;frame.keyEpoch=keyEpoch_;frame.keyStart=keyStart_;
        frame.performanceActive = active_->engine->stats().active_voices > 0;
        frame.start = clock_; frame.sequence = sequence_++; frame.sampleRate = active_->sampleRate;
        frame.count = static_cast<std::uint32_t>(output.size()); frame.channels = probes_.count;
        frame.values = probes_.values; frame.audio = probes_.audio;
        frame.parameterEpoch = parameterEpoch_; frame.observationEpoch = observationEpoch_;
        frame.voice = probes_.voice; frame.voiceGeneration = 0; frame.projection = probes_.projection;
        frame.channelVoices.fill(-1); frame.channelVoiceGenerations.fill(0);
        frame.transition = fadeRemaining_ > 0 || authoredUpdates_ > 0 || !parameterInput.empty();
        for (std::size_t c = 0; c < frame.channels; ++c) std::fill_n(frame.samples[c].begin(), output.size(), 0.0f);
    }
    captureEngineStart_ = engineStart;
    active_->engine->set_output_gains(std::span(outputGroups_).first(outputGroupCount_),defaultOutputGain_);
    active_->engine->set_transport(transport_);
    const auto result = active_->engine->process_stereo(std::span(events_).first(eventCount), output, right, this,
        std::span(updates_).first(authoredUpdates_ + parameterInput.size()));
    if (!result) {
        status.errors.fetch_add(1); panic(); capturing_ = nullptr;
        std::fill(output.begin(), output.end(), 0.0f); std::fill(right.begin(), right.end(), 0.0f); finishSilent(output.size()); return;
    }
    if (authoredUpdates_ || !parameterInput.empty()) ++parameterEpoch_;
    if (capturing_) capturing_->parameterEpoch = parameterEpoch_;
    if (outgoing_) {
        outgoing_->engine->set_output_gains(std::span(outputGroups_).first(outputGroupCount_),defaultOutputGain_);
        const auto n = std::min(fadeRemaining_, output.size());
        std::fill_n(outgoingAuditionAudio_.begin(), n, 0.0f);
        std::fill_n(outgoingAuditionAudioRight_.begin(), n, 0.0f);
        outgoingCaptureStart_ = outgoing_->engine->stats().processed_samples;
        outgoingCaptureCount_ = n; capturingOutgoing_ = true;
        outgoing_->engine->set_transport(transport_);
        const auto previous = outgoing_->engine->process_stereo({}, std::span(oldAudio_).first(n), std::span(oldAudioRight_).first(n), this);
        capturingOutgoing_ = false;
        if (!previous) {
            status.errors.fetch_add(1); panic(); capturing_ = nullptr;
            std::fill(output.begin(), output.end(), 0.0f); std::fill(right.begin(), right.end(), 0.0f); finishSilent(output.size()); return;
        }
        for (std::size_t i = 0; i < n; ++i) {
            const auto oldWeight = static_cast<float>(fadeRemaining_ - i) / static_cast<float>(fadeLength_);
            const auto oldAudition = outgoingAuditionValue_ != invalid_value ? outgoingAuditionAudio_[i] : oldAudio_[i];
            const auto newAudition = auditionValue_ != invalid_value ? auditionAudio_[i] : output[i];
            auditionAudio_[i] = newAudition * (1.0f - oldWeight) + oldAudition * oldWeight;
            output[i] = output[i] * (1.0f - oldWeight) + oldAudio_[i] * oldWeight;
            const auto oldRight = outgoingAuditionValue_ != invalid_value ? outgoingAuditionAudioRight_[i] : oldAudioRight_[i];
            const auto newRight = auditionValue_ != invalid_value ? auditionAudioRight_[i] : right[i];
            auditionAudioRight_[i] = newRight * (1.0f - oldWeight) + oldRight * oldWeight;
            right[i] = right[i] * (1.0f - oldWeight) + oldAudioRight_[i] * oldWeight;
        }
        fadeRemaining_ -= n;
        if (fadeRemaining_ == 0 && retired_.push(outgoing_)) outgoing_ = nullptr;
    }
    double auditionTargetMix = auditionValue_ != invalid_value && auditionValue_ == auditionTarget_ ? 1.0 : 0.0;
    const auto auditionStep = 1.0 / std::max(1.0, std::round(0.010 * active_->sampleRate));
    for (std::size_t i = 0; i < output.size(); ++i) {
        const auto monitored = auditionMix_ == 0.0 ? output[i] :
            output[i] * static_cast<float>(1.0 - auditionMix_) + auditionAudio_[i] * static_cast<float>(auditionMix_);
        const auto monitoredRight = auditionMix_ == 0.0 ? right[i] :
            right[i] * static_cast<float>(1.0 - auditionMix_) + auditionAudioRight_[i] * static_cast<float>(auditionMix_);
        if (std::isfinite(monitored) && std::isfinite(monitoredRight)) { output[i] = monitored; right[i] = monitoredRight; }
        else {
            // An unused intermediate can be nonfinite while the instrument remains valid.
            // Stop listening to that signal without resetting the playing DSP state.
            auditionMix_ = auditionTargetMix = 0.0;
            auditionValue_ = auditionTarget_ = probes_.audition = invalid_value;
            ++observationEpoch_; status.errors.fetch_add(1);
            if (capturing_) { capturing_->transition = true; capturing_->observationEpoch = observationEpoch_; }
        }
        if (auditionMix_ < auditionTargetMix) auditionMix_ = std::min(auditionTargetMix, auditionMix_ + auditionStep);
        else if (auditionMix_ > auditionTargetMix) auditionMix_ = std::max(auditionTargetMix, auditionMix_ - auditionStep);
    }
    float peak = 0;
    for (auto sample : output) peak = std::max(peak, std::abs(sample));
    for (auto sample : right) peak = std::max(peak, std::abs(sample));
    status.peak.store(peak, std::memory_order_relaxed);
    status.voices.store(active_->engine->stats().active_voices, std::memory_order_relaxed);
    if (capturing_) {
        capturing_->performanceActive = capturing_->performanceActive ||
            active_->engine->stats().active_voices > 0 || peak > 1e-7f;
        // Outside a performance there are no sounding voices to observe.
        // Flush the display with silence, not a still-running mono oscillator.
        if (!continuousObservation_ && !capturing_->performanceActive)
            for (std::size_t c = 0; c < capturing_->channels; ++c)
                std::fill_n(capturing_->samples[c].begin(), output.size(), 0.0f);
    }
    status.revision.store(active_->revision, std::memory_order_release);
    status.generation.store(active_->generation, std::memory_order_release);
    status.parameterEpoch.store(parameterEpoch_, std::memory_order_release);
    status.observationEpoch.store(observationEpoch_, std::memory_order_release);
    advanceTransport(output.size());
    clock_ += output.size(); status.samples.store(clock_, std::memory_order_release);
    capturing_ = nullptr;
    if (publish) frames_->commitWrite();
}

void RealtimeSession::capture(ValueId value, VoiceIdentity voice, std::uint64_t start,
                              std::span<const float> samples) noexcept {
    capture_stereo(value,voice,start,samples,samples);
}
void RealtimeSession::capture_stereo(ValueId value, VoiceIdentity voice, std::uint64_t start,
                                     std::span<const float> samples, std::span<const float> right) noexcept {
    if (samples.size()!=right.size()) return;
    if (capturingOutgoing_) {
        if (value != outgoingAuditionValue_ || start < outgoingCaptureStart_ ||
            (outgoingAuditionVoice_ >= 0 && voice.slot != static_cast<std::uint32_t>(outgoingAuditionVoice_))) return;
        const auto offset = static_cast<std::size_t>(start - outgoingCaptureStart_);
        if (offset > outgoingCaptureCount_ || samples.size() > outgoingCaptureCount_ - offset) return;
        for (std::size_t i = 0; i < samples.size(); ++i) { outgoingAuditionAudio_[offset + i] += samples[i]; outgoingAuditionAudioRight_[offset + i] += right[i]; }
        return;
    }
    if (value == auditionValue_ && start >= captureEngineStart_ &&
        (auditionVoice_ < 0 || voice.slot == static_cast<std::uint32_t>(auditionVoice_))) {
        const auto offset = static_cast<std::size_t>(start - captureEngineStart_);
        if (offset <= maxFrames && samples.size() <= maxFrames - offset)
            for (std::size_t i = 0; i < samples.size(); ++i) { auditionAudio_[offset + i] += samples[i]; auditionAudioRight_[offset + i] += right[i]; }
    }
    if (!capturing_ || (probes_.voice >= 0 && voice.slot != static_cast<std::uint32_t>(probes_.voice)) || start < captureEngineStart_) return;
    const auto offset = static_cast<std::size_t>(start - captureEngineStart_);
    if (offset > capturing_->count || samples.size() > capturing_->count - offset) return;
    if (probes_.voice >= 0) {
        if (capturing_->voiceGeneration != 0 && capturing_->voiceGeneration != voice.generation) capturing_->transition = true;
        capturing_->voiceGeneration = voice.generation;
    }
    for (std::size_t c = 0; c < probes_.count; ++c) if (probes_.values[c] == value) {
        auto* target = capturing_->samples[c].data() + offset;
        if (!probes_.audio[c] || probes_.voice >= 0) {
            capturing_->channelVoices[c] = static_cast<int>(voice.slot);
            capturing_->channelVoiceGenerations[c] = voice.generation;
        }
        for (std::size_t i = 0; i < samples.size(); ++i) {
            auto projected=samples[i];
            // Projection describes audio channels; mono control signals retain
            // their authored value, including when the audio view selects Side.
            if (probes_.audio[c]) switch (probes_.projection) {
                case ProbeProjection::mid: projected=0.5f*samples[i]+0.5f*right[i]; break;
                case ProbeProjection::left: break;
                case ProbeProjection::right: projected=right[i]; break;
                case ProbeProjection::side: projected=0.5f*samples[i]-0.5f*right[i]; break;
            }
            if (probes_.audio[c] && probes_.voice < 0) target[i] += projected;
            else target[i] = projected;
        }
    }
}

void RealtimeSession::event(const Event& e, VoiceIdentity, std::string_view action) noexcept {
    if (capturingOutgoing_) return;
    if(action=="accepted" && !replayingHeld_) {
        ++keyEpoch_;keyStart_=clock_+(e.sample-captureEngineStart_);
        if(capturing_) {
            // Several poly notes can start in one callback. Keep the first
            // onset of an inactive packet so its attack is not clipped away.
            if(!capturing_->performanceActive) capturing_->keyStart=keyStart_;
            capturing_->keyEpoch=keyEpoch_;capturing_->performanceActive=true;
        }
    }
    const bool mono = active_ && active_->program.voice_policy.mode == VoiceMode::mono;
    if (action == "completed") {
        // All resident mono keys address one completed kernel; replaying an
        // older held key would otherwise restart it under a different pitch.
        for (auto& h : held_) if (h.used && h.accepted && h.resident && (mono || h.event.note_id == e.note_id)) h.completed = true;
        return;
    }
    if (action == "accepted" && mono && !replayingHeld_) {
        // A new mono press reactivates the resident priority stack. Keys left
        // out of an earlier rebuild are no longer in that engine's ledger.
        for (auto& h : held_) if (h.used && h.accepted && h.resident) h.completed = false;
    }
    for (auto& h : held_) if (h.used && h.event.note_id == e.note_id) {
        if (action == "accepted") { h.accepted = true; h.resident = true; h.completed = false; }
        else if (action == "rejected") { h.accepted = false; h.resident = true; }
        else if (action == "released") h.used = false;
        return;
    }
    if (action == "accepted" || action == "rejected") {
        auto free = std::find_if(held_.begin(), held_.end(), [](const Held& h) { return !h.used; });
        if (free != held_.end()) *free = {true, action == "accepted", true, e, ++order_};
    }
}
} // namespace flows::app
