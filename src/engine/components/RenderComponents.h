#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
#include "../../backend/shared/ResourceHandle.h"
#include "../../core/math/Rect.h"
#include "../../core/math/Color.h"
#include "../resources/GPUSprite.h"

namespace engine {

struct EntityID {
    static constexpr int MAX_LEN = 64;
    std::array<char, MAX_LEN> buf{};

    EntityID() { buf[0] = '\0'; }
    explicit EntityID(const char* s) {
        buf[0] = '\0';
        if (s) {
            size_t i = 0;
            while (s[i] && i < MAX_LEN - 1) { buf[i] = s[i]; ++i; }
            buf[i] = '\0';
        }
    }
    const char* c_str() const { return buf.data(); }
    bool valid() const { return buf[0] != '\0'; }
};

// Entity 显示名称（编辑器 Hierarchy 用）
struct Name {
    static constexpr int MAX_LEN = 64;
    std::array<char, MAX_LEN> buf{};

    Name() { buf[0] = '\0'; }
    explicit Name(const char* s) {
        buf[0] = '\0';
        if (s) {
            size_t i = 0;
            while (s[i] && i < MAX_LEN - 1) { buf[i] = s[i]; ++i; }
            buf[i] = '\0';
        }
    }
    const char* c_str() const { return buf.data(); }
};

struct Transform {
    float x        = 0.f;
    float y        = 0.f;
    float rotation = 0.f;   // 弧度
    float scaleX   = 1.f;
    float scaleY   = 1.f;
};

/**
 * TileMap 是引擎当前公开的网格地图运行时组件。
 *
 * 坐标约定：
 * - Transform.x/y 是整张地图左上角的世界坐标。
 * - 每个 cell 的世界 AABB 为：
 *   [Transform.x + x * tileSize, Transform.y + y * tileSize,
 *    Transform.x + (x + 1) * tileSize, Transform.y + (y + 1) * tileSize]
 * - TileMap 自身不使用 Transform.rotation/scaleX/scaleY；当前渲染和碰撞都按
 *   axis-aligned tile grid 处理。
 *
 * gid 约定：
 * - EMPTY_GID(-1) 表示空格，不渲染、不碰撞。
 * - gid 是全局 tile id。每个 Tileset 使用 [firstGid, firstGid + count)
 *   的半开区间声明自己负责的 gid 范围。
 * - localTileId = gid - firstGid，用于查找 tileset atlas 中的 tile 和 collision。
 *
 * 编辑器/导入导出 JSON 约定：
 * - TileMap 组件本体当前导出 qgame.tilemap.v2 结构；完整资源包仍使用
 *   qgame.tilemap.engine-package 作为外层包装类型。
 * - 运行时保留 qgame.tilemap.v1 导入兼容：旧版 tileset collision[] 会在
 *   读取时映射为 v2 TileCollision::Full。
 * - 字段名保持短名以减少地图文件体积：
 *   w=width, h=height, ts=tileSize, cols=tileset columns。
 * - tools/tilemap_editor/ 应按这些常量和字段语义导出，避免编辑器和运行时
 *   对 visible/collidable/renderLayer 的含义产生分歧。
 */
struct TileMap {
    static constexpr int EMPTY_GID = -1;
    static constexpr int FORMAT_VERSION = 2;
    static constexpr const char* FORMAT_TYPE = "qgame.tilemap.v2";
    static constexpr const char* LEGACY_FORMAT_TYPE = "qgame.tilemap.v1";
    static constexpr const char* ENGINE_PACKAGE_TYPE = "qgame.tilemap.engine-package";
    static constexpr int ENGINE_PACKAGE_VERSION = 2;

    /**
     * TileAnimationFrame 定义 flipbook 动画中的单帧显示 gid。
     *
     * gid 仍然使用全局 gid，这样编辑器不需要猜测 frame 属于哪个 tileset，
     * 运行时也可以统一复用 tilesetForGid/localTileId 逻辑。
     */
    struct TileAnimationFrame {
        int gid = EMPTY_GID;
        float duration = 0.1f;
    };

    /**
     * TileAnimation 描述“某个放置在 layer.tiles 中的 baseGid”如何随时间切换到
     * 若干显示帧。baseGid 是地图里真正存储的稳定值；frames 决定画面上显示什么。
     */
    struct TileAnimation {
        int baseGid = EMPTY_GID;
        std::vector<TileAnimationFrame> frames;
        bool randomStart = false;
        float speed = 1.0f;
    };


