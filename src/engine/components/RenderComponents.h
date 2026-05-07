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

struct TileMap {
    struct Tileset {
        TextureHandle texture;
        int firstGid = 0;      // 全局 tile id 起点
        int count    = 0;      // tile 数量
        int columns  = 1;      // tileset 横向 tile 数
        std::vector<uint8_t> collision; // 每格碰撞属性，0=空，非0=实心
    };

    struct Layer {
        std::string name;
        std::vector<int> tiles; // 全局 tile id，-1 表示空
        bool visible = true;
        bool collidable = true; // 是否参与 TileMap 静态碰撞；独立于 visible，允许隐藏碰撞层
        int renderLayer = 0;
    };

    int width    = 0;  // tile 列数
    int height   = 0;  // tile 行数
    int tileSize = 16; // 每个 tile 的像素大小
    std::vector<Tileset> tilesets;
    std::vector<Layer>   layers;

    int tileAt(int layer, int x, int y) const {
        if (layer < 0 || layer >= static_cast<int>(layers.size())) return -1;
        if (x < 0 || x >= width || y < 0 || y >= height) return -1;
        size_t idx = static_cast<size_t>(y) * width + x;
        if (idx >= layers[layer].tiles.size()) return -1;
        return layers[layer].tiles[idx];
    }

    const Tileset* tilesetForGid(int gid) const {
        for (const auto& ts : tilesets) {
            if (gid >= ts.firstGid && gid < ts.firstGid + ts.count) return &ts;
        }
        return nullptr;
    }

    int localTileId(int gid) const {
        const Tileset* ts = tilesetForGid(gid);
        return ts ? gid - ts->firstGid : -1;
    }

    bool tileSolid(int gid) const {
        const Tileset* ts = tilesetForGid(gid);
        if (!ts) return false;
        const int local = gid - ts->firstGid;
        if (local < 0 || local >= static_cast<int>(ts->collision.size())) return false;
        return ts->collision[static_cast<size_t>(local)] != 0;
    }

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
