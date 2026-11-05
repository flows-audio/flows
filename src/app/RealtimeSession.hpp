#pragma once
#include "compiler/program.hpp"
#include "engine/engine.hpp"
#include "engine/sample.hpp"
#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace flows::app {

template<class T, std::size_t N> class SpscQueue {
    std::array<T, N> items_{};
    std::atomic<std::size_t> write_{0}, read_{0};
public:
    T* writeSlot() noexcept {
        auto w = write_.load(std::memory_order_relaxed);
        return w - read_.load(std::memory_order_acquire) < N ? &items_[w % N] : nullptr;
    }
    void commitWrite() noexcept { write_.fetch_add(1, std::memory_order_release); }
    const T* readSlot() noexcept {
        auto r = read_.load(std::memory_order_relaxed);
        return r != write_.load(std::memory_order_acquire) ? &items_[r % N] : nullptr;
    }
    void commitRead() noexcept { read_.fetch_add(1, std::memory_order_release); }
    bool push(const T& value) noexcept { if (auto* p = writeSlot()) { *p = value; commitWrite(); return true; } return false; }
    bool pop(T& value) noexcept { if (auto* p = readSlot()) { value = *p; commitRead(); return true; } return false; }
};

inline constexpr std::size_t captureChannels = 8;
inline constexpr std::size_t maxFrames = 1024;
enum class ProbeProjection { mid, left, right, side };
struct ProbeSelection {
    std::uint64_t revision = 0;
    std::array<ValueId, captureChannels> values{};
    std::array<bool, captureChannels> audio{};
    std::uint32_t count = 0;
    int voice = -1;
    ValueId audition = invalid_value;
    ProbeProjection projection = ProbeProjection::mid;
};
struct CaptureFrame {
    std::uint64_t revision = 0, generation = 0, parameterEpoch = 0, observationEpoch = 0;
    std::uint64_t start = 0, sequence = 0;
    double sampleRate = 48000;
    Transport transport;
    bool continuous = true;
    // A sounding voice includes its release/tail; raw mono kernels may run
    // outside this lifetime and must not wake an idle performance display.
    bool performanceActive = false;
    std::uint64_t keyEpoch = 0, keyStart = 0;
    std::uint32_t count = 0, channels = 0;
    bool transition = false;
    int voice = -1;
    std::uint64_t voiceGeneration = 0;
    ProbeProjection projection = ProbeProjection::mid;
    std::array<ValueId, captureChannels> values{};
    std::array<bool, captureChannels> audio{};
    std::array<int, captureChannels> channelVoices{};
    std::array<std::uint64_t, captureChannels> channelVoiceGenerations{};
    std::array<std::array<float, maxFrames>, captureChannels> samples{};
};
struct PreparedProgram {
    Program program;
    std::unique_ptr<Engine> engine;
    std::string source, structure;
    std::uint64_t revision = 0, generation = 0, documentGeneration = 0;
    double sampleRate = 48000;
    ProbeSelection probes;
    // Fresh engine: initial runtime snapshot before held replay. Retained engine:
    // identifies host-owned bindings whose ongoing values/ramps must be preserved.
    std::vector<ParameterUpdate> initialParameters;
    std::vector<bool> audioValues;
    PreparedProgram(Program p, std::string text, std::uint64_t rev, double rate, SampleBank samples = {});
};
struct SessionStatus {
    std::atomic<std::uint64_t> revision{0}, generation{0}, samples{0}, drops{0}, errors{0};
    std::atomic<std::uint32_t> voices{0};
    std::atomic<float> peak{0}, cpu{0};
    std::atomic<std::uint64_t> parameterEpoch{0}, observationEpoch{0};
};

