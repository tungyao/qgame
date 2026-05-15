#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <cstring>
#include <cstdint>
#include <string>
#include <sstream>
#include <vector>

#include <engine/api/GameAPI.h>
#include <engine/components/RenderComponents.h>
#include <engine/components/PhysicsComponents.h>
#include <engine/components/InteractionComponents.h>
#include <engine/components/TextComponent.h>
#include <engine/runtime/EngineConfig.h>
#include <engine/runtime/EngineContext.h>
#include <engine/systems/ISystem.h>
#include <engine/systems/InteractionSystem.h>

// ── Constants ──
constexpr float kCenterX = 640.f;
constexpr float kCenterY = 360.f;

// ── Visual feedback component ──
struct InteractVisual {
    core::Color normal;
    core::Color hover;
    core::Color pressed;
};

// ── Procedural texture helpers ──
static TextureHandle makeCircleTex(engine::GameAPI& api, int size, core::Color color) {
    std::vector<uint8_t> pixels(size * size * 4, 0);
    int cx = size / 2, cy = size / 2, r = size / 2 - 2;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            int dx = x - cx, dy = y - cy;
            if (dx * dx + dy * dy <= r * r) {
                int idx = (y * size + x) * 4;
                pixels[idx + 0] = color.r;
                pixels[idx + 1] = color.g;
                pixels[idx + 2] = color.b;
                pixels[idx + 3] = color.a;
            }
        }
    }
    return api.createTextureFromMemory(pixels.data(), size, size);
}

static TextureHandle makeRectTex(engine::GameAPI& api, int w, int h, core::Color color) {
    std::vector<uint8_t> pixels(w * h * 4, 0);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (x >= 1 && x < w - 1 && y >= 1 && y < h - 1) {
                int idx = (y * w + x) * 4;
                pixels[idx + 0] = color.r;
                pixels[idx + 1] = color.g;
                pixels[idx + 2] = color.b;
                pixels[idx + 3] = color.a;
            }
        }
    }
    return api.createTextureFromMemory(pixels.data(), w, h);
}

static engine::Sprite makeSprite(TextureHandle tex, int srcW, int srcH,
                                 int layer, engine::RenderPass pass,
                                 core::Color tint, float pivotX = 0.5f, float pivotY = 0.5f) {
    engine::Sprite s;
    s.texture = tex;
    s.srcRect = { 0, 0, (float)srcW, (float)srcH };
    s.layer = layer;
    s.pass = pass;
    s.visible = true;
    s.tint = tint;
    s.pivotX = pivotX;
    s.pivotY = pivotY;
    return s;
}

// ── Feedback System: applies visual colors based on InteractionSystem state ──
class InteractFeedbackSystem : public engine::ISystem {
public:
    InteractFeedbackSystem(engine::EngineContext& ctx) : ctx_(ctx) {}

    engine::UpdatePhaseMask phaseMask() const override {
        return engine::updatePhaseBit(engine::UpdatePhase::GameplayPostPhysics);
    }

protected:
    void onGameplayPostPhysicsPhase(float dt) override {
        (void)dt;
        auto& isys = ctx_.systems.get<engine::InteractionSystem>();
        entt::entity hovered = isys.hovered();
        entt::entity pressed = isys.pressed();

        auto view = ctx_.world.view<engine::Interactable, engine::Sprite, InteractVisual>();
        for (auto entity : view) {
            auto& sprite = view.get<engine::Sprite>(entity);
            auto& vis = view.get<InteractVisual>(entity);
            if (entity == pressed)
                sprite.tint = vis.pressed;
            else if (entity == hovered)
                sprite.tint = vis.hover;
            else
                sprite.tint = vis.normal;
        }
    }

private:
    engine::EngineContext& ctx_;
};

// ── Helper: create an interactable entity ──
struct InteractableDef {
    const char* name;
    float x, y;
    int texW, texH;
    core::Color normalColor;
    core::Color hoverColor;
    core::Color pressedColor;
    float interactRadius;
    // Collider (optional)
    bool  hasCollider = false;
    engine::ShapeType shapeType = engine::ShapeType::Box;
    float colW = 0.f, colH = 0.f;
    float colRadius = 0.f;
    float colCapLen = 0.f;
};