    enum class TileCollisionShape : uint8_t {
        None = 0,
        Full,
        Rect,
        Polygon,
        OneWay,
        Trigger
    };

    /**
     * TileCollision 描述 gid 的物理形状。
     *
     * points 约定：
     * - Rect: [x, y, w, h]
     * - Polygon: [x0, y0, x1, y1, ...]
     * - Full/None/Trigger 可留空
     */
    struct TileCollision {
        int gid = EMPTY_GID;
        TileCollisionShape shape = TileCollisionShape::None;
        std::vector<float> points;
    };

    /**
     * Tileset 描述一张 tile atlas 在全局 gid 空间中的范围。
     *
     * JSON:
     * {
     *   "id": "optional-editor-id",
     *   "name": "optional-display-name",
     *   "firstGid": 0,
     *   "count": 16,
     *   "cols": 4,
     *   "tex": "texture asset path or display name",
     *   "assetId": "optional AssetManager id",
     *   "sourceKind": "builtin|image",
     *   "sourceDataUrl": "optional editor package image data",
     *   "animations": [...],
     *   "visuals": [...],
     *   "collisions": [...]
     * }
     */
    struct Tileset {
        std::string id;
        std::string name;
        std::string texturePath;
        std::string assetId;
        std::string sourceKind;
        std::string sourceDataUrl;

        TextureHandle texture;              // 渲染用 atlas 纹理；加载器负责把 JSON tex/assetId 转成句柄
        int firstGid = 0;                   // 全局 tile id 起点，包含该值
        int count    = 0;                   // tile 数量；有效 gid 范围是 [firstGid, firstGid + count)
        int columns  = 1;                   // atlas 横向 tile 数；localId % columns 得到 atlas x
        std::vector<TileAnimation> animations;
        std::vector<TileCollision> collisions;
    };

    /**
     * Layer 描述一层 tile 数据。
     *
     * JSON:
     * {
     *   "name": "地面",
     *   "visible": true,
     *   "collidable": true,
     *   "renderLayer": 0,
     *   "tiles": [gid, gid, -1, ...]
     * }
     *
     * visible 和 collidable 是独立开关：
     * - visible=false 只影响渲染，仍允许隐藏碰撞层。
     * - collidable=false 只影响 PhysicsSystem，不影响渲染。
     */
    struct Layer {
        std::string name;        // 编辑器显示名；运行时只用于调试/序列化
        std::vector<int> tiles;  // 行优先数组，长度应为 width * height；EMPTY_GID 表示空格
        bool visible = true;     // 是否渲染该层
        bool collidable = true;  // 是否参与 TileMap 静态碰撞；独立于 visible，允许隐藏碰撞层
        int renderLayer = 0;     // 渲染排序层；和 Sprite::layer 使用同一排序维度
    };

    int width    = 0;             // 地图列数，JSON 字段 w
    int height   = 0;             // 地图行数，JSON 字段 h
    int tileSize = 16;            // tile 边长，单位是世界像素，JSON 字段 ts
    std::vector<Tileset> tilesets; // gid 到纹理/collision 的映射表
    std::vector<Layer>   layers;   // 多图层 tile 数据，按数组顺序作为默认 renderLayer

    /**
     * 返回一维行优先数组下标。调用者应先用 inBounds() 判断坐标是否合法。
     */
    size_t cellIndex(int x, int y) const {
        return static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x);
    }

    /**
     * 检查 cell 坐标是否位于地图范围内。
     */
    bool inBounds(int x, int y) const {
        return x >= 0 && x < width && y >= 0 && y < height;
    }

    /**
     * 指定 layer/cell 的 gid。越界或缺失数据返回 EMPTY_GID。
     */
    int tileAt(int layer, int x, int y) const {
        if (layer < 0 || layer >= static_cast<int>(layers.size())) return EMPTY_GID;
        if (!inBounds(x, y)) return EMPTY_GID;
        size_t idx = cellIndex(x, y);
        if (idx >= layers[layer].tiles.size()) return EMPTY_GID;
        return layers[layer].tiles[idx];
    }

    /**
     * 根据全局 gid 查找负责该 gid 的 Tileset。EMPTY_GID 或未声明 gid 返回 nullptr。
     */
    const Tileset* tilesetForGid(int gid) const {
        if (gid == EMPTY_GID) return nullptr;
        for (const auto& ts : tilesets) {
            if (gid >= ts.firstGid && gid < ts.firstGid + ts.count) return &ts;
        }
        return nullptr;
    }

