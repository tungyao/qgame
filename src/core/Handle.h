#pragma once
#include <cstdint>

namespace core {

// Versioned handle — 防悬挂指针
// index: 20 bits (最多 ~1M 对象), version: 12 bits
template<typename Tag, typename T = uint32_t>
struct Handle {
    static constexpr T INDEX_BITS   = 20;
    static constexpr T VERSION_BITS = 12;
    static constexpr T INDEX_MASK   = (T(1) << INDEX_BITS)   - 1;
    static constexpr T VERSION_MASK = (T(1) << VERSION_BITS) - 1;

    T index   : INDEX_BITS;
    T version : VERSION_BITS;

    Handle() : index(0), version(0) {}
    Handle(T idx, T ver) : index(idx & INDEX_MASK), version(ver & VERSION_MASK) {}

    bool valid() const { return index != 0; }
    bool operator==(const Handle& o) const { return index == o.index && version == o.version; }
    bool operator!=(const Handle& o) const { return !(*this == o); }
};

} // namespace core

// Convenience aliases (defined per-module where needed)
// using TextureHandle = core::Handle<struct TextureTag>;
// using SoundHandle   = core::Handle<struct SoundTag>;
// using EntityHandle  = core::Handle<struct EntityTag>;
