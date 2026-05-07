#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <backend/renderer/IRenderDevice.h>
#include <engine/api/GameAPI.h>
#include <engine/components/LightComponents.h>
#include <engine/components/RenderComponents.h>
#include <engine/components/TextComponent.h>
#include <engine/runtime/EngineConfig.h>
#include <engine/runtime/EngineContext.h>

#ifndef QGAME_BAKED_MANIFEST
#define QGAME_BAKED_MANIFEST "assets/manifest.baked.json"
#endif

namespace {

constexpr float kPi = 3.14159265358979323846f;

bool hasArg(int argc, char** argv, const char* name) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) return true;
    }
    return false;
}

float clamp01(float v) {
    return std::max(0.f, std::min(1.f, v));
}

std::vector<uint8_t> makeSolidTexture(core::Color c) {
    return {c.r, c.g, c.b, c.a};
}

std::vector<uint8_t> makeRadialTexture(int size, float inner, float outer, uint8_t alpha) {
    // A soft radial texture lets the demo build tree crowns, light glows, and
    // water edges without external art. Color is supplied by Sprite::tint.
    std::vector<uint8_t> px(static_cast<size_t>(size) * size * 4, 0);
    const float center = (size - 1) * 0.5f;
    const float invRadius = center > 0.f ? 1.f / center : 1.f;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float dx = (x - center) * invRadius;
            const float dy = (y - center) * invRadius;
            const float d = std::sqrt(dx * dx + dy * dy);
            const float a = 1.f - clamp01((d - inner) / std::max(outer - inner, 0.001f));
            const int i = (y * size + x) * 4;
            px[i + 0] = 255;
            px[i + 1] = 255;
            px[i + 2] = 255;
            px[i + 3] = static_cast<uint8_t>(alpha * a * a);
        }
    }
    return px;
}

entt::entity makeSpritePx(engine::GameAPI& api,
                          TextureHandle texture,
                          float texW,
                          float texH,
                          float x,
                          float y,
                          float w,
                          float h,
                          core::Color tint,
                          int layer) {
    auto e = api.spawnEntity();
    api.addComponent(e, engine::Transform{x, y, 0.f, w / texW, h / texH});

    engine::Sprite s{};
    s.texture = texture;
    s.srcRect = core::Rect{0.f, 0.f, texW, texH};
    s.tint = tint;
    s.layer = layer;
    s.pass = engine::RenderPass::World;
    s.pivotX = 0.5f;
    s.pivotY = 0.5f;
    api.addComponent(e, s);
    return e;
}

entt::entity makeRect(engine::GameAPI& api,
                      TextureHandle whiteTex,
                      float x,
                      float y,
                      float w,
                      float h,
                      core::Color tint,
                      int layer) {
    return makeSpritePx(api, whiteTex, 1.f, 1.f, x, y, w, h, tint, layer);
}

entt::entity makeText(engine::GameAPI& api,
                      engine::FontHandle font,
                      const std::string& value,
                      float x,
                      float y,
                      float size,
                      core::Color color) {
    auto e = api.spawnEntity();
    api.addComponent(e, engine::Transform{x, y});

    engine::TextComponent text{};
    text.text = value;
    text.font = font;
    text.fontSize = size;
    text.color = color;
    text.pass = engine::RenderPass::UI;
    text.layer = 60;
    api.addComponent(e, text);
    return e;
}

void addAABBOccluder(engine::GameAPI& api, entt::entity e, float w, float h, float opacity = 1.f) {
    // Tree trunks are the hard blockers in this 2.5D test. Crowns remain visual
    // mass only, so the shadow shape is stable and easy to reason about.
    engine::LightOccluder2D occ{};
    occ.shape = engine::LightOccluder2D::Shape::AABB;
    occ.width = w;
    occ.height = h;
    occ.opacity = opacity;
    api.addComponent(e, occ);
}

void addWaterReflector(engine::GameAPI& api, entt::entity e, float w, float h) {
    // Reflector2D marks the pond as an SSR receiver. The lighting compute pass
    // mirrors the previously rendered World sceneColor here, so tree sprites
    // and the moving lantern can reflect without duplicate "mirror" entities.
    engine::Reflector2D refl{};
    refl.shape = engine::Reflector2D::Shape::AABB;
    refl.width = w;
    refl.height = h;
    refl.reflectivity = 1.0f;
    refl.roughness = 0.16f;
    refl.tint = core::Color{160, 210, 255, 255};
    api.addComponent(e, refl);
}

struct TreeVisuals {
    float x;
    float trunkTopY;
    float trunkH;
    float crownW;
    float crownH;
    core::Color crownColor;
};

