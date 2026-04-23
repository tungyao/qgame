#pragma once
#include <cstddef>
#include <cstdlib>
#include "../Assert.h"

// MSVC 没有 aligned_alloc，统一用平台宏封装
#if defined(_MSC_VER)
#  include <malloc.h>
#  define POOL_ALIGNED_ALLOC(align, size) ::_aligned_malloc((size), (align))
#  define POOL_ALIGNED_FREE(ptr)          ::_aligned_free((ptr))
#else
#  define POOL_ALIGNED_ALLOC(align, size) ::aligned_alloc((align), (size))
#  define POOL_ALIGNED_FREE(ptr)          ::free((ptr))
#endif

namespace core {

// 固定大小对象池 — O(1) alloc/free，用于 Component 密集分配
template<size_t ObjectSize, size_t Align = alignof(std::max_align_t)>
class PoolAllocator {
    static_assert(ObjectSize >= sizeof(void*), "ObjectSize must fit a pointer");
public:
    explicit PoolAllocator(size_t capacity) : capacity_(capacity) {
        size_t stride = (ObjectSize + Align - 1) & ~(Align - 1);
        stride_ = stride;
        buf_ = static_cast<uint8_t*>(POOL_ALIGNED_ALLOC(Align, stride * capacity));
        ASSERT(buf_);
        // 初始化 free list
        for (size_t i = 0; i < capacity - 1; ++i)
            *reinterpret_cast<void**>(buf_ + i * stride) = buf_ + (i + 1) * stride;
        *reinterpret_cast<void**>(buf_ + (capacity - 1) * stride) = nullptr;
        freeHead_ = buf_;
    }

    ~PoolAllocator() { POOL_ALIGNED_FREE(buf_); }

    PoolAllocator(const PoolAllocator&) = delete;
    PoolAllocator& operator=(const PoolAllocator&) = delete;

    void* alloc() {
        ASSERT_MSG(freeHead_, "PoolAllocator exhausted");
        void* ptr = freeHead_;
        freeHead_ = *reinterpret_cast<void**>(freeHead_);
        ++used_;
        return ptr;
    }

    void free(void* ptr) {
        ASSERT(ptr);
        *reinterpret_cast<void**>(ptr) = freeHead_;
        freeHead_ = ptr;
        --used_;
    }

    template<typename T, typename... Args>
    T* create(Args&&... args) {
        static_assert(sizeof(T) <= ObjectSize && alignof(T) <= Align);
        return new(alloc()) T(static_cast<Args&&>(args)...);
    }

    template<typename T>
    void destroy(T* ptr) {
        ptr->~T();
        free(ptr);
    }

    size_t used()     const { return used_; }
    size_t capacity() const { return capacity_; }

private:
    uint8_t* buf_      = nullptr;
    void*    freeHead_ = nullptr;
    size_t   capacity_ = 0;
    size_t   stride_   = 0;
    size_t   used_     = 0;
};

} // namespace core
