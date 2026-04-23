#pragma once
#include <vector>
#include <variant>
#include "../shared/ResourceHandle.h"
#include "../../core/math/Rect.h"
#include "../../core/math/Color.h"

namespace backend {

struct CameraData {
    float x = 0.f, y = 0.f;
    float zoom = 1.f;
    int   viewportW = 0, viewportH = 0;
};

struct DrawSpriteCmd {
    TextureHandle texture;
    float         x, y;
    float         rotation = 0.f;
    float         scaleX   = 1.f, scaleY = 1.f;
    core::Rect    srcRect;
    int           layer = 0;
    core::Color   tint  = core::Color::White;
};

struct DrawTileCmd {
    TextureHandle tileset;
    int           tileId;
    int           gridX, gridY;
    int           tileSize = 16;
    int           layer    = 0;
};

struct SetCameraCmd {
    CameraData camera;
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
    void setCamera(const CameraData& cam);
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