class RealtimeSession final : private CaptureSink {
public:
    RealtimeSession();
    ~RealtimeSession();
    // One compile worker publishes; the audio callback consumes. Reap runs off audio.
    void submit(std::unique_ptr<PreparedProgram> prepared);
    void reap();
    // Caller must stop audio and the submit producer before shutdown/destruction.
    void shutdown();
    bool setProbes(ProbeSelection selection) noexcept;
    void requestPanic() noexcept { panicRequested_.store(true, std::memory_order_release); }
    // Audio thread only. Call before resolving host parameter bindings for this block.
    // Repeated calls are harmless; reserve is an upper bound on this block's host commands.
    void beginBlock(std::size_t parameterReserve = 0) noexcept;
    std::uint64_t targetRevision() const noexcept { return active_ ? active_->revision : 0; }
    void process(std::span<const Event> events, std::span<float> output,
                 std::span<const ParameterUpdate> parameters = {},
                 bool allowInstall = true) noexcept;
    void processStereo(std::span<const Event> events, std::span<float> left, std::span<float> right,
                       std::span<const ParameterUpdate> parameters = {}, bool allowInstall = true) noexcept;
    // Audio thread only; PPQ denotes the next block's first sample.
    void setTransport(Transport transport) noexcept;
    void setContinuousObservation(bool value) noexcept { continuousObservation_=value; }
    void setOutputGains(std::span<const OutputGainGroup> groups,float fallback=1) noexcept;
    SpscQueue<CaptureFrame, 128>& frames() noexcept { return *frames_; }
    SessionStatus status;
private:
    struct Held {
        bool used = false, accepted = false, resident = false;
        Event event;
        std::uint64_t order = 0;
        // EOF can finish a sampler while its physical key remains held. Keep
        // that key for note-off validation, without restarting it on rebuild.
        bool completed = false;
    };
    std::atomic<PreparedProgram*> pending_{nullptr};
    PreparedProgram* active_ = nullptr;
    PreparedProgram* outgoing_ = nullptr;
    std::unique_ptr<SpscQueue<CaptureFrame, 128>> frames_;
    SpscQueue<PreparedProgram*, 2> retired_;
    SpscQueue<ProbeSelection, 16> selections_;
    std::array<Held, 1024> held_{};
    std::array<std::uint64_t, 1024> shadowIds_{};
    std::array<bool, 1024> shadowUsed_{}, shadowResident_{};
    std::array<Event, 1024> replay_{};
    std::array<Event, 256> events_{};
    std::array<ParameterUpdate, 256> updates_{};
    std::array<float, maxFrames> oldAudio_{};
    std::array<float, maxFrames> auditionAudio_{}, outgoingAuditionAudio_{};
    std::array<float, maxFrames> oldAudioRight_{}, auditionAudioRight_{}, outgoingAuditionAudioRight_{}, legacyRight_{};
    std::unique_ptr<CaptureFrame> scratchFrame_;
    CaptureFrame* capturing_ = nullptr;
    std::uint64_t clock_ = 0, sequence_ = 0, order_ = 0, generation_ = 0;
    std::uint64_t captureEngineStart_ = 0, parameterEpoch_ = 0, observationEpoch_ = 0;
    std::size_t fadeRemaining_ = 0, fadeLength_ = 1;
    ProbeSelection probes_;
    ProbeSelection deferredSelection_;
    bool hasDeferredSelection_ = false;
    std::atomic<bool> panicRequested_{false};
    std::size_t authoredUpdates_ = 0;
    bool boundaryPrepared_ = false;
    Transport transport_{};
    bool haveTransport_ = false, continuousObservation_ = true;
    std::uint64_t keyEpoch_ = 0, keyStart_ = 0;
    std::array<OutputGainGroup,8> outputGroups_{};
    std::size_t outputGroupCount_ = 0;
    float defaultOutputGain_ = 1;
    ValueId auditionValue_ = invalid_value, auditionTarget_ = invalid_value, outgoingAuditionValue_ = invalid_value;
    int auditionVoice_ = -1, outgoingAuditionVoice_ = -1;
    double auditionMix_ = 0.0;
    bool capturingOutgoing_ = false;
    bool replayingHeld_ = false;
    std::uint64_t outgoingCaptureStart_ = 0;
    std::size_t outgoingCaptureCount_ = 0;
    void install(std::size_t parameterCapacity) noexcept;
    void select(ProbeSelection selection) noexcept;
    void finishSilent(std::size_t frames) noexcept;
    void advanceTransport(std::size_t frames) noexcept;
    void panic() noexcept;
    void capture(ValueId value, VoiceIdentity voice, std::uint64_t start,
                 std::span<const float> samples) noexcept override;
    void capture_stereo(ValueId value, VoiceIdentity voice, std::uint64_t start,
                        std::span<const float> left, std::span<const float> right) noexcept override;
    void event(const Event&, VoiceIdentity, std::string_view action) noexcept override;
};
} // namespace flows::app