static entt::entity makeInteractable(engine::GameAPI& api, engine::EngineContext& ctx,
                                      TextureHandle tex, const InteractableDef& def) {
    entt::entity e = api.spawnEntity();
    api.addComponent(e, engine::Transform{ def.x, def.y });
    ctx.world.emplace<engine::Name>(e, def.name);
    api.addComponent(e, makeSprite(tex, def.texW, def.texH, 4,
                                   engine::RenderPass::World, def.normalColor));
    ctx.world.emplace<engine::Interactable>(e, def.interactRadius);
    ctx.world.emplace<InteractVisual>(e, def.normalColor, def.hoverColor, def.pressedColor);

    if (def.hasCollider) {
        engine::Collider col;
        col.shapeType = def.shapeType;
        col.width = def.colW;
        col.height = def.colH;
        col.radius = def.colRadius;
        col.capsuleLength = def.colCapLen;
        col.isTrigger = true;
        col.layer = engine::COLLISION_LAYER_DEFAULT;
        col.mask = engine::COLLISION_LAYER_ALL;
        api.addComponent(e, col);
    }
    return e;
}

// ── Main ──
int main(int argc, char** argv) {
    bool useOpenGL = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--opengl") == 0) useOpenGL = true;
    }

    // ── Engine init ──
    engine::EngineConfig cfg;
    cfg.windowTitle = "Interaction System Test — QGame Demo9";
    cfg.windowWidth = 1280;
    cfg.windowHeight = 720;
    cfg.vsync = true;
    cfg.debug = true;
    if (useOpenGL) cfg.renderBackend = engine::RenderBackend::OpenGL;

    engine::EngineContext ctx;
    ctx.init(cfg);
    engine::GameAPI api{ ctx };

    // ── Procedural textures ──
    TextureHandle circleRed   = makeCircleTex(api, 64, { 200, 60, 60, 255 });
    TextureHandle circleGreen = makeCircleTex(api, 64, { 60, 200, 60, 255 });
    TextureHandle circleBlue  = makeCircleTex(api, 64, { 60, 60, 200, 255 });
    TextureHandle circlePink  = makeCircleTex(api, 40, { 255, 120, 180, 255 });
    TextureHandle rectOrange  = makeRectTex(api, 64, 64, { 220, 160, 60, 255 });
    TextureHandle rectYellow  = makeRectTex(api, 64, 64, { 220, 220, 60, 255 });
    TextureHandle rectCyan    = makeRectTex(api, 64, 64, { 60, 200, 200, 255 });
    TextureHandle rectPurple  = makeRectTex(api, 80, 80, { 180, 80, 200, 255 });

    // ── World Camera ──
    {
        auto cam = api.spawnEntity();
        api.addComponent(cam, engine::Transform{ kCenterX, kCenterY });
        engine::Camera camComp;
        camComp.primary = true;
        camComp.depth = 0;
        camComp.clear = true;
        camComp.clearColor = core::Color{ 30, 30, 40, 255 };
        camComp.layerMask = engine::renderPassBit(engine::RenderPass::World);
        api.addComponent(cam, camComp);
    }

    // ── UI Camera ──
    {
        auto cam = api.spawnEntity();
        api.addComponent(cam, engine::Transform{ 0.f, 0.f });
        engine::Camera camComp;
        camComp.primary = true;
        camComp.depth = 1;
        camComp.clear = false;
        camComp.layerMask = engine::renderPassBit(engine::RenderPass::UI)
                          | engine::renderPassBit(engine::RenderPass::Screen);
        camComp.projectionMode = engine::CameraProjectionMode::StretchWithWindow;
        api.addComponent(cam, camComp);
    }

    // ── Create interactable objects ──
    // Row 1 (top): radius-based only (no Collider)
    makeInteractable(api, ctx, circleRed, {
        "Red", 250.f, 250.f, 64, 64,
        core::Color{ 200, 60, 60, 255 },      // normal: dark red
        core::Color{ 255, 100, 100, 255 },     // hover: bright red
        core::Color{ 255, 200, 200, 255 },     // pressed: light red
        36.f                                    // interact radius
    });

    makeInteractable(api, ctx, circleGreen, {
        "Green", 450.f, 250.f, 64, 64,
        core::Color{ 60, 200, 60, 255 },
        core::Color{ 100, 255, 100, 255 },
        core::Color{ 200, 255, 200, 255 },
        36.f
    });

    makeInteractable(api, ctx, circleBlue, {
        "Blue", 650.f, 250.f, 64, 64,
        core::Color{ 60, 60, 200, 255 },
        core::Color{ 100, 100, 255, 255 },
        core::Color{ 200, 200, 255, 255 },
        36.f
    });

    // Small pink circle — small radius target
    makeInteractable(api, ctx, circlePink, {
        "Pink", 520.f, 160.f, 40, 40,
        core::Color{ 255, 120, 180, 255 },
        core::Color{ 255, 180, 220, 255 },
        core::Color{ 255, 220, 240, 255 },
        22.f
    });

    // Row 2 (bottom): Collider-based (shape-accurate picking)
    // Orange square with Box Collider
    makeInteractable(api, ctx, rectOrange, {
        "Box", 300.f, 480.f, 64, 64,
        core::Color{ 220, 160, 60, 255 },
        core::Color{ 255, 200, 100, 255 },
        core::Color{ 255, 230, 180, 255 },
        0.f,   // radius unused when Collider present
        true,  engine::ShapeType::Box, 64.f, 64.f
    });

    // Yellow square with Circle Collider (click on edges won't register
    // since circle is inset)
    makeInteractable(api, ctx, rectYellow, {
        "CircleCol", 500.f, 480.f, 64, 64,
        core::Color{ 220, 220, 60, 255 },
        core::Color{ 255, 255, 100, 255 },
        core::Color{ 255, 255, 200, 255 },
        0.f,
        true, engine::ShapeType::Circle, 0.f, 0.f, 28.f
    });

    // Cyan square with Capsule Collider
    makeInteractable(api, ctx, rectCyan, {
        "Capsule", 700.f, 480.f, 64, 64,
        core::Color{ 60, 200, 200, 255 },
        core::Color{ 100, 255, 255, 255 },
        core::Color{ 200, 255, 255, 255 },
        0.f,
        true, engine::ShapeType::Capsule, 0.f, 0.f, 22.f, 50.f
    });

    // Purple big square with Box Collider
    makeInteractable(api, ctx, rectPurple, {
        "BigBox", 900.f, 300.f, 80, 80,
        core::Color{ 180, 80, 200, 255 },
        core::Color{ 220, 130, 240, 255 },
        core::Color{ 240, 200, 255, 255 },
        0.f,
        true, engine::ShapeType::Box, 80.f, 80.f
    });

    // ── Register FeedbackSystem ──
    ctx.systems.registerSystem<InteractFeedbackSystem>(ctx);

    // ── Load baked asset manifest (for debug overlay font) ──