    /**
     * 将全局 gid 转为 tileset 内 local id。未找到返回 -1。
     */
    int localTileId(int gid) const {
        const Tileset* ts = tilesetForGid(gid);
        return ts ? gid - ts->firstGid : -1;
    }

    /**
     * 查找 gid 对应的动画定义。
     *
     * 优先级：
     * 1. 若 TileVisual.animation 指向有效索引，则直接使用该动画。
     * 2. 否则按 baseGid 线性匹配，方便没有显式 visual 的导入数据也能工作。
     */
    const TileAnimation* animationForGid(int gid) const {
        const Tileset* ts = tilesetForGid(gid);
        if (!ts) return nullptr;

        for (const TileAnimation& animation : ts->animations) {
            if (animation.baseGid == gid) return &animation;
        }
        return nullptr;
    }

    /**
     * 查找 gid 的显式碰撞定义。未声明时返回 nullptr；调用者可继续检查
     * legacyCollision 兼容数据并按默认 None 处理。
     */
    const TileCollision* collisionForGid(int gid) const {
        const Tileset* ts = tilesetForGid(gid);
        if (!ts) return nullptr;
        for (const TileCollision& collision : ts->collisions) {
            if (collision.gid == gid) return &collision;
        }
        return nullptr;
    }

    /**
     * 解析当前时间下该 gid 实际应显示的 frame gid。
     *
     * 只有 Flipbook/WaterFlipbook 会切换显示帧；其他 visual kind 目前保持原 gid。
     * randomStart 使用 cell 坐标和 layer 生成稳定伪随机起点，保证地图里重复水面
     * tile 不会完全同步闪烁，同时不会因为帧率变化而跳变。
     */
    int resolveVisualGid(int gid, float timeSeconds, int x = 0, int y = 0, int layer = 0) const {

        const TileAnimation* animation = animationForGid(gid);
        if (!animation || animation->frames.empty()) return gid;

        float totalDuration = 0.0f;
        for (const TileAnimationFrame& frame : animation->frames) {
            totalDuration += std::max(frame.duration, 0.0001f);
        }
        if (totalDuration <= 0.0f) return gid;
    
        const float animSpeed = (animation->speed > 0.0f) ? animation->speed : 1.0f;
        if (animation->randomStart) {
            auto mix = [](uint32_t h, uint32_t v) {
                h ^= v + 0x9e3779b9u + (h << 6u) + (h >> 2u);
                return h;
            };
            uint32_t h = 2166136261u;
            h = mix(h, static_cast<uint32_t>(gid));
            h = mix(h, static_cast<uint32_t>(x));
            h = mix(h, static_cast<uint32_t>(y));
            h = mix(h, static_cast<uint32_t>(layer));
            const float normalized = static_cast<float>(h & 0x00FFFFFFu) /
                                     static_cast<float>(0x01000000u);

        }

        float localTime = std::fmod(timeSeconds * animSpeed ,
                                    totalDuration);
        if (localTime < 0.0f) localTime += totalDuration;

        float cursor = 0.0f;
        for (const TileAnimationFrame& frame : animation->frames) {
            const float duration = std::max(frame.duration, 0.0001f);
            if (localTime < cursor + duration) {
                return frame.gid != EMPTY_GID ? frame.gid : gid;
            }
            cursor += duration;
        }

        const TileAnimationFrame& lastFrame = animation->frames.back();
        return lastFrame.gid != EMPTY_GID ? lastFrame.gid : gid;
    }

    /**
     * 返回 gid 的完整碰撞 profile；显式 v2 碰撞优先，其次兼容旧版 collision[]。
     */
    TileCollision collisionProfileForGid(int gid) const {
        TileCollision out;
        out.gid = gid;
        out.shape = TileCollisionShape::None;

        if (gid == EMPTY_GID) return out;
        if (const TileCollision* explicitCollision = collisionForGid(gid)) {
            return *explicitCollision;
        }

        const Tileset* ts = tilesetForGid(gid);
        if (!ts) return out;
        const int local = gid - ts->firstGid;
        return out;
    }

    /**
     * 判断某个 gid 是否产生阻挡碰撞。
     *
     * Trigger 只汇报事件，不阻挡；其余有体积的 shape 都视为阻挡。
     */
    bool tileBlocks(int gid) const {
        const TileCollision collision = collisionProfileForGid(gid);
        switch (collision.shape) {
            case TileCollisionShape::None:
            case TileCollisionShape::Trigger:
                return false;
            case TileCollisionShape::Full:
            case TileCollisionShape::Rect:
            case TileCollisionShape::Polygon:
            case TileCollisionShape::OneWay:
            default:
                return true;
        }
    }

