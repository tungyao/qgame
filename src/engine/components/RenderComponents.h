#pragma once
#include <array>
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
 * - TileMap 组件本体使用 qgame.tilemap.v1 结构；完整资源包使用
 *   qgame.tilemap.engine-package。
 * - 字段名保持短名以减少地图文件体积：
 *   w=width, h=height, ts=tileSize, cols=tileset columns。
 * - tools/tilemap_editor.html 应按这些常量和字段语义导出，避免编辑器和运行时
 *   对 visible/collidable/renderLayer 的含义产生分歧。
 */
struct TileMap {
    static constexpr int EMPTY_GID = -1;
    static constexpr int FORMAT_VERSION = 1;
    static constexpr const char* FORMAT_TYPE = "qgame.tilemap.v1";
    static constexpr const char* ENGINE_PACKAGE_TYPE = "qgame.tilemap.engine-package";

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
     *   "collision": [0, 1, ...]
     * }
     */
    struct Tileset {
        TextureHandle texture;              // 渲染用 atlas 纹理；加载器负责把 JSON tex/assetId 转成句柄
        int firstGid = 0;                   // 全局 tile id 起点，包含该值
        int count    = 0;                   // tile 数量；有效 gid 范围是 [firstGid, firstGid + count)
        int columns  = 1;                   // atlas 横向 tile 数；localId % columns 得到 atlas x
        std::vector<uint8_t> collision;     // 每个 local tile 的碰撞标记；0=非实心，非0=实心
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
     * 判断某个 gid 在 Tileset collision 表中是否为实心 tile。
     */
    bool tileSolid(int gid) const {
        const Tileset* ts = tilesetForGid(gid);
        if (!ts) return false;
        const int local = gid - ts->firstGid;
        if (local < 0 || local >= static_cast<int>(ts->collision.size())) return false;
        return ts->collision[static_cast<size_t>(local)] != 0;
    }

    /**
     * 判断某个 cell 是否有任意 collidable 图层放置了实心 tile。
     */
    bool solidTileAt(int x, int y) const {
        for (int layer = 0; layer < static_cast<int>(layers.size()); ++layer) {
            if (!layers[layer].collidable) continue;
            if (tileSolid(tileAt(layer, x, y))) return true;
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

    // —— 兼容字段（暂保留，未来移除）——————————————————————————
    RenderPass  renderPass  = RenderPass::World;
    CameraType  type        = CameraType::World;
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
