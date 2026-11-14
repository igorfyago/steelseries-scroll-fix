// ring.h : fixed size queue for exactly one writer thread and one reader
// thread. Neither side ever waits on a lock, which is what lets the mouse hook
// hand records to slower threads without being held up by them.
#pragma once
#include <atomic>
#include <cstdint>

namespace fbw {

template <typename T, uint32_t N>   // N must be a power of two
class Ring {
public:
    // Returns false when the queue is full; the item is then dropped.
    bool push(const T& v) {
        const uint32_t h = head_.load(std::memory_order_relaxed);
        const uint32_t next = (h + 1) & (N - 1);
        if (next == tail_.load(std::memory_order_acquire)) return false;
        buf_[h] = v;
        head_.store(next, std::memory_order_release);
        return true;
    }

    bool pop(T* out) {
        const uint32_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire)) return false;
        *out = buf_[t];
        tail_.store((t + 1) & (N - 1), std::memory_order_release);
        return true;
    }

private:
    T buf_[N];
    std::atomic<uint32_t> head_{0};
    std::atomic<uint32_t> tail_{0};
};

}  // namespace fbw