    /**
     * 判断某个 gid 是否为 trigger tile。
     */
    bool tileTriggers(int gid) const {
        return collisionProfileForGid(gid).shape == TileCollisionShape::Trigger;
    }

    /**
     * 兼容旧调用点：tileSolid 现在等价于“是否阻挡”。
     */
    bool tileSolid(int gid) const {
        return tileBlocks(gid);
    }

    /**
     * 返回某个图层/格子的碰撞定义。
     *
     * layer 非法、坐标越界、图层不参与碰撞或 cell 为空时，都返回 shape=None。
     */
    TileCollision collisionAt(int layer, int x, int y) const {
        TileCollision out;
        out.shape = TileCollisionShape::None;
        out.gid = EMPTY_GID;

        if (layer < 0 || layer >= static_cast<int>(layers.size())) return out;
        if (!inBounds(x, y)) return out;
        if (!layers[static_cast<size_t>(layer)].collidable) return out;

        const int gid = tileAt(layer, x, y);
        out = collisionProfileForGid(gid);
        return out;
    }

    /**
     * 判断某个 cell 是否有任意 collidable 图层放置了实心 tile。
     */
    bool solidTileAt(int x, int y) const {
        for (int layer = 0; layer < static_cast<int>(layers.size()); ++layer) {
            if (!layers[layer].collidable) continue;
            if (tileBlocks(tileAt(layer, x, y))) return true;
        }
        return false;
    }
};

enum class RenderPass : int {
    World = 0,   // 世界渲染（3D/2D场景）
    UI     = 1,  // UI渲染（跟随世界变换）
    Screen = 2   // 屏幕UI（固定屏幕位置）
};

enum class CameraType : int {
    World = 0,   // 主世界摄像机
    UI     = 1,  // UI摄像机
    Screen = 2   // 屏幕摄像机
};

// CameraProjectionMode 定义“窗口尺寸变化时，相机怎样解释自己的 viewport”。
//
// StretchWithWindow:
//   旧行为。camera.zoom 直接按“当前窗口像素 / 世界单位”解释。
//   窗口越高，可见世界越高；窗口越宽，可见世界越宽。
//
// FixedVertical:
//   第一阶段推荐行为。camera.zoom 先按 referenceViewportHeight 对齐到一个
//   “参考高度”，再按当前窗口高度等比换算成实际 zoom。
//   结果是：纵向可见世界高度保持稳定，横向只会因为 aspect ratio 变化而扩宽/缩窄。
//
// 这正是多数 2D tilemap 游戏的主视角策略：
//   - 地图格子大小固定
//   - 世界单位固定
//   - 全屏 / 任意分辨率下，主要变化的是左右视野宽度
enum class CameraProjectionMode : int {
    StretchWithWindow = 0,
    FixedVertical     = 1
};

// 默认 layer mask：覆盖所有 RenderPass 位（World/UI/Screen + 未来扩展）
inline constexpr uint32_t kCameraLayerMaskAll = 0xFFFFFFFFu;

// 把 RenderPass 值转成 layer 位
inline constexpr uint32_t renderPassBit(RenderPass p) {
    return 1u << static_cast<uint32_t>(p);
}

struct Camera {
    float       zoom        = 1.f;
    float       rotation    = 0.f;   // 弧度
    bool        primary     = true;  // = active；多相机各自独立开关

    // —— Camera-driven 渲染参数 ————————————————————————————————
    int         depth       = 0;                       // 绘制顺序：小先大后；同 depth 按 ECS 顺序
    uint32_t    layerMask   = kCameraLayerMaskAll;     // 该相机绘制哪些 RenderPass 的 drawable
    bool        clear       = true;                    // 是否清屏（叠加相机置 false）
    core::Color clearColor  = core::Color::Black;
    bool        cullEnabled = true;                    // 关掉则该相机跳过视锥剔除（UI/Screen 适用）
    bool        pixelSnap   = false;                   // 是否将相机坐标对齐到物理像素，防止次像素抖动

    // 投影策略：
    //   - StretchWithWindow: 完全跟着当前窗口尺寸拉伸
    //   - FixedVertical    : 保持固定纵向视野，横向按 aspect 扩展
    //
    // 默认选 FixedVertical，是为了让 1280x720、1920x1080、21:9 等输出都保持
    // 同样的“竖向世界高度”，从而获得稳定的 tile/gameplay 手感。
    CameraProjectionMode projectionMode = CameraProjectionMode::FixedVertical;

