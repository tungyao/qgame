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
#include <unordered_map>
#include <vector>

namespace backend { class CommandBuffer; }

namespace engine {
class EngineContext;

class RenderSystem final : public ISystem {
public:
    explicit RenderSystem(EngineContext& ctx);
    ~RenderSystem();

    UpdatePhaseMask phaseMask() const override {
        return updatePhaseBit(UpdatePhase::Render);
    }

    void init()           override;
    void update(float dt) override;
    void shutdown()       override;

    // 把 Camera 组件在给定 viewport 下解析成真正参与渲染/裁剪的视图参数。
    //
    // 这是第一阶段的核心入口：
    //   - CPU culling
    //   - GPU-driven 可见区
    //   - 粒子 camera
    //   - 光照 viewport
    //   - UI 世界锚点
    //
    // 都必须复用这份结果，避免“同一台相机，不同系统各算各的”。
    static ResolvedCameraView2D resolveCameraView(const Transform& tf, const Camera& cam,
                                                  int viewportW, int viewportH);

    // 选出“当前帧用于渲染 World pass 的主相机”，并返回它在窗口尺寸下的解析结果。
    //
    // 选择规则与 RenderSystem 构建 camera pass 的规则保持一致：
    //   1. 只看 primary=true 的相机
    //   2. 只看 layerMask 覆盖 World 的相机
    //   3. 按 depth 从小到大取第一台
    //
    // UISystem 的 UIWorldAnchor 需要这份结果，把世界坐标投到屏幕像素时才能与
    // 本帧真正绘制出来的世界视图保持一致。
    static ResolvedCameraView2D resolveActiveWorldCamera(const EngineContext& ctx);

    static void buildSceneCommands(EngineContext& ctx, backend::CommandBuffer& cb,
                                   int viewportW, int viewportH,
                                   float tileAnimationTimeSeconds = 0.0f);

    SpriteBuffer& spriteBuffer() { return spriteBuffer_; }
    GPUDrivenRenderer& gpuRenderer() { return gpuRenderer_; }
    
    void setGPUDrivenEnabled(bool enabled) { gpuDrivenEnabled_ = enabled; }
    bool isGPUDrivenEnabled() const { return gpuDrivenEnabled_; }

private:
    void onRenderPhase(float dt) override {
        update(dt);
    }

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
    void onSpriteConstruct(entt::registry& reg, entt::entity e);
    void onSpriteUpdate(entt::registry& reg, entt::entity e);
    void onTransformConstruct(entt::registry& reg, entt::entity e);
    void onTransformUpdate(entt::registry& reg, entt::entity e);
    void queueSpriteSync(entt::entity e);
    void ensureSpriteCullCapacity(uint32_t required);
    void deactivateSpriteCullProxy(GPUHandle handle);
    void updateSpriteCullProxy(const Sprite& spr, const GPUSprite& slot, float sortY);
    void markMixedGPUSlotDirty(GPUHandle handle);
    void syncParticleEmitters(float dt);
    std::vector<backend::IRenderDevice::GPUParticleParams>
    collectParticleParams(const Camera& cam,
                          const backend::CameraData& camera,
                          float dt);
    void submitParticlePass(const Camera& cam,
                            const backend::CameraData& camera,
                            float dt);

    EngineContext& ctx_;
    SpriteBuffer spriteBuffer_;
    GPUDrivenRenderer gpuRenderer_;
    GPUParticleRenderer particleRenderer_;

    struct CachedGPUTile {
        TextureHandle texture; // 当前 tile 所属 tileset 纹理，用于提交时按纹理切批
        uint32_t gpuIndex = 0; // mixedGpuSpriteBuffer_ 中的实例索引
        entt::entity mapEntity = entt::null; // 反查 TileMap 组件，用于按帧解析动画 frame
        int baseGid = TileMap::EMPTY_GID; // layer.tiles 中存储的稳定 gid，不随动画跳帧改变
        int cellX = 0;          // 地图格坐标 X；randomStart 和 per-frame frame 解析都需要
        int cellY = 0;          // 地图格坐标 Y
        int layerIndex = 0;     // 图层索引；用于 randomStart hash 与 collision/visual 对齐
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

