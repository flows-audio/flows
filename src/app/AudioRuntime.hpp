#pragma once
// The realtime half of the app core. One instance runs inside each host's audio
// callback: JUCE's processBlock on macOS, a Wasm Audio Worklet in the browser.
// Everything here is lock-free and allocation-free; the control side (AppCore)
// communicates through the atomics, queues and mailboxes below.
#include "HostAutomation.hpp"
#include "Metronome.hpp"
#include "RealtimeSession.hpp"
#include "SequenceBank.hpp"
#include "StepSequencer.hpp"
#include "engine/boundary_smoother.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <optional>
#include <span>

namespace flows::app {
// Host-owned normalized (0..1) macro parameters. Hosts expose them to DAWs
// (JUCE AudioParameterFloat) or keep them in memory (browser).
class MacroParameters {
public:
    virtual ~MacroParameters() = default;
    virtual float get(int slot) const noexcept = 0;  // audio-thread safe
    virtual void set(int slot, float normalized) = 0;  // control threads only
};
// Lock-free in-memory parameters for hosts without a DAW.
class AtomicMacroParameters final : public MacroParameters {
public:
    AtomicMacroParameters() {
        for (auto& value : values_) value.store(.5f);
    }
    float get(int slot) const noexcept override { return values_[static_cast<std::size_t>(slot)].load(std::memory_order_relaxed); }
    void set(int slot, float normalized) override { values_[static_cast<std::size_t>(slot)].store(normalized); }

private:
    std::array<std::atomic<float>, 32> values_;
};

struct RawMidi {
    const std::uint8_t* data = nullptr;
    int size = 0;
    int samplePosition = 0;
};
// Musical position reported by a plugin host. Standalone and browser hosts
// pass none: their transport is the internal tempo and local position.
struct HostPosition {
    bool playing = false;
    std::optional<double> bpm, ppq;
    std::optional<std::int64_t> timeInSamples;
};
struct AudioBlock {
    float* left = nullptr;
    float* right = nullptr;  // null for a mono output bus
    int frames = 0;
    std::span<const RawMidi> midi;
    const HostPosition* position = nullptr;
};

struct MacroBinding {
    ValueId value = invalid_value;
    double min = 0, max = 1;
    bool logarithmic = false;
};
struct MacroMap {
    std::uint64_t revision = 0;
    std::array<MacroBinding, 32> bindings{};
};
struct KeyboardEvent {
    Event event;
    std::uint64_t documentRevision = 0;
};

class AudioRuntime {
public:
    explicit AudioRuntime(MacroParameters& parameters) : parameters_(parameters) { previousMacro_.fill(.5f); }
    void prepare(double sampleRate, int blockSize) noexcept;
    void release() noexcept;
    // Renders one host callback. Output is always finite; invalid audio is
    // replaced by silence and resets the instruments.
    void process(const AudioBlock& block) noexcept;
    // Releases every note held by external MIDI input at the next block.
    void releaseMidiNotes() noexcept { midiRelease_.store(true); }
    // Host automation capture (JUCE wrapper adapters call these on the audio thread).
    void beginHostAutomation() noexcept;
    void hostParameter(int slot, int offset, float value) noexcept;
    void auParameter(int slot, int offset, unsigned int duration, float start, float end, bool ramp) noexcept;
    // High-resolution clock in seconds for the CPU meter (steady_clock by
    // default). Hosts whose audio thread lacks a clock can pass nullptr.
    void setClock(double (*clock)() noexcept) noexcept { clock_ = clock; }

    // Shared with the control side --------------------------------------------
    RealtimeSession session;
    SequenceBank sequenceSession;
    std::atomic<double> rate{48000};
    std::atomic<double> bpm{138}, reportedBpm{138};
    std::atomic<bool> hostTempo{false};
    std::atomic<int> blockSize{256};
    std::atomic<bool> running{false}, panic{false}, demo{false};
    // Like panic (voices, held MIDI and the free-running pattern reset at the
    // next block) but keeps UI notes queued after it: audio (re)starting must
    // not drop the key press that started it.
    std::atomic<bool> restart{false};
    std::atomic<std::uint64_t> hostOverflows{0};
    std::atomic<bool> sequencePinned{false}, sequencePanic{false}, sequenceAuditionMuted{false};
    std::atomic<bool> sequenceSourcePlayable{false};
    std::atomic<std::uint64_t> sequenceTargetRevision{1}, sequenceMinimumRevision{1};
    std::atomic<float> mixedPeak{0};
    std::atomic<std::uint32_t> mixedVoices{0};
    std::atomic<int> sequencePlayhead{-1};
    LatestMailbox<SequencePattern> sequencePatterns;
    LatestMailbox<MacroMap> macroMaps;
    SpscQueue<KeyboardEvent, 256> keyboard;
    std::atomic<std::uint64_t> audioDocumentRevision{1};
    // A failed draft may retain historical source, but never audible DSP.
    std::atomic<bool> sourcePlayable{false};

private:
    struct HostPoint {
        int slot = 0, offset = 0;
        float value = 0;
        unsigned int duration = 0;
        float start = 0;
        int kind = 0;
    };
    struct MidiHeld {
        bool used = false;
        int channel = 0, pitch = 0;
        std::uint64_t id = 0, order = 0;
    };
    MacroParameters& parameters_;
    static double steadySeconds() noexcept;
    double (*clock_)() noexcept = &AudioRuntime::steadySeconds;
    std::atomic<bool> midiRelease_{false};
    MacroMap macros_, waitingMacros_;
    std::array<float, 32> previousMacro_{};
    HostQueue<HostPoint, 256> auPoints_;
    std::atomic<bool> auOverflow_{false};
    std::array<HostPoint, 256> hostPoints_{};
    std::size_t hostPointCount_ = 0;
    bool rawHostPoints_ = false, hostPointOverflow_ = false;
    std::array<MidiHeld, 1024> midiHeld_{};
    std::uint64_t nextNote_ = 1, midiOrder_ = 0;
    std::array<Event, 256> blockEvents_{};
    std::array<ParameterUpdate, 256> blockParameters_{};
    std::array<float, maxFrames> mono_{}, right_{};
    std::array<Event, 256> sequenceEvents_{};
    std::array<float, maxFrames> sequenceLeft_{}, sequenceRight_{}, clickLeft_{}, clickRight_{};
    StepSequencer sequencer_;
    Metronome metronome_;
    bool sequenceAudioPinned_ = false, sequenceWasReady_ = true;
    BoundarySmoother sequenceRouting_;
    bool sequenceRouteEnabled_ = false;
    std::optional<std::int64_t> nextTransportSample_;
    double fallbackPpq_ = 0;
};
}  // namespace flows::app
