#include "AudioRuntime.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

#if defined(__SSE__) || defined(__x86_64__)
#include <xmmintrin.h>
#endif

namespace flows::app {
namespace {
// Flush denormals for the duration of one callback, like juce::ScopedNoDenormals.
class ScopedNoDenormals {
public:
    ScopedNoDenormals() noexcept {
#if defined(__aarch64__)
        asm volatile("mrs %0, fpcr" : "=r"(saved_));
        const std::uint64_t flushed = saved_ | (1ull << 24);
        asm volatile("msr fpcr, %0" : : "r"(flushed));
#elif defined(__SSE__) || defined(__x86_64__)
        saved_ = _mm_getcsr();
        _mm_setcsr(static_cast<unsigned>(saved_) | 0x8040);
#endif
    }
    ~ScopedNoDenormals() {
#if defined(__aarch64__)
        asm volatile("msr fpcr, %0" : : "r"(saved_));
#elif defined(__SSE__) || defined(__x86_64__)
        _mm_setcsr(static_cast<unsigned>(saved_));
#endif
    }

private:
    std::uint64_t saved_ = 0;
};
constexpr std::uint64_t noteMaskBit = 1ull << 61;
}  // namespace

double AudioRuntime::steadySeconds() noexcept {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
void AudioRuntime::prepare(double sampleRate, int samplesPerBlock) noexcept {
    sequenceRouting_ = {};
    sequenceRouteEnabled_ = false;
    rate.store(sampleRate);
    blockSize.store(samplesPerBlock);
    running.store(true);
    panic.store(true);
}
void AudioRuntime::release() noexcept {
    running.store(false);
    panic.store(true);
    sequencePanic.store(true);
    session.requestPanic();
    sequenceSession.requestPanic();
}
void AudioRuntime::beginHostAutomation() noexcept {
    hostPointCount_ = 0;
    rawHostPoints_ = true;
    hostPointOverflow_ = false;
}
void AudioRuntime::hostParameter(int slot, int offset, float value) noexcept {
    if (slot < 0 || slot >= 32 || offset < 0 || !std::isfinite(value)) return;
    if (hostPointCount_ >= hostPoints_.size()) {
        hostPointOverflow_ = true;
        return;
    }
    hostPoints_[hostPointCount_++] = {slot, offset, std::clamp(value, 0.0f, 1.0f)};
}
void AudioRuntime::auParameter(int slot, int offset, unsigned int duration, float start, float end, bool ramp) noexcept {
    if (slot < 0 || slot >= 32 || !std::isfinite(start) || !std::isfinite(end)) {
        auOverflow_.store(true);
        return;
    }
    if (!auPoints_.push({slot, offset, std::clamp(end, 0.0f, 1.0f), duration, std::clamp(start, 0.0f, 1.0f), ramp ? 2 : 1}))
        auOverflow_.store(true);
}

void AudioRuntime::process(const AudioBlock& block) noexcept {
    ScopedNoDenormals guard;
    const double started = clock_ ? clock_() : 0;
    const int total = block.frames;
    for (int i = 0; i < total; ++i) {
        block.left[i] = 0;
        if (block.right) block.right[i] = 0;
    }
    if (total == 0) {
        rawHostPoints_ = false;
        hostPointCount_ = 0;
        return;
    }
    const auto clock = session.status.samples.load();
    const bool pinned = sequencePinned.load(std::memory_order_acquire);
    const bool resetSequence = sequencePanic.exchange(false);
    if (resetSequence) sequenceSession.requestPanic();
    Transport transport{bpm.load(), fallbackPpq_, true, true};
    bool tempoFromHost = false;
    // Standalone and browser hosts pass no position: the device clock has no
    // musical transport and must not stop the local pattern or transport LFO.
    // Only plugins follow host state.
    if (const auto* position = block.position) {
        transport.playing = position->playing;
        if (const auto tempo = position->bpm; tempo && std::isfinite(*tempo) && *tempo > 0 && *tempo <= 1000) {
            transport.bpm = *tempo;
            tempoFromHost = true;
        }
        if (const auto beat = position->ppq; beat && std::isfinite(*beat)) transport.ppq = *beat;
        if (const auto sample = position->timeInSamples; sample && position->playing) {
            if (nextTransportSample_ && *sample != *nextTransportSample_) {
                panic.store(true);
                sequencePanic.store(true);
                sequenceSession.requestPanic();
            }
            nextTransportSample_ = *sample + total;
        } else
            nextTransportSample_.reset();
    }
    reportedBpm.store(transport.bpm);
    hostTempo.store(tempoFromHost);
    HostPoint auPoint;
    for (std::size_t drained = 0; drained < 256 && auPoints_.pop(auPoint); ++drained) {
        if (hostPointCount_ < hostPoints_.size())
            hostPoints_[hostPointCount_++] = auPoint;
        else
            hostPointOverflow_ = true;
    }
    if (auPoints_.hasPending()) hostPointOverflow_ = true;
    const bool automationOverflow = hostPointOverflow_ || auOverflow_.exchange(false);
    if (automationOverflow) hostOverflows.fetch_add(1);
    if (panic.exchange(false) || automationOverflow) {
        session.requestPanic();
        for (auto& h : midiHeld_) h.used = false;
        KeyboardEvent discarded;
        for (std::size_t i = 0; i < 256 && keyboard.pop(discarded); ++i) {
        }
        if (!pinned) {
            sequencer_.reset();
            sequencePlayhead.store(-1);
        }
        if (hostPointOverflow_) hostPointCount_ = 0;
    }
    hostPointOverflow_ = false;
    std::size_t events = 0;
    auto append = [&](Event e) {
        if (events == blockEvents_.size()) {
            panic.store(true);
            return false;
        }
        blockEvents_[events++] = e;
        return true;
    };
    KeyboardEvent uiEvent;
    for (std::size_t drained = 0; drained < 256 && keyboard.pop(uiEvent); ++drained) {
        if (uiEvent.documentRevision != audioDocumentRevision.load(std::memory_order_acquire)) continue;
        uiEvent.event.sample = clock;
        if (!append(uiEvent.event)) break;
    }
    std::size_t midiMessages = 0;
    for (const auto& message : block.midi) {
        if (++midiMessages > 256) {
            panic.store(true);
            hostOverflows.fetch_add(1);
            break;
        }
        const auto* bytes = message.data;
        if (!bytes || message.size < 3) continue;
        const int kind = bytes[0] & 0xf0, channel = (bytes[0] & 0x0f) + 1, pitch = bytes[1] & 127;
        if (kind == 0xb0 && (bytes[1] == 120 || bytes[1] == 123)) {
            panic.store(true);
            continue;
        }
        if (kind != 0x80 && kind != 0x90) continue;
        const auto sample = clock + static_cast<std::uint64_t>(std::clamp(message.samplePosition, 0, total - 1));
        if (kind == 0x90 && bytes[2] != 0) {
            auto free = std::find_if(midiHeld_.begin(), midiHeld_.end(), [](const auto& h) { return !h.used; });
            if (free == midiHeld_.end()) {
                panic.store(true);
                continue;
            }
            *free = {true, channel, pitch, nextNote_++, ++midiOrder_};
            append({sample, EventType::note_on, free->id, static_cast<double>(pitch), (bytes[2] & 127) / 127.0});
        } else {
            MidiHeld* first = nullptr;
            for (auto& h : midiHeld_)
                if (h.used && h.channel == channel && h.pitch == pitch && (!first || h.order < first->order)) first = &h;
            if (first) {
                append({sample, EventType::note_off, first->id, static_cast<double>(pitch), 0});
                first->used = false;
            }
        }
    }
    // Release after this block's MIDI, including a note-on already in the
    // block. The note-off is stamped on the last sample and stays after that
    // note-on because the block sort is stable. UI keys and the sequencer are
    // not in midiHeld_. A full event buffer sets panic and the later panic
    // path resets the instrument, including those other notes.
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
            if (!sequencer_.stop(clock, append)) panic.store(true);
        } else
            sequencer_.reset();
        sequenceAudioPinned_ = pinned;
        sequencePlayhead.store(-1);
    }
    SequencePattern sequence;
    if (sequencePatterns.pop(sequence)) sequencer_.configure(sequence);
    sequenceSession.beginBlock();
    const bool unitReady = !pinned || (sequenceSourcePlayable.load(std::memory_order_acquire) &&
                                       sequenceSession.targetRevision() >= sequenceMinimumRevision.load(std::memory_order_acquire));
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
    if (!sequencer_.process(demo.load(), clock, static_cast<std::uint32_t>(total), rate.load(), transport, sequenceAppend)) {
        if (pinned) {
            sequenceSession.requestPanic();
            sequenceEvents = 0;
            sequencer_.reset();
        } else
            panic.store(true);
        hostOverflows.fetch_add(1);
    }
    sequencePlayhead.store(sequencer_.playhead());
    std::array<OutputGainGroup, sequencerRows> laneGains;
    for (std::size_t i = 0; i < sequencerRows; ++i) {
        const auto& lane = sequencer_.pattern().rows[i];
        laneGains[i] = {noteMaskBit | (7ull << 56), noteMaskBit | (i << 56),
                        i >= sequencer_.pattern().rowCount || lane.mute ? 0.f
                                                                        : static_cast<float>(std::pow(10., lane.volume / 20))};
    }
    session.setOutputGains(laneGains);
    sequenceSession.setOutputGains(laneGains);
    session.setContinuousObservation(demo.load() && transport.playing);
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
    if (panic.exchange(false)) {
        session.requestPanic();
        events = 0;
        for (auto& h : midiHeld_) h.used = false;
        if (!pinned) {
            sequencer_.reset();
            sequencePlayhead.store(-1);
        }
    }
    session.beginBlock(std::min<std::size_t>(256, hostPointCount_ + 32));
    MacroMap map;
    if (macroMaps.pop(map)) waitingMacros_ = map;
    const auto activeRevision = session.targetRevision();
    const bool currentInstrument = sourcePlayable.load(std::memory_order_acquire) &&
                                   activeRevision >= audioDocumentRevision.load(std::memory_order_acquire);
    if (!currentInstrument) {
        session.requestPanic();
        events = 0;
        for (auto& held : midiHeld_) held.used = false;
        if (!pinned) {
            sequencer_.reset();
            sequencePlayhead.store(-1);
        }
    }
    std::array<bool, 32> changedMapping{};
    if (waitingMacros_.revision == activeRevision)
        for (std::size_t i = 0; i < 32; ++i) {
            const auto& old = macros_.bindings[i];
            const auto& next = waitingMacros_.bindings[i];
            changedMapping[i] = old.value != next.value || old.min != next.min || old.max != next.max ||
                                old.logarithmic != next.logarithmic;
        }
    if (waitingMacros_.revision == activeRevision) macros_ = waitingMacros_;
    std::size_t params = 0;
    std::array<bool, 32> received{};
    auto canonical = [](const MacroBinding& b, double normalized) {
        return b.logarithmic && b.min > 0 ? b.min * std::pow(b.max / b.min, normalized) : b.min + normalized * (b.max - b.min);
    };
    auto addParam = [&](int slot, int offset, float normalized, unsigned int duration = 0,
                        std::optional<double> start = {}) {
        const auto& b = macros_.bindings[static_cast<std::size_t>(slot)];
        if (b.value == invalid_value || macros_.revision != activeRevision) return;
        if (offset < 0 || offset >= total || params == blockParameters_.size()) {
            panic.store(true);
            return;
        }
        ParameterUpdate update{clock + static_cast<std::uint64_t>(offset), b.value, canonical(b, normalized), duration,
                               b.logarithmic};
        if (start) update.ramp_start = canonical(b, *start);
        blockParameters_[params++] = update;
    };
    std::array<int, 32> previousOffset;
    previousOffset.fill(-1);
    for (std::size_t i = 0; i < hostPointCount_; ++i) {
        const auto point = hostPoints_[i];
        const int slot = point.slot;
        received[static_cast<std::size_t>(slot)] = true;
        if (point.kind == 0) {
            // VST3 queues interpolate from an implicit point at offset -1.
            const int from = previousOffset[static_cast<std::size_t>(slot)];
            if (point.offset < from || point.offset >= total) {
                panic.store(true);
                continue;
            }
            if (point.offset == 0 || point.offset == from)
                addParam(slot, point.offset, point.value);
            else if (from < 0) {
                const double start = previousMacro_[static_cast<std::size_t>(slot)] +
                                     (point.value - previousMacro_[static_cast<std::size_t>(slot)]) /
                                         static_cast<double>(point.offset + 1);
                addParam(slot, 0, point.value, static_cast<unsigned int>(point.offset), start);
            } else
                addParam(slot, from, point.value, static_cast<unsigned int>(point.offset - from));
            previousOffset[static_cast<std::size_t>(slot)] = point.offset;
        } else if (point.kind == 1 || point.duration == 0)
            addParam(slot, std::max(0, point.offset), point.value);
        else {
            const auto elapsed = point.offset < 0 ? static_cast<std::uint64_t>(-static_cast<std::int64_t>(point.offset)) : 0;
            const auto remaining = elapsed >= point.duration ? 0u : point.duration - static_cast<unsigned int>(elapsed);
            const double start = remaining == 0 ? point.value
                                                : point.start + (point.value - point.start) * static_cast<double>(elapsed) /
                                                                    point.duration;
            addParam(slot, std::max(0, point.offset), point.value, remaining, start);
        }
        previousMacro_[static_cast<std::size_t>(slot)] = point.value;
    }
    for (int i = 0; i < 32; ++i)
        if (!received[static_cast<std::size_t>(i)]) {
            const auto value = parameters_.get(i);
            if (changedMapping[static_cast<std::size_t>(i)] || value != previousMacro_[static_cast<std::size_t>(i)])
                addParam(i, 0, value);
            previousMacro_[static_cast<std::size_t>(i)] = value;
        }
    if (panic.exchange(false)) {
        session.requestPanic();
        events = 0;
        params = 0;
        for (auto& h : midiHeld_) h.used = false;
        if (!pinned) {
            sequencer_.reset();
            sequencePlayhead.store(-1);
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
    float peak = 0;
    bool invalid = false;
    const bool mixSequence = pinned && unitReady && demo.load() && transport.playing && !sequenceAuditionMuted.load();
    const auto routeFade = static_cast<std::uint32_t>(std::max(1.0, std::round(rate.load() * 0.003)));
    for (int offset = 0; offset < total; offset += static_cast<int>(maxFrames)) {
        const auto count = static_cast<std::size_t>(std::min<int>(static_cast<int>(maxFrames), total - offset));
        const auto end = clock + static_cast<std::uint64_t>(offset) + count;
        auto eb = e, pb = p, sb = se;
        while (e < events && blockEvents_[e].sample < end) ++e;
        while (p < params && blockParameters_[p].sample < end) ++p;
        while (se < sequenceEvents && sequenceEvents_[se].sample < end) ++se;
        auto positioned = transport;
        if (positioned.playing) positioned.ppq += double(offset) * positioned.bpm / (60.0 * rate.load());
        session.setTransport(positioned);
        session.processStereo(std::span(blockEvents_).subspan(eb, e - eb), std::span(mono_).first(count),
                              std::span(right_).first(count), std::span(blockParameters_).subspan(pb, p - pb), offset == 0);
        sequenceSession.setTransport(positioned);
        sequenceSession.processStereo(std::span(sequenceEvents_).subspan(sb, se - sb), std::span(sequenceLeft_).first(count),
                                      std::span(sequenceRight_).first(count), {}, offset == 0);
        std::fill_n(clickLeft_.begin(), count, 0);
        std::fill_n(clickRight_.begin(), count, 0);
        metronome_.process(demo.load(), sequencer_.pattern().metronome, rate.load(), positioned,
                           std::span(clickLeft_).first(count), std::span(clickRight_).first(count));
        const bool mixActive = sourcePlayable.load(std::memory_order_acquire) &&
                               activeRevision >= audioDocumentRevision.load(std::memory_order_acquire);
        if (mixSequence != sequenceRouteEnabled_) {
            sequenceRouting_.mark();
            sequenceRouteEnabled_ = mixSequence;
        }
        for (std::size_t i = 0; i < count; ++i) {
            const auto sequenceAudio = sequenceRouting_.tick(mixSequence ? sequenceLeft_[i] : 0,
                                                             mixSequence ? sequenceRight_[i] : 0, routeFade);
            float left = (mixActive ? mono_[i] : 0) + sequenceAudio[0] + clickLeft_[i];
            float right = (mixActive ? right_[i] : 0) + sequenceAudio[1] + clickRight_[i];
            if (!std::isfinite(left) || !std::isfinite(right)) {
                left = right = 0;
                invalid = true;
            }
            block.left[offset + static_cast<int>(i)] = left;
            if (block.right) block.right[offset + static_cast<int>(i)] = right;
            peak = std::max(peak, std::max(std::abs(left), std::abs(right)));
        }
    }
    fallbackPpq_ = transport.ppq + (transport.playing ? double(total) * transport.bpm / (60.0 * rate.load()) : 0);
    if (!currentInstrument) {
        session.status.peak.store(0);
        session.status.voices.store(0);
    }
    if (invalid) {
        // A ramp may contain finite but enormous samples before it overflows.
        // Reject the whole callback and discard the routing correction as well.
        for (int i = 0; i < total; ++i) {
            block.left[i] = 0;
            if (block.right) block.right[i] = 0;
        }
        peak = 0;
        sequenceRouting_ = {};
        panic.store(true);
        sequencePanic.store(true);
        session.requestPanic();
        sequenceSession.requestPanic();
        session.status.errors.fetch_add(1);
    }
    mixedPeak.store(peak);
    mixedVoices.store((currentInstrument ? session.status.voices.load() : 0) +
                      (mixSequence ? sequenceSession.status.voices.load() : 0));
    rawHostPoints_ = false;
    hostPointCount_ = 0;
    if (clock_) {
        const auto elapsed = clock_() - started;
        session.status.cpu.store(static_cast<float>(100 * elapsed * rate.load() / total));
    }
}
}  // namespace flows::app
