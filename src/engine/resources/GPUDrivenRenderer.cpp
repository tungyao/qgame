#include "GPUDrivenRenderer.h"
#include "../../core/Logger.h"
#include <cstring>
#include <cmath>

namespace engine {

namespace {
    const char* cullingShaderGLSL = R"GLSL(
#version 450
layout(local_size_x = 64) in;

struct GPUSprite {
    float transform[12];
    float color[4];
    float uv[4];
    uint textureIndex;
    uint layer;
    int sortKey;
    uint flags;
};

layout(std430, binding = 0) readonly buffer SpriteBuffer {
    GPUSprite sprites[];
};

layout(std430, binding = 1) buffer VisibleBuffer {
    uint visibleCount;
    uint visibleIndices[];
};

layout(std140, binding = 2) uniform CameraParams {
    float camX, camY;
    float zoom;
    float rotation;
    float viewMinX, viewMinY;
    float viewMaxX, viewMaxY;
    uint spriteCount;
    uint cullEnabled;
    uint layerMask;
};

layout(std430, binding = 3) buffer CounterBuffer {
    uint atomicCounter;
};

shared uint localCount;
shared uint localIndices[64];

void main() {
    uint idx = gl_GlobalInvocationID.x;
    
    if (gl_LocalInvocationIndex == 0) {
        localCount = 0;
    }
    barrier();
    
    bool visible = false;
    
    if (idx < spriteCount) {
        GPUSprite s = sprites[idx];
        
        uint passBits = (s.flags >> 1) & 0x7u;
        if ((layerMask & (1u << passBits)) != 0u) {
            if (cullEnabled == 0u) {
                visible = true;
            } else {
                float tx = s.transform[3];
                float ty = s.transform[7];
                
                float sx = abs(s.transform[0]) + abs(s.transform[1]);
                float sy = abs(s.transform[4]) + abs(s.transform[5]);
                float halfW = sx * 0.5;
                float halfH = sy * 0.5;
                
                if (tx + halfW >= viewMinX && tx - halfW <= viewMaxX &&
                    ty + halfH >= viewMinY && ty - halfH <= viewMaxY) {
                    visible = true;
                }
            }
        }
    }
    
    if (visible) {
        uint localIdx = atomicAdd(localCount, 1);
        if (localIdx < 64) {
            localIndices[localIdx] = idx;
        }
    }
    barrier();
    
    if (gl_LocalInvocationIndex == 0 && localCount > 0) {
        uint globalOffset = atomicAdd(atomicCounter, localCount);
        for (uint i = 0; i < localCount && globalOffset + i < spriteCount; ++i) {
            visibleIndices[globalOffset + i] = localIndices[i];
        }
    }
}
)GLSL";
}

void GPUDrivenRenderer::init(backend::IRenderDevice* device) {
    device_ = device;
    createBuffers();
    createPipelines();
    initialized_ = true;
    core::logInfo("GPUDrivenRenderer initialized");
}

void GPUDrivenRenderer::createPipelines() {
    backend::ComputePipelineDesc desc{};
    desc.code = cullingShaderGLSL;
    desc.codeSize = strlen(cullingShaderGLSL);
    desc.entryPoint = "main";
    desc.threadCountX = CULLING_WORKGROUP_SIZE;
    desc.threadCountY = 1;
    desc.threadCountZ = 1;
    desc.numReadonlyStorageBuffers = 1;
    desc.numReadwriteStorageBuffers = 2;
    desc.numUniformBuffers = 1;
    
    cullingPipeline_ = device_->createComputePipeline(desc);
    if (cullingPipeline_.valid()) {
        core::logInfo("GPUDrivenRenderer: culling pipeline created");
    } else {
        core::logWarn("GPUDrivenRenderer: failed to create culling pipeline");
    }
}

void GPUDrivenRenderer::shutdown() {
    if (!device_) return;
    
    if (cullingPipeline_.valid()) device_->destroyComputePipeline(cullingPipeline_);
    if (sortPipeline_.valid()) device_->destroyComputePipeline(sortPipeline_);
    if (visibleIndexBuffer_.valid()) device_->destroyBuffer(visibleIndexBuffer_);
    if (drawArgsBuffer_.valid()) device_->destroyBuffer(drawArgsBuffer_);
    if (counterBuffer_.valid()) device_->destroyBuffer(counterBuffer_);
    if (sortPingBuffer_.valid()) device_->destroyBuffer(sortPingBuffer_);
    if (sortPongBuffer_.valid()) device_->destroyBuffer(sortPongBuffer_);
    if (uniformBuffer_.valid()) device_->destroyBuffer(uniformBuffer_);
    
    initialized_ = false;
}

void GPUDrivenRenderer::createBuffers() {
    backend::BufferDesc desc{};
    
    desc.size = MAX_VISIBLE_SPRITES * sizeof(uint32_t);
    desc.usage = backend::BufferUsage::Storage | backend::BufferUsage::Indirect;
    visibleIndexBuffer_ = device_->createBuffer(desc);
    
    desc.size = sizeof(GPUDrawArgs);
    desc.usage = backend::BufferUsage::Storage | backend::BufferUsage::Indirect;
    drawArgsBuffer_ = device_->createBuffer(desc);
    
    desc.size = sizeof(uint32_t);
    desc.usage = backend::BufferUsage::Storage;
    counterBuffer_ = device_->createBuffer(desc);
    
    desc.size = MAX_VISIBLE_SPRITES * sizeof(uint32_t);
    desc.usage = backend::BufferUsage::Storage;
    sortPingBuffer_ = device_->createBuffer(desc);
    sortPongBuffer_ = device_->createBuffer(desc);
    
    desc.size = sizeof(CullingUniforms);
    desc.usage = backend::BufferUsage::Uniform;
    uniformBuffer_ = device_->createBuffer(desc);
    
    resetVisibleCount();
}

