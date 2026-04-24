#pragma once
#include <vector>
#include <variant>
#include "../shared/ResourceHandle.h"
#include "../../core/math/Rect.h"
#include "../../core/math/Color.h"
#include "../../engine/components/RenderComponents.h"

namespace backend {

struct CameraData {
    float x = 0.f, y = 0.f;
    float zoom = 1.f;
    float rotation = 0.f;
    int   viewportW = 0, viewportH = 0;

    float worldToScreenX(float wx) const {
        return (wx - x) * zoom + viewportW * 0.5f;
    }
    float worldToScreenY(float wy) const {
        return (wy - y) * zoom + viewportH * 0.5f;
    }
    float screenToWorldX(float sx) const {
        return (sx - viewportW * 0.5f) / zoom + x;
    }
    float screenToWorldY(float sy) const {
        return (sy - viewportH * 0.5f) / zoom + y;
    }
    void worldToScreen(float wx, float wy, float& outSx, float& outSy) const {
        outSx = worldToScreenX(wx);
        outSy = worldToScreenY(wy);
    }
    void screenToWorld(float sx, float sy, float& outWx, float& outWy) const {
        outWx = screenToWorldX(sx);
        outWy = screenToWorldY(sy);
    }
};

struct DrawSpriteCmd {
    TextureHandle        texture;
    float                x, y;
    float                rotation = 0.f;
    float                scaleX   = 1.f, scaleY = 1.f;
    float                pivotX   = 0.5f, pivotY = 0.5f;
    core::Rect           srcRect;
    int                  layer = 0;
    int                  sortKey = 0;
    bool                 ySort = false;
    core::Color          tint  = core::Color::White;
    engine::RenderPass   pass  = engine::RenderPass::World;
};

struct DrawTileCmd {
    TextureHandle        tileset;
    int                  tileId;
    int                  gridX, gridY;
    int                  tileSize = 16;
    int                  layer    = 0;
    int                  sortKey  = 0;
    bool                 ySort    = false;
    engine::RenderPass   pass     = engine::RenderPass::World;
};

struct SetCameraCmd {
    CameraData camera;
    engine::RenderPass pass = engine::RenderPass::World;
};

struct ClearCmd {
    core::Color color = core::Color::Black;
};

using RenderCmd = std::variant<DrawSpriteCmd, DrawTileCmd, SetCameraCmd, ClearCmd>;

class CommandBuffer {
public:
    void begin();
    void end();

    void clear(const core::Color& color = core::Color::Black);
    void setCamera(const CameraData& cam, engine::RenderPass pass = engine::RenderPass::World);
    void drawSprite(const DrawSpriteCmd& cmd);
    void drawTile(const DrawTileCmd& cmd);

    const std::vector<RenderCmd>& commands() const { return cmds_; }
    bool isRecording() const { return recording_; }
    void reset();


private:
    std::vector<RenderCmd> cmds_;
    bool recording_ = false;
};

} // namespace backend
