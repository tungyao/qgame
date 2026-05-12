#pragma once
#include "ISystem.h"
#include "../runtime/EngineContext.h"
#include "../../backend/renderer/CommandBuffer.h"

namespace engine {

struct DebugOverlayComponent {
    float updateTimer = 0.0f;
    float fpsAccumulator = 0.0f;
    int fpsFrames = 0;
};

struct DebugRect {
    float x, y, w, h;
    core::Color color = core::Color::Green;
};

class DebugOverlaySystem final : public ISystem {
public:
    explicit DebugOverlaySystem(EngineContext& ctx);
    UpdatePhaseMask phaseMask() const override;
    void init() override;
    void shutdown() override;
    bool runPhase(UpdatePhase phase, float dt) override;

    // Called by RenderSystem to emit debug draw commands into the command buffer
    void emitDebugDrawCommands(backend::CommandBuffer& cb);

    bool debugEnabled() const { return debugEnabled_; }
    void setDebugEnabled(bool enabled);

    // Public access to debug rects — other systems can add custom debug draws
    std::vector<DebugRect>& debugRects() { return debugRects_; }

    // Custom info lines appended below the engine stats in the overlay text
    void setCustomInfo(const std::string& info) { customInfo_ = info; }
    const std::string& customInfo() const { return customInfo_; }

    float lineThickness() const { return lineThickness_; }
    void setLineThickness(float t) { lineThickness_ = t; }

private:
    void collectColliderDebugRects();
    void ensureDebugTexture();

    EngineContext& ctx_;
    bool debugEnabled_ = false;
    TextureHandle debugWhiteTexture_; // 1x1 white pixel texture for debug wireframe lines
    std::vector<DebugRect> debugRects_;     // merged debug rects for current frame
    std::vector<DebugRect> colliderRects_;  // collected from Collider components each frame
    std::string customInfo_;                // game-specific info appended below engine stats
    float lineThickness_ = 2.0f;
};

} // namespace engine
