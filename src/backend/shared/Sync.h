#pragma once
#include <atomic>

namespace backend {

// CPU-GPU 帧同步 fence（轻量占位，bgfx 内部已管理）
struct Fence {
    std::atomic<uint64_t> value{0};
    void signal(uint64_t v) { value.store(v, std::memory_order_release); }
    bool reached(uint64_t v) const { return value.load(std::memory_order_acquire) >= v; }
};

} // namespace backend