void makeTree(engine::GameAPI& api,
              TextureHandle whiteTex,
              TextureHandle crownTex,
              const TreeVisuals& t,
              float /*waterY*/) {
    const float trunkW = 24.f;
    const float trunkY = t.trunkTopY + t.trunkH * 0.5f;
    makeRect(api, whiteTex, t.x, trunkY, trunkW, t.trunkH,
             core::Color{66, 45, 30, 255}, 14);
    auto trunkOcc = makeRect(api, whiteTex, t.x, trunkY, trunkW + 8.f, t.trunkH,
                             core::Color{22, 18, 16, 60}, 13);
    addAABBOccluder(api, trunkOcc, trunkW + 8.f, t.trunkH, 1.f);

    makeSpritePx(api, crownTex, 128.f, 128.f, t.x, t.trunkTopY - 18.f,
                 t.crownW, t.crownH, t.crownColor, 15);
    makeSpritePx(api, crownTex, 128.f, 128.f, t.x - t.crownW * 0.22f, t.trunkTopY + 8.f,
                 t.crownW * 0.66f, t.crownH * 0.66f, core::Color{24, 70, 44, 230}, 15);
    makeSpritePx(api, crownTex, 128.f, 128.f, t.x + t.crownW * 0.22f, t.trunkTopY + 12.f,
                 t.crownW * 0.62f, t.crownH * 0.62f, core::Color{18, 58, 38, 225}, 15);

    // No explicit mirrored sprites here: demo5 validates sceneColor SSR. The
    // water Reflector2D samples the offscreen World color target and mirrors
    // these actual tree sprites into the pond.
}

} // namespace

