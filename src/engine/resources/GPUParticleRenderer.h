#pragma once

#include "../components/ParticleComponent.h"
#include "GPUParticle.h"
#include "../../backend/renderer/IRenderDevice.h"
#include <vector>

namespace engine {

struct Transform;

// 管理全局 GPU 粒子池、alive index、indirect args 和 compute pipelines。
class GPUParticleRenderer {
public:
    static constexpr uint32_t MAX_PARTICLES = 65536;
    static constexpr uint32_t UPDATE_WORKGROUP_SIZE = 64;
    static constexpr uint32_t SORT_WORKGROUP_SIZE = 128;

    void init(backend::IRenderDevice* device);
    void shutdown();

    bool isInitialized() const { return initialized_; }
    bool hasUpdatePipeline() const { return updatePipeline_.valid(); }
    bool hasSortPipeline() const { return sortPipeline_.valid(); }

    void ensureEmitter(ParticleComponent& pc);
    void emit(ParticleComponent& pc, const Transform& tf, float dt,
              backend::IRenderDevice& device);

    BufferHandle particleBuffer() const { return particleBuffer_; }
    BufferHandle aliveIndexBuffer() const { return aliveIndexBuffer_; }
    BufferHandle indirectArgsBuffer() const { return indirectArgsBuffer_; }
    ComputePipelineHandle updatePipeline() const { return updatePipeline_; }
    ComputePipelineHandle sortPipeline() const { return sortPipeline_; }

private:
    uint32_t allocateRange(uint32_t count);
    GPUParticle makeParticle(const ParticleComponent& pc, const Transform& tf,
                             backend::IRenderDevice& device);
    float rand01(uint32_t& state);

    backend::IRenderDevice* device_ = nullptr;
    bool initialized_ = false;

    BufferHandle particleBuffer_;
    BufferHandle aliveIndexBuffer_;
    BufferHandle indirectArgsBuffer_;
    ComputePipelineHandle updatePipeline_;
    ComputePipelineHandle sortPipeline_;

    // 简单线性 allocator：第一版假设 emitter 数量稳定。组件销毁时不回收，
    // 避免为 MVP 引入额外 free-list 状态。
    uint32_t nextOffset_ = 0;
};

} // namespace engine
