#pragma once
#include "RealtimeSession.hpp"
#include "StepSequencer.hpp"

namespace flows::app {
// One independent kernel per selected lane; storage is prepared off the audio
// thread. Legacy patterns keep their shared instrument until a lane is assigned.
class SequenceBank {
    struct Storage {
        std::array<RealtimeSession,sequencerRows> sessions;
        std::array<Event,256> events{};
        std::array<float,maxFrames> left{},right{};
    };
    std::unique_ptr<Storage> storage_=std::make_unique<Storage>();
    std::atomic<std::size_t> count_{1};
    std::atomic<bool> publishing_{false};
    std::size_t audioCount_=1;
public:
    SessionStatus status;
    void submit(std::vector<std::unique_ptr<PreparedProgram>> programs) {
        const auto count=programs.size();
        publishing_.store(true,std::memory_order_release);
        count_.store(count,std::memory_order_release);
        for(std::size_t i=0;i<count;++i)storage_->sessions[i].submit(std::move(programs[i]));
        publishing_.store(false,std::memory_order_release);
    }
    void requestPanic() noexcept { for(auto& s:storage_->sessions)s.requestPanic(); }
    void beginBlock() noexcept { audioCount_=count_.load(std::memory_order_acquire);for(auto& s:storage_->sessions)s.beginBlock(); }
    std::uint64_t targetRevision() const noexcept {
        if(publishing_.load(std::memory_order_acquire)||audioCount_!=count_.load(std::memory_order_acquire))return 0;
        auto revision=storage_->sessions[0].targetRevision();
        for(std::size_t i=1;i<audioCount_;++i)revision=std::min(revision,storage_->sessions[i].targetRevision());
        return revision;
    }
    void setOutputGains(std::span<const OutputGainGroup> groups) noexcept {
        for(std::size_t i=0;i<sequencerRows;++i)storage_->sessions[i].setOutputGains(groups,i<groups.size()?groups[i].gain:0);
    }
    void setTransport(Transport t) noexcept { for(auto& s:storage_->sessions)s.setTransport(t); }
    void processStereo(std::span<const Event> input,std::span<float> left,std::span<float> right,
                       std::span<const ParameterUpdate> = {},bool install=true) noexcept {
        std::fill(left.begin(),left.end(),0);std::fill(right.begin(),right.end(),0);
        const auto count=audioCount_;std::uint64_t errors=0;std::uint32_t voices=0;std::uint64_t revision=~std::uint64_t{0};
        for(std::size_t lane=0;lane<sequencerRows;++lane) {
            std::size_t n=0;
            for(auto e:input)if(lane<count&&(count==1||((e.note_id>>56)&7)==lane))storage_->events[n++]=e;
            auto l=std::span(storage_->left).first(left.size()),r=std::span(storage_->right).first(right.size());
            auto& session=storage_->sessions[lane];
            session.processStereo(std::span(storage_->events).first(n),l,r,{},install);
            errors+=session.status.errors.load();
            if(lane>=count)continue;
            for(std::size_t i=0;i<left.size();++i){left[i]+=l[i];right[i]+=r[i];}
            voices+=session.status.voices.load();revision=std::min(revision,session.status.revision.load());
        }
        status.voices.store(voices);status.revision.store(revision);status.errors.store(errors);
    }
    auto& frames() noexcept { return storage_->sessions[0].frames(); }
    void reap() { for(auto& s:storage_->sessions){while(s.frames().readSlot())s.frames().commitRead();s.reap();} }
};
}
