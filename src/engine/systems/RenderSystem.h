#pragma once
#include "ISystem.h"
#include "../components/RenderComponents.h"
#include "../components/AnimatorComponent.h"
#include "../resources/SpriteBuffer.h"
#include "../resources/GPUDrivenRenderer.h"
#include "../resources/GPUParticleRenderer.h"
#include "../../backend/renderer/IRenderDevice.h"
#include <entt/entt.hpp>
#include <cstdint>

namespace backend { class CommandBuffer; }

namespace engine {
class EngineContext;

class RenderSystem final : public ISystem {
public:
    explicit RenderSystem(EngineContext& ctx);
    ~RenderSystem();

    void init()           override;
    void update(float dt) override;
    void shutdown()       override;

    static void buildSceneCommands(EngineContext& ctx, backend::CommandBuffer& cb,
                                   int viewportW, int viewportH);

    SpriteBuffer& spriteBuffer() { return spriteBuffer_; }
    GPUDrivenRenderer& gpuRenderer() { return gpuRenderer_; }
    
    void setGPUDrivenEnabled(bool enabled) { gpuDrivenEnabled_ = enabled; }
    bool isGPUDrivenEnabled() const { return gpuDrivenEnabled_; }

private:
    void buildCommandBuffer();
    void buildCommandBufferGPUDriven();
    void syncEntitiesToGPU();
    void allocateGPUSlot(entt::entity e, Sprite& spr);
    void freeGPUSlot(entt::registry& reg, entt::entity e);
    void updateGPUSlot(const Transform& tf, const Sprite& spr, const AnimatorOutput* aout = nullptr);
    void ensureMixedGPUCapacity(uint32_t required);
    void syncSpritesToMixedGPUBuffer();
    void rebuildTileGPUCacheIfNeeded();
    uint64_t computeTileGPUCacheSignature() const;
    void onTransformUpdate(entt::registry& reg, entt::entity e);
    void syncParticleEmitters(float dt);
    std::vector<backend::IRenderDevice::GPUParticleParams>
    collectParticleParams(const Transform& tf, const Camera& cam,
                          int viewportW, int viewportH, float dt);
    void submitParticlePass(const Transform& tf, const Camera& cam,
                            int viewportW, int viewportH, float dt);

    EngineContext& ctx_;
    SpriteBuffer spriteBuffer_;
    GPUDrivenRenderer gpuRenderer_;
    GPUParticleRenderer particleRenderer_;

    struct CachedGPUTile {
        TextureHandle texture; // 当前 tile 所属 tileset 纹理，用于提交时按纹理切批
        uint32_t gpuIndex = 0; // mixedGpuSpriteBuffer_ 中的实例索引
        int layer = 0;         // 渲染层，和 Sprite::layer 使用同一排序维度
        bool ySort = true;     // TileMap 默认按行参与 ySort，使角色可插入地形层
        float y = 0.f;         // ySort 使用的世界 Y 坐标
        int sortKey = 0;       // 细粒度排序键，保留给后续 tile/object 排序扩展
        int seq = 0;           // 稳定插入序，保证完全相同排序键时帧间不抖动
        float centerX = 0.f;   // 相机裁剪用世界中心 X
        float centerY = 0.f;   // 相机裁剪用世界中心 Y
        float halfW = 0.f;     // 相机裁剪用半宽
        float halfH = 0.f;     // 相机裁剪用半高
    };

    BufferHandle mixedGpuSpriteBuffer_; // GPU-driven shader 读取的统一 Sprite/Tile instance buffer
    uint32_t mixedGpuSpriteCapacity_ = 0; // mixedGpuSpriteBuffer_ 当前可容纳的 GPUSprite 数量
    uint32_t tileGpuBaseIndex_ = 0; // TileMap 缓存实例在 mixed buffer 中的起始索引
    uint64_t tileGpuCacheSignature_ = 0; // TileMap 数据签名；变化时重建并重传静态 tile 实例
    std::vector<GPUSprite> tileGpuInstances_; // CPU 侧静态 tile 实例缓存，仅 dirty 时上传
    std::vector<CachedGPUTile> tileGpuItems_; // CPU 侧排序/裁剪元数据，指向 tileGpuInstances_ 的 GPU 索引
    bool gpuDrivenEnabled_ = false;
    float lastDt_ = 0.f;
    entt::connection destroyConnection_;
    entt::connection transformUpdateConnection_;
};

} // namespace engine