    struct SpriteCullProxy {
        // slotActive 表示这个 gpu slot 当前是否对应一只存活 sprite。
        // 它与 Sprite::visible 不同：不可见 sprite 仍然可能占着 slot，
        // 只是不会被插入 spatial hash，也不会进入可见列表。
        bool slotActive = false;
        bool visible = false;

        TextureHandle texture;
        uint32_t gpuIndex = 0;
        RenderPass pass = RenderPass::World;
        int layer = 0;
        bool ySort = false;
        float y = 0.f;
        int sortKey = 0;
        uint32_t seq = 0;

        // 这是 CPU culling 直接读取的预计算 AABB。
        // 每次 sprite dirty 时更新一次，避免每台相机都从 transform 现算。
        float centerX = 0.f;
        float centerY = 0.f;
        float halfW = 0.f;
        float halfH = 0.f;

        // 记录当前 AABB 覆盖的格子范围，便于对象移动时做 remove+insert。
        int minCellX = 0;
        int maxCellX = -1;
        int minCellY = 0;
        int maxCellY = -1;
        bool indexedInGrid = false;

        // 查询阶段用 stamp 去重：大对象可能跨多个格子，但同一相机只应收集一次。
        uint32_t queryStamp = 0;

        // activeSpriteSlots_ 中的位置，用 swap-pop 在 O(1) 时间移除。
        uint32_t activeListIndex = 0;
    };

    struct SpriteSpatialHash {
        // 固定 cell size 的 spatial hash。
        // 2D 渲染里它比四叉树更适合做热路径裁剪：
        //   - 更新简单（remove+insert）
        //   - query 只扫相机覆盖的 cell
        //   - 稀疏世界下不需要预分配整张二维表
        float cellSize = 128.f;
        float invCellSize = 1.f / 128.f;
        std::unordered_map<int64_t, std::vector<uint32_t>> cells;

        void setCellSize(float newCellSize) {
            cellSize = newCellSize;
            invCellSize = 1.f / newCellSize;
        }
    };

    BufferHandle mixedGpuSpriteBuffer_; // GPU-driven shader 读取的统一 Sprite/Tile instance buffer
    uint32_t mixedGpuSpriteCapacity_ = 0; // mixedGpuSpriteBuffer_ 当前可容纳的 GPUSprite 数量
    uint32_t tileGpuBaseIndex_ = 0; // TileMap 缓存实例在 mixed buffer 中的起始索引
    uint64_t tileGpuCacheSignature_ = 0; // TileMap 数据签名；变化时重建并重传静态 tile 实例
    std::vector<GPUSprite> tileGpuInstances_; // CPU 侧静态 tile 实例缓存，仅 dirty 时上传
    std::vector<CachedGPUTile> tileGpuItems_; // CPU 侧排序/裁剪元数据，指向 tileGpuInstances_ 的 GPU 索引
    std::vector<uint8_t> mixedGpuDirty_; // mixed buffer 的 dirty 位，避免每帧全量重传 sprite 区域
    std::vector<SpriteCullProxy> spriteCullProxies_; // 与 gpuHandle.index 对齐的 CPU 裁剪缓存
    std::vector<uint32_t> activeSpriteSlots_; // 当前活跃 sprite 的 gpu slot 列表；cull 关闭时直接遍历它
    std::vector<uint32_t> visibleSpriteSlotsScratch_; // 每相机复用的查询缓存，避免临时分配
    std::vector<entt::entity> queuedSpriteSync_; // Transform/Sprite 变化后，等待同步到 GPU+proxy 的实体
    SpriteSpatialHash spriteSpatialHash_; // 世界 sprite 的空间索引
    uint32_t spriteCullQueryStamp_ = 1; // 每次 query 递增，用于跨 cell 去重
    uint32_t spriteCullSequenceCounter_ = 1; // 稳定插入序，排序键完全相同的时候保持帧间稳定
    bool gpuDrivenEnabled_ = false;
    float lastDt_ = 0.f;
    float tileAnimationTimeSeconds_ = 0.f; // TileMap v2 视觉动画时间轴；只影响渲染，不影响碰撞
    entt::connection destroyConnection_;
    entt::connection spriteConstructConnection_;
    entt::connection spriteUpdateConnection_;
    entt::connection transformConstructConnection_;
    entt::connection transformUpdateConnection_;
};

} // namespace engine
