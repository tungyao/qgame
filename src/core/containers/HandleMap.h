#pragma once
#include <vector>
#include <cstdint>
#include "../Handle.h"
#include "../Assert.h"

namespace core {

// Handle → T 映射，支持版本号检测 use-after-free
template<typename HandleType, typename T>
class HandleMap {
    using IndexT = uint32_t;
public:
    explicit HandleMap(size_t capacity = 256) {
        slots_.reserve(capacity);
        // index 0 保留为无效槽
        slots_.push_back({});
    }

    HandleType insert(T value) {
        if (!freeList_.empty()) {
            IndexT idx = freeList_.back();
            freeList_.pop_back();
            slots_[idx].value   = std::move(value);
            slots_[idx].occupied = true;
            return HandleType{idx, slots_[idx].version};
        }
        IndexT idx = static_cast<IndexT>(slots_.size());
        slots_.push_back({std::move(value), 1, true});
        return HandleType{idx, 1};
    }

    void remove(HandleType h) {
        ASSERT(valid(h));
        Slot& s = slots_[h.index];
        s.occupied = false;
        ++s.version;
        freeList_.push_back(h.index);
    }

    bool valid(HandleType h) const {
        if (h.index == 0 || h.index >= slots_.size()) return false;
        const Slot& s = slots_[h.index];
        return s.occupied && s.version == h.version;
    }

    T& get(HandleType h) {
        ASSERT(valid(h));
        return slots_[h.index].value;
    }

    const T& get(HandleType h) const {
        ASSERT(valid(h));
        return slots_[h.index].value;
    }

    T* tryGet(HandleType h) {
        return valid(h) ? &slots_[h.index].value : nullptr;
    }

private:
    struct Slot {
        T        value;
        uint32_t version  = 0;
        bool     occupied = false;
    };
    std::vector<Slot>    slots_;
    std::vector<IndexT>  freeList_;
};

} // namespace core
