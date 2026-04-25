#pragma once
#include "../../backend/renderer/IRenderDevice.h"
#include "../../backend/renderer/CommandBuffer.h"
#include "GPUSprite.h"
#include "SpriteBuffer.h"
#include <memory>

namespace engine {

struct GPUDrawArgs {
    uint32_t vertexCount = 6;
    uint32_t instanceCount = 0;
    uint32_t firstVertex = 0;
    uint32_t firstInstance = 0;
};

class GPUDrivenRenderer {
public:
    static constexpr uint32_t MAX_VISIBLE_SPRITES = 65536;
    static constexpr uint32_t MAX_TEXTURES = 64;
    static constexpr uint32_t CULLING_WORKGROUP_SIZE = 64;
    static constexpr uint32_t SORTING_WORKGROUP_SIZE = 256;

    void init(backend::IRenderDevice* device);
    void shutdown();
    
    void setCullingParams(float camX, float camY, float zoom, float rotation,
                          float viewMinX, float viewMinY, float viewMaxX, float viewMaxY,
                          uint32_t spriteCount, uint32_t layerMask, bool cullEnabled);
    
    void dispatchCulling(backend::CommandBuffer& cb, uint32_t spriteCount);
    void dispatchSorting(backend::CommandBuffer& cb, uint32_t visibleCount);
    
    BufferHandle getVisibleIndexBuffer() const { return visibleIndexBuffer_; }
    BufferHandle getDrawArgsBuffer() const { return drawArgsBuffer_; }
    
    void resetVisibleCount();
    uint32_t getVisibleCount() const;
    
    bool isInitialized() const { return initialized_; }
    bool hasCullingPipeline() const { return cullingPipeline_.valid(); }
    
    void setProjectionMatrix(const float* proj) {
        std::memcpy(projMatrix_, proj, sizeof(projMatrix_));
    }
    
    const float* getProjectionMatrix() const { return projMatrix_; }
    
    void setViewMatrix(float camX, float camY, float zoom, float rotation);
    const float* getViewMatrix() const { return viewMatrix_; }
    
    void buildMVP(float* out) const;

private:
    void createPipelines();
    void createBuffers();
    
    backend::IRenderDevice* device_ = nullptr;
    bool initialized_ = false;
    
    ComputePipelineHandle cullingPipeline_;
    ComputePipelineHandle sortPipeline_;
    
    BufferHandle visibleIndexBuffer_;
    BufferHandle drawArgsBuffer_;
    BufferHandle counterBuffer_;
    BufferHandle sortPingBuffer_;
    BufferHandle sortPongBuffer_;
    BufferHandle uniformBuffer_;
    
    float projMatrix_[16] = {};
    float viewMatrix_[16] = {};
    
    struct CullingUniforms {
        float camX, camY;
        float zoom;
        float rotation;
        float viewMinX, viewMinY;
        float viewMaxX, viewMaxY;
        uint32_t spriteCount;
        uint32_t cullEnabled;
        uint32_t layerMask;
        uint32_t pad;
    } cullingParams_;
};

}
