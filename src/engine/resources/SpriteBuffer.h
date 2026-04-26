#pragma once
#include "GPUSprite.h"
#include "../../backend/shared/ResourceHandle.h"
#include "../../backend/renderer/IRenderDevice.h"
#include <vector>
#include <cstring>
#include <algorithm>

namespace engine {

class SpriteBuffer {
public:
    static constexpr uint32_t FRAME_COUNT = 3;
    static constexpr uint32_t INITIAL_CAPACITY = 1024;

    struct UpdateRange {
        size_t offset;
        size_t size;
        std::vector<uint8_t> data;
    };

    SpriteBuffer() = default;

    void init(backend::IRenderDevice* device, uint32_t initialCapacity = INITIAL_CAPACITY) {
        device_ = device;
        capacity_ = initialCapacity;
        activeCount_ = 0;

        for (uint32_t i = 0; i < FRAME_COUNT; ++i) {
            backend::BufferDesc desc{};
            desc.size = capacity_ * sizeof(GPUSprite);
            desc.usage = backend::BufferUsage::Storage | backend::BufferUsage::Vertex;
            buffers_[i] = device_->createBuffer(desc);
        }

        generations_.resize(capacity_, 0);
        slots_.resize(capacity_);
        dirty_.resize(capacity_, 0);
        freeList_.reserve(capacity_);

        for (uint32_t i = capacity_; i > 0; --i) {
            freeList_.push_back(i - 1);
        }
    }

    void shutdown() {
        for (uint32_t i = 0; i < FRAME_COUNT; ++i) {
            if (buffers_[i].valid()) {
                device_->destroyBuffer(buffers_[i]);
            }
        }
    }

    GPUHandle allocate() {
        uint32_t index;

        if (freeList_.empty()) {
            grow(capacity_ * 2);
        }

        index = freeList_.back();
        freeList_.pop_back();

        generations_[index]++;
        activeCount_++;

        return GPUHandle{ index, generations_[index] };
    }

    void free(GPUHandle handle) {
        if (!validate(handle)) return;

        freeList_.push_back(handle.index);
        activeCount_--;
    }

    bool validate(GPUHandle handle) const {
        if (handle.index >= capacity_) return false;
        return generations_[handle.index] == handle.generation;
    }

    GPUSprite* getSlot(GPUHandle handle) {
        if (!validate(handle)) return nullptr;
        return &slots_[handle.index];
    }

    const GPUSprite* getSlot(GPUHandle handle) const {
        if (!validate(handle)) return nullptr;
        return &slots_[handle.index];
    }

    void markDirty(GPUHandle handle) {
        if (validate(handle)) {
            // 同一份修改要广播到所有 frame buffer，下次每个 buffer 第一次轮到时才会被刷写
            dirty_[handle.index] = static_cast<uint8_t>((1u << FRAME_COUNT) - 1u);
        }
    }

    void markAllDirty() {
        std::fill(dirty_.begin(), dirty_.end(),
                  static_cast<uint8_t>((1u << FRAME_COUNT) - 1u));
    }

    uint32_t activeCount() const { return activeCount_; }
    uint32_t capacity() const { return capacity_; }

    void advanceFrame() {
        frameIndex_ = (frameIndex_ + 1) % FRAME_COUNT;
    }

    BufferHandle currentBuffer() const {
        return buffers_[frameIndex_];
    }

    void uploadDirty() {
        const uint8_t bit = static_cast<uint8_t>(1u << frameIndex_);
        std::vector<UpdateRange> updates;
        updates.reserve(64);

        for (uint32_t i = 0; i < capacity_; ++i) {
            if ((dirty_[i] & bit) == 0 || generations_[i] == 0) continue;

            UpdateRange range;
            range.offset = i * sizeof(GPUSprite);
            range.size = sizeof(GPUSprite);
            range.data.resize(sizeof(GPUSprite));
            std::memcpy(range.data.data(), &slots_[i], sizeof(GPUSprite));
            updates.push_back(std::move(range));

            dirty_[i] &= static_cast<uint8_t>(~bit);
        }

        if (updates.empty()) return;

        coalesceAndUpload(updates);
    }

