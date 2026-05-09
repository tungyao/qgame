#include "GPUParticleRenderer.h"

#include "../components/RenderComponents.h"
#include "../../core/Logger.h"
#include "particle_update_spv.h"
#include "particle_sort_spv.h"
#include "particle_sort_bitonic_spv.h"

#include <algorithm>
#include <cmath>

namespace engine {

void GPUParticleRenderer::init(backend::IRenderDevice* device) {
    device_ = device;
    if (!device_) return;

    backend::BufferDesc bufferDesc{};
    bufferDesc.size = MAX_PARTICLES * sizeof(GPUParticle);
    bufferDesc.usage = backend::BufferUsage::Storage | backend::BufferUsage::Vertex;
    particleBuffer_ = device_->createBuffer(bufferDesc);

    bufferDesc.size = MAX_PARTICLES * sizeof(uint32_t);
    bufferDesc.usage = backend::BufferUsage::Storage;
    aliveIndexBuffer_ = device_->createBuffer(bufferDesc);

    // SDL_GPUIndexedIndirectDrawCommand = 5 uint32-sized fields.
    // The update compute writes num_instances, and the render pass consumes it.
    bufferDesc.size = sizeof(uint32_t) * 5;
    bufferDesc.usage = backend::BufferUsage::Storage | backend::BufferUsage::Indirect;
    indirectArgsBuffer_ = device_->createBuffer(bufferDesc);

    backend::ComputePipelineDesc pipelineDesc{};
    pipelineDesc.spirvCode = particle_update_spv;
    pipelineDesc.spirvSize = particle_update_spv_size;
    pipelineDesc.entryPoint = "main";
    pipelineDesc.threadCountX = UPDATE_WORKGROUP_SIZE;
    pipelineDesc.threadCountY = 1;
    pipelineDesc.threadCountZ = 1;
    pipelineDesc.numReadwriteStorageBuffers = 3;
    pipelineDesc.numUniformBuffers = 1;
    updatePipeline_ = device_->createComputePipeline(pipelineDesc);

    backend::ComputePipelineDesc sortDesc{};
    sortDesc.spirvCode = particle_sort_spv;
    sortDesc.spirvSize = particle_sort_spv_size;
    sortDesc.entryPoint = "main";
    sortDesc.threadCountX = SORT_WORKGROUP_SIZE;
    sortDesc.threadCountY = 1;
    sortDesc.threadCountZ = 1;
    sortDesc.numReadonlyStorageBuffers = 2;
    sortDesc.numReadwriteStorageBuffers = 1;
    sortDesc.numUniformBuffers = 1;
    sortPipeline_ = device_->createComputePipeline(sortDesc);

    // Bitonic sort pipeline: single-pass sort for maxParticleCount ≤ 256.
    // Uses 256 threads × 1 workgroup with groupshared memory and
    // GroupMemoryBarrierWithGroupSync barriers between merge stages.
    backend::ComputePipelineDesc bitonicDesc{};
    bitonicDesc.spirvCode = particle_sort_bitonic_spv;
    bitonicDesc.spirvSize = particle_sort_bitonic_spv_size;
    bitonicDesc.entryPoint = "main";
    bitonicDesc.threadCountX = BITONIC_SORT_WORKGROUP_SIZE;
    bitonicDesc.threadCountY = 1;
    bitonicDesc.threadCountZ = 1;
    bitonicDesc.numReadonlyStorageBuffers = 2;
    bitonicDesc.numReadwriteStorageBuffers = 1;
    bitonicDesc.numUniformBuffers = 1;
    bitonicSortPipeline_ = device_->createComputePipeline(bitonicDesc);

    initialized_ = particleBuffer_.valid() &&
                   aliveIndexBuffer_.valid() &&
                   indirectArgsBuffer_.valid() &&
                   updatePipeline_.valid() &&
                   sortPipeline_.valid();
    if (initialized_) {
        core::logInfo("GPUParticleRenderer initialized");
    } else {
        core::logWarn("GPUParticleRenderer disabled: buffer or compute pipeline unavailable");
    }
}

void GPUParticleRenderer::shutdown() {
    if (!device_) return;

    if (updatePipeline_.valid()) {
        device_->destroyComputePipeline(updatePipeline_);
    }
    if (sortPipeline_.valid()) {
        device_->destroyComputePipeline(sortPipeline_);
    }
    if (bitonicSortPipeline_.valid()) {
        device_->destroyComputePipeline(bitonicSortPipeline_);
    }
    if (particleBuffer_.valid()) {
        device_->destroyBuffer(particleBuffer_);
    }
    if (aliveIndexBuffer_.valid()) {
        device_->destroyBuffer(aliveIndexBuffer_);
    }
    if (indirectArgsBuffer_.valid()) {
        device_->destroyBuffer(indirectArgsBuffer_);
    }

    updatePipeline_ = {};
    sortPipeline_ = {};
    bitonicSortPipeline_ = {};
    particleBuffer_ = {};
    aliveIndexBuffer_ = {};
    indirectArgsBuffer_ = {};
    nextOffset_ = 0;
    initialized_ = false;
}

void GPUParticleRenderer::ensureEmitter(ParticleComponent& pc) {
    if (!initialized_) return;
    if (pc.gpuOffset != 0xFFFFFFFFu && pc.gpuCount == pc.maxParticles) return;

    pc.gpuCount = std::max(1u, std::min(pc.maxParticles, MAX_PARTICLES));
    pc.gpuOffset = allocateRange(pc.gpuCount);

    // 新 range 默认写成 inactive，避免 GPU 第一次绘制未初始化数据。
    std::vector<GPUParticle> inactive(pc.gpuCount);
    for (GPUParticle& p : inactive) {
        p.posLife[2] = -1.f;
        p.posLife[3] = std::max(pc.lifetime, 0.001f);
    }
    device_->uploadToBuffer(particleBuffer_, inactive.data(),
                            inactive.size() * sizeof(GPUParticle),
                            pc.gpuOffset * sizeof(GPUParticle));
}

void GPUParticleRenderer::emit(ParticleComponent& pc, const Transform& tf,
                               float dt, backend::IRenderDevice& device) {
    if (!initialized_ || !pc.visible || !pc.playing || !pc.texture.valid()) return;
    ensureEmitter(pc);
    if (pc.gpuOffset == 0xFFFFFFFFu || pc.gpuCount == 0) return;

    pc.accumulator += std::max(0.f, dt) * std::max(0.f, pc.emissionRate);
    const uint32_t spawnCount = static_cast<uint32_t>(pc.accumulator);
    if (spawnCount == 0) return;
    pc.accumulator -= static_cast<float>(spawnCount);

    const uint32_t cappedSpawn = std::min(spawnCount, pc.gpuCount);
    std::vector<GPUParticle> spawned(cappedSpawn);
    for (uint32_t i = 0; i < cappedSpawn; ++i) {
        spawned[i] = makeParticle(pc, tf, device);
    }

    // Batch upload all spawned particles in at most two contiguous transfers.
    // The emitter operates as a ring buffer: new particles are written
    // sequentially starting at a write cursor; when the cursor reaches the
    // end of the range it wraps. This avoids the per-particle
    // SDL_AcquireGPUCommandBuffer / SDL_SubmitGPUCommandBuffer overhead that
    // scattered round-robin writes would incur.
    const size_t base = static_cast<size_t>(pc.gpuOffset) * sizeof(GPUParticle);
    const size_t rangeBytes = static_cast<size_t>(pc.gpuCount) * sizeof(GPUParticle);
    const size_t chunkBytes = static_cast<size_t>(cappedSpawn) * sizeof(GPUParticle);
    const size_t writeStart = base + (static_cast<size_t>(pc.seed) * sizeof(GPUParticle)) % rangeBytes;
    const size_t chunk1 = std::min(rangeBytes - (writeStart - base), chunkBytes);

    device_->uploadToBuffer(particleBuffer_, spawned.data(), chunk1, writeStart);

    if (chunk1 < chunkBytes) {
        const size_t remaining = chunkBytes - chunk1;
        device_->uploadToBuffer(particleBuffer_,
                                spawned.data() + chunk1 / sizeof(GPUParticle),
                                remaining, base);
    }

    pc.seed += cappedSpawn;
}

uint32_t GPUParticleRenderer::allocateRange(uint32_t count) {
    if (count > MAX_PARTICLES) count = MAX_PARTICLES;
    if (nextOffset_ + count > MAX_PARTICLES) {
        core::logWarn("GPUParticleRenderer: particle pool exhausted, wrapping allocations");
        nextOffset_ = 0;
    }

    const uint32_t out = nextOffset_;
    nextOffset_ += count;
    return out;
}

GPUParticle GPUParticleRenderer::makeParticle(const ParticleComponent& pc,
                                              const Transform& tf,
                                              backend::IRenderDevice& device) {
    uint32_t seed = pc.seed * 747796405u + 2891336453u;
    const float angleJitter = (rand01(seed) - 0.5f) * std::max(0.f, pc.spread);
    const float angle = tf.rotation + angleJitter;
    const float speed = pc.speedMin + (pc.speedMax - pc.speedMin) * rand01(seed);

    GPUParticle p{};
    p.posLife[0] = tf.x;
    p.posLife[1] = tf.y;
    p.posLife[2] = 0.f;
    p.posLife[3] = std::max(pc.lifetime, 0.001f);

    p.velSize[0] = std::cos(angle) * speed;
    p.velSize[1] = std::sin(angle) * speed;
    p.velSize[2] = pc.sizeStart;
    p.velSize[3] = pc.sizeEnd;

    packParticleColor(p.color0, pc.colorStart.r, pc.colorStart.g,
                      pc.colorStart.b, pc.colorStart.a);
    packParticleColor(p.color1, pc.colorEnd.r, pc.colorEnd.g,
                      pc.colorEnd.b, pc.colorEnd.a);

    int texW = 1, texH = 1;
    device.getTextureDimensions(pc.texture, texW, texH);
    p.uv[0] = pc.srcRect.x / static_cast<float>(texW);
    p.uv[1] = pc.srcRect.y / static_cast<float>(texH);
    p.uv[2] = (pc.srcRect.x + pc.srcRect.w) / static_cast<float>(texW);
    p.uv[3] = (pc.srcRect.y + pc.srcRect.h) / static_cast<float>(texH);

    p.textureIndex = pc.texture.index;
    p.layer = static_cast<uint32_t>(pc.layer);
    p.sortKey = pc.sortOrder;
    p.flags = static_cast<uint32_t>(pc.pass) | (pc.ySort ? (1u << 8) : 0u);
    return p;
}

float GPUParticleRenderer::rand01(uint32_t& state) {
    // 小型 LCG 足够用于发射角度/速度抖动；粒子确定性来自组件 seed。
    state = state * 1664525u + 1013904223u;
    return static_cast<float>((state >> 8) & 0x00FFFFFFu) / 16777215.0f;
}

} // namespace engine
