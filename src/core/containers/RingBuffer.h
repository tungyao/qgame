#pragma once
#include <atomic>
#include <array>
#include "../Assert.h"

namespace core {

// 单生产者单消费者无锁环形队列 (SPSC)
// 适用于 AudioCommandQueue 主线程 push / AudioThread pop
template<typename T, size_t Capacity>
class RingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of two");
public:
    bool push(const T& item) {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t next = (head + 1) & MASK;
        if (next == tail_.load(std::memory_order_acquire))
            return false;  // full
        buf_[head] = item;
        head_.store(next, std::memory_order_release);
        return true;
    }

    bool pop(T& out) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire))
            return false;  // empty
        out = buf_[tail];
        tail_.store((tail + 1) & MASK, std::memory_order_release);
        return true;
    }

    bool empty() const {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

private:
    static constexpr size_t MASK = Capacity - 1;
    std::array<T, Capacity>  buf_;
    std::atomic<size_t>      head_{0};
    std::atomic<size_t>      tail_{0};
};

} // namespace core
