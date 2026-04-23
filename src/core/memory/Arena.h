#pragma once
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include "../Assert.h"

namespace core {

// 帧内线性分配器 — 帧末调用 reset() 一次性释放
class Arena {
public:
    explicit Arena(size_t capacity) : capacity_(capacity) {
        buf_ = static_cast<uint8_t*>(::malloc(capacity));
        ASSERT(buf_);
    }
    ~Arena() { ::free(buf_); }

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    void* alloc(size_t size, size_t align = alignof(std::max_align_t)) {
        size_t offset = (offset_ + align - 1) & ~(align - 1);
        ASSERT_MSG(offset + size <= capacity_, "Arena out of memory");
        void* ptr = buf_ + offset;
        offset_ = offset + size;
        return ptr;
    }

    template<typename T, typename... Args>
    T* create(Args&&... args) {
        void* mem = alloc(sizeof(T), alignof(T));
        return new(mem) T(static_cast<Args&&>(args)...);
    }

    void reset() { offset_ = 0; }

    size_t used()      const { return offset_; }
    size_t capacity()  const { return capacity_; }

private:
    uint8_t* buf_      = nullptr;
    size_t   offset_   = 0;
    size_t   capacity_ = 0;
};

} // namespace core
