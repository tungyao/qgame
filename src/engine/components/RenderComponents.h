#pragma once
#include <vector>
#include "../../backend/shared/ResourceHandle.h"
#include "../../core/math/Rect.h"
#include "../../core/math/Color.h"

namespace engine {

struct Transform {
    float x        = 0.f;
    float y        = 0.f;
    float rotation = 0.f;   // 弧度
    float scaleX   = 1.f;
    float scaleY   = 1.f;
};

struct Sprite {
    TextureHandle texture;
    core::Rect    srcRect;   // spritesheet 中的 UV 区域（像素坐标）
    int           layer = 0;
    core::Color   tint  = core::Color::White;
    float         pivotX = 0.5f;  // 锚点，相对于 sprite 宽高的比例
    float         pivotY = 0.5f;
};

struct TileMap {
    int           width    = 0;    // tile 列数
    int           height   = 0;    // tile 行数
    int           tileSize = 16;   // 每个 tile 的像素大小
    int           tilesetCols = 1; // tileset 横向有多少列
    TextureHandle tileset;

    // layers[0]=地面 [1]=物体 [2]=顶层遮挡，-1 表示空 tile
    static constexpr int MAX_LAYERS = 3;
    std::vector<int> layers[MAX_LAYERS];

    int tileAt(int layer, int x, int y) const {
        if (layer < 0 || layer >= MAX_LAYERS) return -1;
        if (x < 0 || x >= width || y < 0 || y >= height) return -1;
        size_t idx = static_cast<size_t>(y) * width + x;
        if (idx >= layers[layer].size()) return -1;
        return layers[layer][idx];
    }
};

// 摄像机（每个场景至多一个有效摄像机）
struct Camera {
    float zoom    = 1.f;
    bool  primary = true;  // 标记主摄像机
};

} // namespace engine
