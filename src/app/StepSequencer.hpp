#pragma once
#include "engine/engine.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace flows::app {
inline constexpr std::size_t sequencerRows = 8, sequencerSteps = 32;
enum class StepDirection { forward, reverse, pingpong, random };
enum class StepScale { chromatic, major, minor, pentatonic };
struct SequenceStep { bool active = false; double velocity = .85, gate = .6; };
struct SequenceRow { int note = 30; bool mute = false; double volume = 0; std::array<SequenceStep,sequencerSteps> steps{}; };
struct MetronomeSettings { bool mute = true; int division = 4; double volume = -12; };
struct SequencePattern {
    MetronomeSettings metronome;
    int length = 16, division = 16, root = 0, transpose = 0;
    double swing = 0;
    StepDirection direction = StepDirection::forward;
    StepScale scale = StepScale::chromatic;
    std::size_t rowCount = 1;
    std::array<SequenceRow,sequencerRows> rows{};
};
inline SequencePattern rollingBassPattern() {
    SequencePattern pattern;
    for (std::size_t i=0;i<sequencerSteps;++i) pattern.rows[0].steps[i].active=i%4!=0;
    return pattern;
}
inline int sequencePitch(const SequencePattern& pattern, int note) noexcept {
    note=std::clamp(note+pattern.transpose,0,127);
    if (pattern.scale==StepScale::chromatic) return note;
    constexpr std::array<int,7> major{0,2,4,5,7,9,11}, minor{0,2,3,5,7,8,10};
    constexpr std::array<int,5> pentatonic{0,2,4,7,9};
    const auto contains=[&](int candidate) {
        const int interval=(candidate-pattern.root+132)%12;
        if (pattern.scale==StepScale::pentatonic) return std::find(pentatonic.begin(),pentatonic.end(),interval)!=pentatonic.end();
        const auto& scale=pattern.scale==StepScale::major ? major : minor;
        return std::find(scale.begin(),scale.end(),interval)!=scale.end();
    };
    // Resolve equal distances downward; never emit a pitch outside MIDI range.
    for (int distance=0;distance<12;++distance) {
        if (note-distance>=0 && contains(note-distance)) return note-distance;
        if (note+distance<=127 && contains(note+distance)) return note+distance;
    }
    return note;
}