void GPUDrivenRenderer::setCullingParams(float camX, float camY, float zoom, float rotation,
                                          float viewMinX, float viewMinY, float viewMaxX, float viewMaxY,
                                          uint32_t spriteCount, uint32_t layerMask, bool cullEnabled) {
    cullingParams_.camX = camX;
    cullingParams_.camY = camY;
    cullingParams_.zoom = zoom;
    cullingParams_.rotation = rotation;
    cullingParams_.viewMinX = viewMinX;
    cullingParams_.viewMinY = viewMinY;
    cullingParams_.viewMaxX = viewMaxX;
    cullingParams_.viewMaxY = viewMaxY;
    cullingParams_.spriteCount = spriteCount;
    cullingParams_.cullEnabled = cullEnabled ? 1 : 0;
    cullingParams_.layerMask = layerMask;
    
    if (uniformBuffer_.valid()) {
        device_->uploadToBuffer(uniformBuffer_, &cullingParams_, sizeof(cullingParams_), 0);
    }
}

void GPUDrivenRenderer::dispatchCulling(backend::CommandBuffer& cb, uint32_t spriteCount) {
    if (!cullingPipeline_.valid()) return;
    
    resetVisibleCount();
    
    backend::DispatchCmd cmd{};
    cmd.pipeline = cullingPipeline_;
    cmd.groupCountX = (spriteCount + CULLING_WORKGROUP_SIZE - 1) / CULLING_WORKGROUP_SIZE;
    cmd.groupCountY = 1;
    cmd.groupCountZ = 1;
    
    cmd.bindings.readonlyStorageBufferCount = 1;
    cmd.bindings.readonlyStorageBuffers[0] = {};
    
    cmd.bindings.readwriteStorageBufferCount = 2;
    cmd.bindings.readwriteStorageBuffers[0] = visibleIndexBuffer_;
    cmd.bindings.readwriteStorageBuffers[1] = counterBuffer_;
    
    cb.dispatch(cmd);
    cb.barrier(backend::BarrierCmd::Type::StorageBuffer);
}

void GPUDrivenRenderer::dispatchSorting(backend::CommandBuffer& cb, uint32_t visibleCount) {
    if (!sortPipeline_.valid() || visibleCount == 0) return;
    
    uint32_t n = visibleCount;
    uint32_t maxStage = 0;
    while ((1u << maxStage) < n) ++maxStage;
    
    BufferHandle pingBuffer = visibleIndexBuffer_;
    BufferHandle pongBuffer = sortPingBuffer_;
    
    for (uint32_t stage = 0; stage < maxStage; ++stage) {
        for (uint32_t pass = 0; pass <= stage; ++pass) {
            backend::DispatchCmd cmd{};
            cmd.pipeline = sortPipeline_;
            cmd.groupCountX = (n + SORTING_WORKGROUP_SIZE - 1) / SORTING_WORKGROUP_SIZE;
            cmd.groupCountY = 1;
            cmd.groupCountZ = 1;
            
            cmd.bindings.readonlyStorageBufferCount = 2;
            cmd.bindings.readonlyStorageBuffers[0] = {};
            cmd.bindings.readonlyStorageBuffers[1] = pingBuffer;
            
            cmd.bindings.readwriteStorageBufferCount = 1;
            cmd.bindings.readwriteStorageBuffers[0] = pongBuffer;
            
            cb.dispatch(cmd);
            cb.barrier(backend::BarrierCmd::Type::StorageBuffer);
            
            std::swap(pingBuffer, pongBuffer);
        }
    }
}

void GPUDrivenRenderer::resetVisibleCount() {
    uint32_t zero = 0;
    if (counterBuffer_.valid()) {
        device_->uploadToBuffer(counterBuffer_, &zero, sizeof(zero), 0);
    }
}

uint32_t GPUDrivenRenderer::getVisibleCount() const {
    if (!counterBuffer_.valid()) return 0;
    uint32_t count = 0;
    device_->downloadFromBuffer(counterBuffer_, &count, sizeof(count), 0);
    return count;
}

void GPUDrivenRenderer::setViewMatrix(float camX, float camY, float zoom, float rotation) {
    const float c = cosf(rotation);
    const float s = sinf(rotation);
    
    memset(viewMatrix_, 0, sizeof(viewMatrix_));
    viewMatrix_[0]  =  c * zoom;
    viewMatrix_[1]  =  s * zoom;
    viewMatrix_[4]  = -s * zoom;
    viewMatrix_[5]  =  c * zoom;
    viewMatrix_[10] = 1.f;
    viewMatrix_[12] = -( c * camX - s * camY) * zoom;
    viewMatrix_[13] = -( s * camX + c * camY) * zoom;
    viewMatrix_[15] = 1.f;
}

void GPUDrivenRenderer::buildMVP(float* out) const {
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            out[i * 4 + j] = 0.f;
            for (int k = 0; k < 4; ++k) {
                out[i * 4 + j] += viewMatrix_[i * 4 + k] * projMatrix_[k * 4 + j];
            }
        }
    }
}

}