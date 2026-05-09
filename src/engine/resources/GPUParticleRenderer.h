#pragma once

#include "../components/ParticleComponent.h"
#include "GPUParticle.h"
#include "../../backend/renderer/IRenderDevice.h"
#include <vector>

namespace engine {

struct Transform;

// 全局 GPU 粒子系统。
//
// 管理：
//   - 粒子数据池         Particles[MAX_PARTICLES]  (storage + vertex)
//   - 活粒子压缩索引      AliveIndices[]             (storage)
//   - 间接绘制参数        IndirectArgs[]             (storage + indirect)
//   - 发射器描述符池      Emitters[MAX_EMITTERS]     (storage)
//   - 空闲槽位链表        FreeList[MAX_PARTICLES+1]   (storage)
//
// compute pipeline:
//   - emitPipeline_  粒子发射 + 推进 + 紧凑（单次 dispatch 处理所有发射器）
//   - sortPipeline_  排序（odd-even, 备用）
//   - bitonicSortPipeline_  排序（bitonic, ≤256 粒子）
class GPUParticleRenderer {
public:
    static constexpr uint32_t MAX_PARTICLES = 65536;
    static constexpr uint32_t MAX_EMITTERS  = 128;
    static constexpr uint32_t EMIT_WORKGROUP_SIZE = 64;
    static constexpr uint32_t SORT_WORKGROUP_SIZE = 128;
    static constexpr uint32_t BITONIC_SORT_WORKGROUP_SIZE = 256;

    void init(backend::IRenderDevice* device);
    void shutdown();

    bool isInitialized() const { return initialized_; }
    bool hasEmitPipeline() const { return emitPipeline_.valid(); }

    // ── 发射器登记 ──
    // 分配 emitter buffer 槽位并上传配置。返回槽位索引。
    // 若 emitter 已登记且配置未变，不重复上传。
    uint32_t registerEmitter(ParticleComponent& pc);

    // ── 每帧同步 ──
    // 仅上传发射器位置 / 旋转（变化字段），不触碰配置参数。
    void syncEmitterPosition(uint32_t emitterIdx, const Transform& tf,
                             backend::IRenderDevice& device);

    // ── 访问器 ──
    BufferHandle particleBuffer()     const { return particleBuffer_; }
    BufferHandle aliveIndexBuffer()   const { return aliveIndexBuffer_; }
    BufferHandle indirectArgsBuffer() const { return indirectArgsBuffer_; }
    BufferHandle emitterBuffer()      const { return emitterBuffer_; }
    BufferHandle freeListBuffer()     const { return freeListBuffer_; }
    ComputePipelineHandle emitPipeline()    const { return emitPipeline_; }
    ComputePipelineHandle sortPipeline()    const { return sortPipeline_; }
    ComputePipelineHandle bitonicSortPipeline() const { return bitonicSortPipeline_; }
    uint32_t emitterCount() const { return emitterCount_; }

private:
    uint32_t allocateParticleRange(uint32_t count);
    void uploadEmitterConfig(uint32_t idx, const ParticleComponent& pc,
                             backend::IRenderDevice& device);

    backend::IRenderDevice* device_ = nullptr;
    bool initialized_ = false;

    BufferHandle particleBuffer_;
    BufferHandle aliveIndexBuffer_;
    BufferHandle indirectArgsBuffer_;
    BufferHandle emitterBuffer_;
    BufferHandle freeListBuffer_;
    ComputePipelineHandle emitPipeline_;
    ComputePipelineHandle sortPipeline_;
    ComputePipelineHandle bitonicSortPipeline_;

    uint32_t particleOffset_ = 0;      // particle pool linear allocator
    uint32_t emitterCount_ = 0;        // tracked separately from buffer
};

} // namespace engine