    // referenceViewportHeight 只在 FixedVertical 下生效。
    //
    // 含义不是“世界单位高度”，而是“设计时参考的屏幕高度（像素）”。
    // camera.zoom 仍旧表示“参考高度下，每个世界单位对应多少像素”。
    //
    // 举例：
    //   referenceViewportHeight = 720
    //   zoom = 2
    //
    // 则 1280x720 下：
    //   effectiveZoom = 2
    //   visibleWorldHeight = 720 / 2 = 360 world units
    //
    // 到 1920x1080 下：
    //   effectiveZoom = 2 * (1080 / 720) = 3
    //   visibleWorldHeight = 1080 / 3 = 360 world units
    //
    // 这样纵向保持 360 world units 不变，只是横向可见范围变宽。
    float       referenceViewportHeight = 720.f;

    // —— 兼容字段（暂保留，未来移除）——————————————————————————
    RenderPass  renderPass  = RenderPass::World;
    CameraType  type        = CameraType::World;
};

// ResolvedCameraView2D 是“某个 Camera 在某个输出尺寸下真正用于渲染/裁剪的结果”。
//
// 它把两类信息拆清楚了：
//   1. Authored data
//      即组件上保存的 camera.x/y/rotation/zoom/projectionMode
//   2. Runtime-resolved data
//      即结合当前窗口尺寸后，真正送给渲染器的 effective zoom 和可见世界范围
//
// RenderSystem、UISystem、粒子、光照、GPU-driven 裁剪必须共享同一份解析规则，
// 否则最容易出现的问题是：
//   - CPU culling 和 GPU draw 的视野不一致
//   - UI 世界锚点与实际渲染位置不一致
//   - 粒子/光照仍按旧 viewport 计算，边界抖动
struct ResolvedCameraView2D {
    bool  valid = false;

    // 相机中心与旋转，直接来自 Transform/Camera。
    float x = 0.f;
    float y = 0.f;
    float rotation = 0.f;

    // authoredZoom 是组件里配置的原始 zoom；
    // zoom 是结合 projectionMode + 当前 viewport 后的实际 zoom。
    float authoredZoom = 1.f;
    float zoom = 1.f;

    // 当前相机用于数学换算的 viewport 尺寸。
    // 第一阶段里它等于窗口尺寸；后续若接入 letterbox / 子 viewport / offscreen，
    // 这里就是那块真正参与世界投影的矩形尺寸。
    int viewportW = 0;
    int viewportH = 0;

    CameraProjectionMode projectionMode = CameraProjectionMode::FixedVertical;
    float referenceViewportHeight = 720.f;

    // 解析后的世界可见范围（单位：world units）。
    // visibleWorldW/H 与 zoom 的关系：
    //   visibleWorldW = viewportW / zoom
    //   visibleWorldH = viewportH / zoom
    float visibleWorldW = 0.f;
    float visibleWorldH = 0.f;
};

struct Sprite {
    TextureHandle texture;
    core::Rect    srcRect;
    int           layer = 0;
    int           sortOrder = 0;
    bool          ySort = false;
    bool          visible = true;
    core::Color   tint  = core::Color::White;
    float         pivotX = 0.5f;
    float         pivotY = 0.5f;
    RenderPass    pass   = RenderPass::World;
    
    GPUHandle     gpuHandle;
    bool          gpuDirty = true;
};

// 按 region ID 给 Sprite 分区染色。Sprite 对应的 base texture 必须有 sibling
// "<path>.id.png"（AssetManager 自动加载）才会生效。
//
// region ID 0 = 背景/不染色；ID 1..MAX_REGIONS-1 = 可染色部位。
// 多源叠加（基础 + 装备覆盖 + 状态 buff）请在 game 层合并后写入 slots，
// 引擎不维护多 Tinting 组件链。
struct Tinting {
    static constexpr int MAX_REGIONS = 16;
    struct Slot {
        bool        enabled = false;          // false = 该 region passthrough（不改变颜色）
        core::Color color   = core::Color::White;  // 与 baseColor 相乘
    };
    std::array<Slot, MAX_REGIONS> slots{};
};

inline float getSortKey(const Transform& tf, const Sprite& spr) {
    if (spr.ySort) {
        return tf.y + spr.sortOrder * 0.001f;
    }
    return spr.sortOrder + tf.y * 0.001f;
}

} // namespace engine