// Audio-thread-owned scheduler. Pattern replacement is a fixed-size copy; no
// dynamic storage, locks, or unbounded catch-up loops occur while processing.
class StepSequencer {
    struct Held { std::uint64_t id=0; int pitch=0; double off=0; };
    SequencePattern pattern_=rollingBassPattern();
    std::array<Held,sequencerRows> held_{};
    std::uint64_t tick_=0, identity_=0;
    double origin_=0, expectedBeat_=0;
    bool running_=false, changed_=false;
    std::array<bool,sequencerRows> releaseRows_{};
    int playhead_=-1;
    std::uint32_t random_=0x6d2b79f5;
    double beat(std::uint64_t tick) const noexcept {
        return origin_+(static_cast<double>(tick)+(tick%2 ? pattern_.swing : 0))*4.0/pattern_.division;
    }
    int stepIndex() noexcept {
        const auto length=static_cast<std::uint64_t>(pattern_.length);
        if (pattern_.direction==StepDirection::reverse) return static_cast<int>(length-1-tick_%length);
        if (pattern_.direction==StepDirection::pingpong) {
            const auto position=tick_%(2*(length-1));
            return static_cast<int>(position<length ? position : 2*(length-1)-position);
        }
        if (pattern_.direction==StepDirection::random) {
            random_^=random_<<13; random_^=random_>>17; random_^=random_<<5;
            return static_cast<int>(random_%length);
        }
        return static_cast<int>(tick_%length);
    }
    template<class Emit> bool release(std::uint64_t clock, Emit& emit) noexcept {
        bool success=true;
        for (auto& held:held_) if (held.id) {
            if (!emit(Event{clock,EventType::note_off,held.id,static_cast<double>(held.pitch),0})) success=false;
            held={};
        }
        return success;
    }
public:
    void configure(const SequencePattern& pattern) noexcept {
        // Note/velocity/gate edits affect future triggers without moving the
        // clock. Removed or muted lanes release their own held notes only.
        changed_=changed_ || pattern.length!=pattern_.length || pattern.division!=pattern_.division || pattern.direction!=pattern_.direction;
        for (std::size_t row=0;row<sequencerRows;++row)
            releaseRows_[row]=releaseRows_[row] || row>=pattern.rowCount || pattern.rows[row].mute
                || (pattern.rowCount<pattern_.rowCount && pattern.rows[row].note!=pattern_.rows[row].note);
        pattern_=pattern;
    }
    // The caller must also panic the engine before discarding held identities.
    void reset() noexcept { held_={}; releaseRows_={}; running_=false; playhead_=-1; }
    const SequencePattern& pattern() const noexcept { return pattern_; }
    int playhead() const noexcept { return playhead_; }
    template<class Emit> bool stop(std::uint64_t clock,Emit emit) noexcept {
        const bool success=release(clock,emit);reset();changed_=false;return success;
    }
    template<class Emit> bool process(bool enabled, std::uint64_t clock, std::uint32_t frames,
                                      double rate, const Transport& transport, Emit emit) noexcept {
        if (!frames) return true;
        if (!(rate>0) || !std::isfinite(rate) || !(transport.bpm>0) || !std::isfinite(transport.bpm) || !std::isfinite(transport.ppq)) return false;
        const double samplesPerBeat=rate*60.0/transport.bpm;
        if (!enabled || !transport.playing) {
            const bool success=release(clock,emit); reset(); changed_=false; return success;
        }
        // Hosts may locate without sample-position metadata. A beat jump must
        // release old notes, rather than trying to replay the skipped interval.
        const bool discontinuity=running_ && std::abs(transport.ppq-expectedBeat_)>2.0/samplesPerBeat;
        if (changed_ || discontinuity) { if (!release(clock,emit)) return false; reset(); }
        changed_=false;
        for (std::size_t row=0;row<sequencerRows;++row) if (releaseRows_[row]) {
            if (held_[row].id && !emit(Event{clock,EventType::note_off,held_[row].id,static_cast<double>(held_[row].pitch),0})) return false;
            held_[row]={}; releaseRows_[row]=false;
        }
        if (!running_) { origin_=transport.ppq; tick_=0; random_=0x6d2b79f5; running_=true; }
        expectedBeat_=transport.ppq+frames/samplesPerBeat;
        const auto sampleFor=[&](double position) {
            const double relative=(position-transport.ppq)*samplesPerBeat;
            if (!std::isfinite(relative) || relative>=static_cast<double>(frames)+1) return clock+frames;
            if (relative<=0) return clock;
            return clock+static_cast<std::uint64_t>(std::llround(relative));
        };
        // At most 256 steps/releases even if a broken host provides an enormous
        // block or tempo. The caller converts false to an explicit voice reset.
        for (std::size_t count=0;count<256;++count) {
            const auto nextBeat=beat(tick_);
            const auto nextSample=sampleFor(nextBeat);
            Held* first=nullptr;
            for (auto& held:held_) if (held.id && (!first || held.off<first->off)) first=&held;
            if (first && first->off<=nextBeat) {
                const auto offSample=sampleFor(first->off);
                if (offSample>=clock+frames) return true;
                if (!emit(Event{offSample,EventType::note_off,first->id,static_cast<double>(first->pitch),0})) return false;
                *first={}; continue;
            }
            if (nextSample>=clock+frames) return true;
            playhead_=stepIndex();
            const double duration=beat(tick_+1)-nextBeat;
            for (std::size_t row=0;row<pattern_.rowCount;++row) {
                const auto& lane=pattern_.rows[row]; const auto& step=lane.steps[static_cast<std::size_t>(playhead_)];
                if (lane.mute || !step.active || step.velocity<=0) continue;
                // A swing edit can shorten the next interval beneath a note's
                // old gate. Never overwrite an identity before releasing it.
                if (held_[row].id && !emit(Event{nextSample,EventType::note_off,held_[row].id,static_cast<double>(held_[row].pitch),0})) return false;
                const auto id=(1ull<<61)|(static_cast<std::uint64_t>(row)<<56)|(++identity_ & ((1ull<<56)-1));
                const int pitch=sequencePitch(pattern_,lane.note);
                if (!emit(Event{nextSample,EventType::note_on,id,static_cast<double>(pitch),step.velocity})) return false;
                held_[row]={id,pitch,nextBeat+duration*step.gate};
            }
            ++tick_;
        }
        return false;
    }
};
}
