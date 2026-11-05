#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace flows::app {
// Single publisher / single consumer ownership exchange. The newest map replaces
// an unconsumed map, including while the audio device is stopped. Neither side
// ever writes the other side's currently owned slot.
template<class T> class LatestMailbox {
    std::array<T,3> slots_{};
    std::atomic<unsigned int> middle_{2};
    unsigned int producer_=0, consumer_=1;
public:
    void push(const T& value) noexcept {
        slots_[producer_]=value;
        producer_=middle_.exchange(producer_|4,std::memory_order_acq_rel)&3;
    }
    bool pop(T& value) noexcept {
        if((middle_.load(std::memory_order_acquire)&4)==0) return false;
        consumer_=middle_.exchange(consumer_,std::memory_order_acq_rel)&3;
        value=slots_[consumer_]; return true;
    }
};
// AU setters may run concurrently with rendering. Sequence-numbered cells keep
// the audio consumer lock-free; bounded producer retries turn contention into an
// explicit overflow/reset instead of blocking the audio callback.
template<class T, std::size_t Capacity> class HostQueue {
    struct Cell { std::atomic<std::size_t> sequence{0}; T value{}; };
    std::array<Cell,Capacity> cells_;
    std::atomic<std::size_t> enqueue_{0};
    std::size_t dequeue_ = 0;
public:
    HostQueue() { for (std::size_t i=0;i<Capacity;++i) cells_[i].sequence.store(i); }
    bool push(const T& value) noexcept {
        auto position = enqueue_.load(std::memory_order_relaxed);
        for (int attempt=0;attempt<8;++attempt) {
            auto& cell = cells_[position % Capacity];
            const auto sequence = cell.sequence.load(std::memory_order_acquire);
            const auto difference = static_cast<std::intptr_t>(sequence)-static_cast<std::intptr_t>(position);
            if (difference == 0) {
                if (enqueue_.compare_exchange_weak(position,position+1,std::memory_order_relaxed)) {
                    cell.value = value; cell.sequence.store(position+1,std::memory_order_release); return true;
                }
            } else if (difference < 0) return false;
            else position = enqueue_.load(std::memory_order_relaxed);
        }
        return false;
    }
    bool pop(T& value) noexcept {
        auto& cell = cells_[dequeue_ % Capacity];
        if (cell.sequence.load(std::memory_order_acquire) != dequeue_+1) return false;
        value = cell.value; cell.sequence.store(dequeue_+Capacity,std::memory_order_release); ++dequeue_; return true;
    }
    bool hasPending() const noexcept {
        return cells_[dequeue_%Capacity].sequence.load(std::memory_order_acquire)==dequeue_+1;
    }
};
}