int main(int argc, char** argv) {
    const bool useOpenGL = hasArg(argc, argv, "--opengl");
    const bool autoExit = hasArg(argc, argv, "--auto-exit");

    engine::EngineConfig cfg;
    cfg.windowTitle = "QGame Demo5 - 2.5D Forest Shadows and Water Reflections";
    cfg.windowWidth = 1280;
    cfg.windowHeight = 720;
    cfg.vsync = true;
    if (useOpenGL) {
        cfg.renderBackend = engine::RenderBackend::OpenGL;
    }

    engine::EngineContext ctx;
    ctx.init(cfg);
    engine::GameAPI api{ctx};

    api.loadAssetManifest(QGAME_BAKED_MANIFEST);
    engine::FontHandle font = api.loadFontById("font.demo.main");

    const auto whitePx = makeSolidTexture(core::Color::White);
    const auto crownPx = makeRadialTexture(128, 0.15f, 1.0f, 255);
    const auto glowPx = makeRadialTexture(256, 0.0f, 1.0f, 210);
    const auto waterPx = makeRadialTexture(128, 0.22f, 1.0f, 230);
    TextureHandle whiteTex = api.createTextureFromMemory(whitePx.data(), 1, 1);
    TextureHandle crownTex = api.createTextureFromMemory(crownPx.data(), 128, 128);
    TextureHandle glowTex = api.createTextureFromMemory(glowPx.data(), 256, 256);
    TextureHandle waterTex = api.createTextureFromMemory(waterPx.data(), 128, 128);

    auto worldCam = api.spawnEntity();
    api.addComponent(worldCam, engine::Transform{640.f, 360.f});
    engine::Camera wcam{};
    wcam.zoom = 1.f;
    wcam.primary = true;
    wcam.depth = 0;
    wcam.layerMask = engine::renderPassBit(engine::RenderPass::World);
    wcam.clear = true;
    wcam.clearColor = core::Color{8, 12, 22, 255};
    wcam.cullEnabled = false;
    api.addComponent(worldCam, wcam);

    auto uiCam = api.spawnEntity();
    api.addComponent(uiCam, engine::Transform{640.f, 360.f});
    engine::Camera ucam{};
    ucam.zoom = 1.f;
    ucam.primary = true;
    ucam.depth = 1;
    ucam.layerMask = engine::renderPassBit(engine::RenderPass::UI);
    ucam.clear = false;
    ucam.cullEnabled = false;
    api.addComponent(uiCam, ucam);

    auto env = api.spawnEntity();
    engine::Environment2D environment{};
    environment.ambientColor = core::Color{20, 30, 54, 255};
    environment.ambientIntensity = 0.16f;
    environment.exposure = 1.02f;
    environment.bloomThreshold = 0.8f;
    environment.wetness = 1.0f;
    api.addComponent(env, environment);

    // Layered night background and ground bands.
    makeRect(api, whiteTex, 640.f, 360.f, 1280.f, 720.f, core::Color{8, 12, 22, 255}, 0);
    makeRect(api, whiteTex, 640.f, 465.f, 1280.f, 310.f, core::Color{18, 28, 34, 255}, 1);
    makeRect(api, whiteTex, 640.f, 630.f, 1280.f, 180.f, core::Color{14, 22, 26, 255}, 1);
    makeRect(api, whiteTex, 640.f, 430.f, 1280.f, 10.f, core::Color{42, 58, 62, 255}, 2);

    const float waterX = 710.f;
    const float waterY = 535.f;
    const float waterW = 650.f;
    const float waterH = 210.f;
    auto water = makeSpritePx(api, waterTex, 128.f, 128.f, waterX, waterY, waterW, waterH,
                              core::Color{44, 108, 146, 190}, 5);
    addWaterReflector(api, water, waterW, waterH);
    makeSpritePx(api, waterTex, 128.f, 128.f, waterX + 28.f, waterY - 16.f, waterW * 0.86f, waterH * 0.64f,
                 core::Color{80, 150, 190, 72}, 7);
    makeRect(api, whiteTex, waterX, waterY - waterH * 0.48f, waterW * 0.74f, 5.f,
             core::Color{115, 178, 210, 140}, 8);

    const TreeVisuals trees[] = {
        {250.f, 315.f, 165.f, 128.f, 122.f, core::Color{22, 80, 46, 245}},
        {365.f, 285.f, 195.f, 150.f, 142.f, core::Color{18, 72, 42, 250}},
        {500.f, 330.f, 145.f, 118.f, 114.f, core::Color{28, 92, 54, 240}},
        {650.f, 300.f, 185.f, 142.f, 136.f, core::Color{20, 82, 50, 245}},
        {815.f, 325.f, 160.f, 132.f, 126.f, core::Color{26, 90, 58, 238}},
        {990.f, 292.f, 192.f, 154.f, 146.f, core::Color{16, 68, 42, 250}},
        {1110.f, 344.f, 135.f, 116.f, 108.f, core::Color{28, 76, 50, 236}},
    };
    for (const TreeVisuals& t : trees) {
        makeTree(api, whiteTex, crownTex, t, waterY - waterH * 0.48f);
    }

    // A moving lantern travels through the forest. It casts shadows from tree
    // trunks and reflects on the authored water Reflector2D.
    auto lantern = makeSpritePx(api, glowTex, 256.f, 256.f, 260.f, 335.f, 150.f, 150.f,
                                core::Color{255, 194, 94, 150}, 12);
    engine::Light2D light{};
    light.type = engine::Light2DType::Point;
    light.color = core::Color{255, 185, 82, 255};
    light.radius = 430.f;
    light.intensity = 1.55f;
    light.softness = 34.f;
    light.castsShadow = true;
    api.addComponent(lantern, light);
    auto lanternCore = makeRect(api, whiteTex, 260.f, 335.f, 18.f, 18.f,
                                core::Color{255, 238, 166, 255}, 18);
    makeText(api, font, "Demo5: 2.5D forest - shadows and water reflection", 28.f, 34.f, 24.f,
             core::Color{230, 240, 255, 255});
    makeText(api, font,
             "Moving lantern casts tree shadows; water uses sceneColor SSR for trees and light.",
             28.f, 68.f, 14.f, core::Color{176, 198, 220, 255});
    auto statusText = makeText(api, font, "", 28.f, 96.f, 14.f,
                               core::Color{150, 226, 180, 255});
    makeText(api, font, "ESC quit | --opengl fallback | --auto-exit smoke",
             28.f, 680.f, 14.f, core::Color{150, 160, 178, 255});

    float t = 0.f;
    float statusTimer = 0.f;
    while (ctx.scheduler.tick()) {
        const float dt = ctx.scheduler.deltaTime();
        t += dt;
        statusTimer += dt;

        const float path = (std::sin(t * 0.32f) * 0.5f + 0.5f);
        const float x = 430.f + path * 520.f;
        const float y = 315.f + std::sin(t * 0.72f) * 26.f;
        api.patchComponent<engine::Transform>(lantern, [&](engine::Transform& tf) {
            tf.x = x;
            tf.y = y;
        });
        api.patchComponent<engine::Transform>(lanternCore, [&](engine::Transform& tf) {
            tf.x = x;
            tf.y = y;
        });

        if (statusTimer >= 0.25f) {
            statusTimer = 0.f;
            const backend::RendererCapabilities& caps = ctx.renderDevice().capabilities();
            const backend::RenderFrameStats& stats = ctx.renderDevice().frameStats();
            char buf[320];
            std::snprintf(buf, sizeof(buf),
                          "backend=%s | lighting2D=%d | lights=%u occluders=%u reflectors=%u | lantern=(%.0f, %.0f)",
                          caps.backendName,
                          caps.supportsLighting2D ? 1 : 0,
                          stats.light2DCount,
                          stats.occluder2DCount,
                          stats.reflector2DCount,
                          x,
                          y);
            api.getComponent<engine::TextComponent>(statusText).text = buf;
        }

        if (api.isKeyJustPressed(SDLK_ESCAPE)) {
            api.quit();
            break;
        }
        if (autoExit && t >= 2.0f) {
            api.quit();
            break;
        }
    }

    ctx.shutdown();
    return 0;
}
