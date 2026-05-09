#include "GPUParticleRenderer.h"

#include "../components/RenderComponents.h"
#include "../../core/Logger.h"
#include "particle_emit_spv.h"
#include "particle_sort_spv.h"
#include "particle_sort_bitonic_spv.h"

#include <algorithm>
#include <cstring>

namespace engine {

void GPUParticleRenderer::init(backend::IRenderDevice* device) {
    device_ = device;
    if (!device_) return;

    backend::BufferDesc bufferDesc{};

    // ── Particle pool ──
    bufferDesc.size = MAX_PARTICLES * sizeof(GPUParticle);
    bufferDesc.usage = backend::BufferUsage::Storage | backend::BufferUsage::Vertex;
    particleBuffer_ = device_->createBuffer(bufferDesc);

    // ── Alive index ──
    bufferDesc.size = MAX_PARTICLES * sizeof(uint32_t);
    bufferDesc.usage = backend::BufferUsage::Storage;
    aliveIndexBuffer_ = device_->createBuffer(bufferDesc);

    // ── Indirect draw args: 5 uints per emitter ──
    bufferDesc.size = MAX_EMITTERS * sizeof(uint32_t) * 5;
    bufferDesc.usage = backend::BufferUsage::Storage | backend::BufferUsage::Indirect;
    indirectArgsBuffer_ = device_->createBuffer(bufferDesc);

    // ── Emitter descriptor buffer ──
    bufferDesc.size = MAX_EMITTERS * sizeof(GPUEmitter);
    bufferDesc.usage = backend::BufferUsage::Storage;
    emitterBuffer_ = device_->createBuffer(bufferDesc);
    {
        // Init all emitter slots to inactive.
        std::vector<GPUEmitter> init(MAX_EMITTERS);
        std::memset(init.data(), 0, init.size() * sizeof(GPUEmitter));
        for (auto& e : init) em_setParticleCount(e, 0);
        device_->uploadToBuffer(emitterBuffer_, init.data(),
                                init.size() * sizeof(GPUEmitter), 0);
    }

    // ── Free list: N particle slots + 1 head ──
    bufferDesc.size = (MAX_PARTICLES + 1) * sizeof(uint32_t);
    bufferDesc.usage = backend::BufferUsage::Storage;
    freeListBuffer_ = device_->createBuffer(bufferDesc);
    {
        // Populate free list with all particle indices (0 → 1 → 2 → ... → SENTINEL).
        std::vector<uint32_t> freeInit(MAX_PARTICLES + 1);
        for (uint32_t i = 0; i < MAX_PARTICLES; ++i) {
            freeInit[i] = (i + 1 < MAX_PARTICLES) ? (i + 1) : 0xFFFFFFFFu;
        }
        freeInit[MAX_PARTICLES] = 0;  // head points to slot 0
        device_->uploadToBuffer(freeListBuffer_, freeInit.data(),
                                freeInit.size() * sizeof(uint32_t), 0);
    }

    // ── Emit pipeline (replaces old update pipeline) ──
    {
        backend::ComputePipelineDesc desc{};
        desc.spirvCode = particle_emit_spv;
        desc.spirvSize = particle_emit_spv_size;
        desc.entryPoint = "main";
        desc.threadCountX = EMIT_WORKGROUP_SIZE;
        desc.threadCountY = 1;
        desc.threadCountZ = 1;
        desc.numReadwriteStorageBuffers = 5;  // Particles, Alive, FreeList, DrawArgs, Emitters
        desc.numUniformBuffers = 1;
        emitPipeline_ = device_->createComputePipeline(desc);
    }

    // ── Sort pipeline (odd-even fallback) ──
    {
        backend::ComputePipelineDesc desc{};
        desc.spirvCode = particle_sort_spv;
        desc.spirvSize = particle_sort_spv_size;
        desc.entryPoint = "main";
        desc.threadCountX = SORT_WORKGROUP_SIZE;
        desc.threadCountY = 1;
        desc.threadCountZ = 1;
        desc.numReadonlyStorageBuffers = 2;
        desc.numReadwriteStorageBuffers = 1;
        desc.numUniformBuffers = 1;
        sortPipeline_ = device_->createComputePipeline(desc);
    }

    // ── Bitonic sort pipeline ──
    {
        backend::ComputePipelineDesc desc{};
        desc.spirvCode = particle_sort_bitonic_spv;
        desc.spirvSize = particle_sort_bitonic_spv_size;
        desc.entryPoint = "main";
        desc.threadCountX = BITONIC_SORT_WORKGROUP_SIZE;
        desc.threadCountY = 1;
        desc.threadCountZ = 1;
        desc.numReadonlyStorageBuffers = 2;
        desc.numReadwriteStorageBuffers = 1;
        desc.numUniformBuffers = 1;
        bitonicSortPipeline_ = device_->createComputePipeline(desc);
    }

    initialized_ = particleBuffer_.valid() &&
                   aliveIndexBuffer_.valid() &&
                   indirectArgsBuffer_.valid() &&
                   emitterBuffer_.valid() &&
                   freeListBuffer_.valid() &&
                   emitPipeline_.valid();
    if (initialized_) {
        core::logInfo("GPUParticleRenderer initialized (GPU self-emission, %u max emitters)", MAX_EMITTERS);
    } else {
        core::logWarn("GPUParticleRenderer disabled");
    }
}

void GPUParticleRenderer::shutdown() {
    if (!device_) return;

    auto destroy = [&](auto& h) { if (h.valid()) device_->destroyComputePipeline(h); h = {}; };
    destroy(emitPipeline_);
    destroy(sortPipeline_);
    destroy(bitonicSortPipeline_);

    auto destroyBuf = [&](auto& h) { if (h.valid()) device_->destroyBuffer(h); h = {}; };
    destroyBuf(particleBuffer_);
    destroyBuf(aliveIndexBuffer_);
    destroyBuf(indirectArgsBuffer_);
    destroyBuf(emitterBuffer_);
    destroyBuf(freeListBuffer_);

    particleOffset_ = 0;
    emitterCount_ = 0;
    initialized_ = false;
}

uint32_t GPUParticleRenderer::allocateParticleRange(uint32_t count) {
    if (count > MAX_PARTICLES) count = MAX_PARTICLES;
    if (particleOffset_ + count > MAX_PARTICLES) {
        core::logWarn("GPUParticleRenderer: pool exhausted, wrapping");
        particleOffset_ = 0;
    }
    uint32_t out = particleOffset_;
    particleOffset_ += count;
    return out;
}

void GPUParticleRenderer::uploadEmitterConfig(uint32_t idx,
                                               const ParticleComponent& pc,
                                               backend::IRenderDevice& device) {
    GPUEmitter e{};
    em_setFirstParticle(e, pc.gpuOffset);
    em_setParticleCount(e, pc.gpuCount);
    em_setEmissionRate(e, pc.emissionRate);
    em_setLifetime(e, pc.lifetime);
    em_setSpeedMin(e, pc.speedMin);
    em_setSpeedMax(e, pc.speedMax);
    em_setSizeStart(e, pc.sizeStart);
    em_setSizeEnd(e, pc.sizeEnd);
    em_setSpread(e, pc.spread);
    em_setLayer(e, static_cast<uint32_t>(pc.layer));
    em_setSortKey(e, pc.sortOrder);

    packParticleColor(e.colorStart, pc.colorStart.r, pc.colorStart.g,
                      pc.colorStart.b, pc.colorStart.a);
    packParticleColor(e.colorEnd, pc.colorEnd.r, pc.colorEnd.g,
                      pc.colorEnd.b, pc.colorEnd.a);

    int texW = 1, texH = 1;
    device.getTextureDimensions(pc.texture, texW, texH);
    e.uvRect[0] = pc.srcRect.x / static_cast<float>(texW);
    e.uvRect[1] = pc.srcRect.y / static_cast<float>(texH);
    e.uvRect[2] = (pc.srcRect.x + pc.srcRect.w) / static_cast<float>(texW);
    e.uvRect[3] = (pc.srcRect.y + pc.srcRect.h) / static_cast<float>(texH);

    em_setTextureIndex(e, pc.texture.index);

    uint32_t flags = static_cast<uint32_t>(pc.pass) | sortModeBits(pc.sortMode);
    em_setFlags(e, flags);

    // GPU-side initial state
    em_setAccumulator(e, 0.f);
    em_setSeed(e, pc.seed);
    em_setWriteCursor(e, 0);

    device.uploadToBuffer(emitterBuffer_, &e, sizeof(GPUEmitter),
                          idx * sizeof(GPUEmitter));
}

uint32_t GPUParticleRenderer::registerEmitter(ParticleComponent& pc) {
    if (!initialized_ || emitterCount_ >= MAX_EMITTERS) return 0xFFFFFFFFu;

    // Check if already registered
    if (pc.gpuEmitterIndex != 0xFFFFFFFFu) return pc.gpuEmitterIndex;

    pc.gpuCount = std::max(1u, std::min(pc.maxParticles, MAX_PARTICLES));
    pc.gpuOffset = allocateParticleRange(pc.gpuCount);
    pc.gpuEmitterIndex = emitterCount_++;

    uploadEmitterConfig(pc.gpuEmitterIndex, pc, *device_);
    return pc.gpuEmitterIndex;
}

void GPUParticleRenderer::syncEmitterPosition(uint32_t emitterIdx,
                                              const Transform& tf,
                                              backend::IRenderDevice& device) {
    if (!initialized_ || emitterIdx >= emitterCount_) return;

    // Only need to write pos_rot[0..2] — position and rotation.
    // The GPU reads firstParticle from slot[3] so we must preserve it.
    GPUEmitter e{};  // Only fields [0..2] matter; rest is never read on this path.
    em_setPosX(e, tf.x);
    em_setPosY(e, tf.y);
    em_setRotation(e, tf.rotation);

    const size_t offset = emitterIdx * sizeof(GPUEmitter);
    device.uploadToBuffer(emitterBuffer_, &e, 3 * sizeof(float), offset);
}

} // namespace engine