    void uploadAll() {
        BufferHandle buf = buffers_[frameIndex_];
        device_->uploadToBuffer(buf, slots_.data(), capacity_ * sizeof(GPUSprite), 0);
    }

    void debugDumpSlot(GPUHandle handle) const {
        if (!validate(handle)) {
            printf("Invalid handle\n");
            return;
        }
        const GPUSprite& s = slots_[handle.index];
        printf("Slot %u (gen %u):\n", handle.index, handle.generation);
        printf("  transform: [%.2f, %.2f, %.2f, %.2f, ...]\n",
               s.transform[0], s.transform[1], s.transform[2], s.transform[3]);
        printf("  color: [%.2f, %.2f, %.2f, %.2f]\n", s.color[0], s.color[1], s.color[2], s.color[3]);
        printf("  uv: [%.2f, %.2f, %.2f, %.2f]\n", s.uv[0], s.uv[1], s.uv[2], s.uv[3]);
        printf("  textureIndex: %u, layer: %u, sortKey: %d\n", s.textureIndex, s.layer, s.sortKey);
    }

    uint32_t dirtyCount() const {
        const uint8_t bit = static_cast<uint8_t>(1u << frameIndex_);
        uint32_t count = 0;
        for (uint8_t d : dirty_) if (d & bit) count++;
        return count;
    }

private:
    void grow(uint32_t newCapacity) {
        std::vector<uint32_t> newGenerations(newCapacity, 0);
        std::vector<GPUSprite> newSlots(newCapacity);
        std::vector<uint8_t> newDirty(newCapacity, 0);

        for (uint32_t i = 0; i < capacity_; ++i) {
            newGenerations[i] = generations_[i];
            newSlots[i] = slots_[i];
            newDirty[i] = dirty_[i];
        }

        for (uint32_t i = newCapacity - 1; i >= capacity_; --i) {
            freeList_.push_back(i);
        }

        generations_ = std::move(newGenerations);
        slots_ = std::move(newSlots);
        dirty_ = std::move(newDirty);
        capacity_ = newCapacity;

        for (uint32_t i = 0; i < FRAME_COUNT; ++i) {
            device_->destroyBuffer(buffers_[i]);

            backend::BufferDesc desc{};
            desc.size = capacity_ * sizeof(GPUSprite);
            desc.usage = backend::BufferUsage::Storage | backend::BufferUsage::Vertex;
            buffers_[i] = device_->createBuffer(desc);
        }

        std::fill(dirty_.begin(), dirty_.end(),
                  static_cast<uint8_t>((1u << FRAME_COUNT) - 1u));
    }

    void coalesceAndUpload(std::vector<UpdateRange>& ranges) {
        if (ranges.empty()) return;

        std::sort(ranges.begin(), ranges.end(),
                  [](const UpdateRange& a, const UpdateRange& b) {
                      return a.offset < b.offset;
                  });

        std::vector<UpdateRange> merged;
        merged.push_back(std::move(ranges[0]));

        for (size_t i = 1; i < ranges.size(); ++i) {
            UpdateRange& last = merged.back();
            UpdateRange& curr = ranges[i];

            if (curr.offset <= last.offset + last.size + sizeof(GPUSprite) * 4) {
                size_t newEnd = std::max(last.offset + last.size, curr.offset + curr.size);
                last.size = newEnd - last.offset;

                if (curr.offset + curr.size > last.offset + last.data.size()) {
                    last.data.resize(newEnd - last.offset);
                }

                std::memcpy(last.data.data() + (curr.offset - last.offset),
                            curr.data.data(), curr.size);
            } else {
                merged.push_back(std::move(curr));
            }
        }

        BufferHandle buf = buffers_[frameIndex_];
        for (const UpdateRange& r : merged) {
            device_->uploadToBuffer(buf, r.data.data(), r.size, r.offset);
        }
    }

    backend::IRenderDevice* device_ = nullptr;

    BufferHandle buffers_[FRAME_COUNT];
    uint32_t frameIndex_ = 0;

    std::vector<uint32_t> freeList_;
    std::vector<uint32_t> generations_;
    std::vector<GPUSprite> slots_;
    std::vector<uint8_t> dirty_;

    uint32_t capacity_ = 0;
    uint32_t activeCount_ = 0;
};

}