#ifdef QGAME_BAKED_MANIFEST
    api.loadAssetManifest(QGAME_BAKED_MANIFEST);
#endif
    api.enableDebugOverlay();

    // ── 主循环 ──
    while (ctx.scheduler.tick()) {
        if (api.isKeyJustPressed(SDLK_ESCAPE)) {
            api.quit();
            break;
        }

        // 更新调试信息
        auto& isys = ctx.systems.get<engine::InteractionSystem>();
        entt::entity hovered = isys.hovered();
        entt::entity pressed = isys.pressed();

        std::ostringstream oss;
        oss << "Interaction System Demo\n"
            << "Radius-based (top row): Red | Green | Blue | Pink\n"
            << "Collider-based (bottom row): Box | CircleCol | Capsule | BigBox\n\n"
            << "Hovered: ";
        if (hovered != entt::null) {
            auto* name = ctx.world.try_get<engine::Name>(hovered);
            oss << (name ? name->c_str() : "unnamed");
        } else {
            oss << "(none)";
        }
        oss << "\nPressed: ";
        if (pressed != entt::null) {
            auto* name = ctx.world.try_get<engine::Name>(pressed);
            oss << (name ? name->c_str() : "unnamed");
        } else {
            oss << "(none)";
        }
        api.setDebugInfo(oss.str());
    }

    ctx.shutdown();
    return 0;
}
