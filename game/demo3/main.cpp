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
    // A single white/color pixel is enough for every rectangle in this demo.
    // Sprite scale turns it into floors, walls, water, and debug panels.
    return {c.r, c.g, c.b, c.a};
}

std::vector<uint8_t> makeRadialLightTexture(int size) {
    // This texture is only a debug visualizer for Light2D data. It deliberately
    // is not part of the future lighting model; the compute pass will generate
    // light into a lighting texture instead of drawing these glow sprites.
    std::vector<uint8_t> px(static_cast<size_t>(size) * size * 4, 0);
    const float center = (size - 1) * 0.5f;
    const float invRadius = center > 0.f ? 1.f / center : 1.f;

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float dx = (x - center) * invRadius;
            const float dy = (y - center) * invRadius;
            const float d = std::sqrt(dx * dx + dy * dy);
            const float falloff = clamp01(1.f - d);
            const float alpha = falloff * falloff;
            const int i = (y * size + x) * 4;
            px[i + 0] = 255;
            px[i + 1] = 255;
            px[i + 2] = 255;
            px[i + 3] = static_cast<uint8_t>(180.f * alpha);
        }
    }
    return px;
}

entt::entity makeSprite(engine::GameAPI& api,
                        TextureHandle texture,
                        float x,
                        float y,
                        float w,
                        float h,
                        core::Color tint,
                        int layer,
                        float srcW = 1.f,
                        float srcH = 1.f) {
    auto e = api.spawnEntity();
    api.addComponent(e, engine::Transform{x, y, 0.f, w, h});

    engine::Sprite s{};
    s.texture = texture;
    // Solid debug rectangles use a 1x1 texture, while glow sprites use a full
    // 256x256 radial texture. Keeping the source size explicit prevents the
    // glow from accidentally sampling only the transparent corner pixel.
    s.srcRect = core::Rect{0.f, 0.f, srcW, srcH};
    s.tint = tint;
    s.layer = layer;
    s.pass = engine::RenderPass::World;
    s.pivotX = 0.5f;
    s.pivotY = 0.5f;
    api.addComponent(e, s);
    return e;
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
    text.layer = 50;
    api.addComponent(e, text);
    return e;
}

} // namespace

int main(int argc, char** argv) {
    const bool useOpenGL = hasArg(argc, argv, "--opengl");
    const bool autoExit = hasArg(argc, argv, "--auto-exit");

    engine::EngineConfig cfg;
    cfg.windowTitle = "QGame Demo3 - Vulkan 2D Lighting L3";
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

    const auto solidPx = makeSolidTexture(core::Color::White);
    const auto glowPx = makeRadialLightTexture(256);
    TextureHandle whiteTex = api.createTextureFromMemory(solidPx.data(), 1, 1);
    TextureHandle glowTex = api.createTextureFromMemory(glowPx.data(), 256, 256);

    // World camera owns all world-space demo geometry. It clears to a cold,
    // low-light color so the debug light sprites read like a night scene.
    auto worldCam = api.spawnEntity();
    api.addComponent(worldCam, engine::Transform{640.f, 360.f});
    engine::Camera wcam{};
    wcam.zoom = 1.f;
    wcam.primary = true;
    wcam.depth = 0;
    wcam.layerMask = engine::renderPassBit(engine::RenderPass::World);
    wcam.clear = true;
    wcam.clearColor = core::Color{10, 14, 24, 255};
    wcam.cullEnabled = false;
    api.addComponent(worldCam, wcam);

    // UI camera is separate on purpose: the lighting plan keeps Text/UI outside
    // World lighting so diagnostics stay readable in very dark scenes.
    auto uiCam = api.spawnEntity();
    // UI text commands still go through the same camera transform math as world
    // commands. Placing the UI camera at the viewport center makes UI positions
    // behave like top-left pixel coordinates for this demo.
    api.addComponent(uiCam, engine::Transform{640.f, 360.f});
    engine::Camera ucam{};
    ucam.zoom = 1.f;
    ucam.primary = true;
    ucam.depth = 1;
    ucam.layerMask = engine::renderPassBit(engine::RenderPass::UI);
    ucam.clear = false;
    ucam.cullEnabled = false;
    api.addComponent(uiCam, ucam);

    // One environment entity is enough for the scene. Later RenderSystem will
    // select the first enabled Environment2D and push it into lighting uniforms.
    auto env = api.spawnEntity();
    api.addComponent(env, engine::Name{"lighting.environment.night"});
    engine::Environment2D environment{};
    environment.ambientColor = core::Color{24, 32, 52, 255};
    environment.ambientIntensity = 0.18f;
    environment.exposure = 0.9f;
    environment.bloomThreshold = 0.85f;
    environment.wetness = 0.65f;
    api.addComponent(env, environment);

    // Background/floor strips. They are intentionally simple colored geometry:
    // this demo proves the no-extra-lighting-texture workflow.
    makeSprite(api, whiteTex, 640.f, 360.f, 1280.f, 720.f, core::Color{18, 24, 38, 255}, 0);
    makeSprite(api, whiteTex, 640.f, 620.f, 1180.f, 80.f, core::Color{30, 38, 50, 255}, 1);
    makeSprite(api, whiteTex, 640.f, 520.f, 1180.f, 16.f, core::Color{52, 62, 72, 255}, 2);

    // Reflector2D AABB represents a wet street / shallow water patch. The blue
    // translucent sprite is only a debug overlay; Reflector2D is the real data.
    auto wetStreet = makeSprite(api, whiteTex, 650.f, 610.f, 760.f, 72.f,
                                core::Color{70, 120, 150, 95}, 3);
    engine::Reflector2D wet{};
    wet.shape = engine::Reflector2D::Shape::AABB;
    wet.width = 760.f;
    wet.height = 72.f;
    wet.reflectivity = 0.72f;
    wet.roughness = 0.42f;
    wet.tint = core::Color{120, 170, 210, 255};
    api.addComponent(wetStreet, wet);

    // Occluders are solid blocks for now. L1/L2 will expand these AABBs into
    // four GPU line segments before the compute ray-casting pass runs.
    auto wallA = makeSprite(api, whiteTex, 450.f, 380.f, 80.f, 240.f,
                            core::Color{34, 36, 42, 255}, 10);
    engine::LightOccluder2D occA{};
    occA.shape = engine::LightOccluder2D::Shape::AABB;
    occA.width = 80.f;
    occA.height = 240.f;
    occA.opacity = 1.f;
    api.addComponent(wallA, occA);

    auto wallB = makeSprite(api, whiteTex, 820.f, 330.f, 140.f, 110.f,
                            core::Color{42, 38, 36, 255}, 10);
    engine::LightOccluder2D occB{};
    occB.shape = engine::LightOccluder2D::Shape::AABB;
    occB.width = 140.f;
    occB.height = 110.f;
    occB.opacity = 0.9f;
    api.addComponent(wallB, occB);

    // Warm street lamp: the visible glow sprite and Light2D live on the same
    // entity so transform animation automatically moves both debug and data.
    auto lamp = makeSprite(api, glowTex, 300.f, 250.f, 2.4f, 2.4f,
                           core::Color{255, 190, 95, 170}, 5, 256.f, 256.f);
    engine::Light2D lampLight{};
    lampLight.type = engine::Light2DType::Point;
    lampLight.color = core::Color{255, 186, 92, 255};
    lampLight.radius = 310.f;
    lampLight.intensity = 1.25f;
    lampLight.softness = 32.f;
    lampLight.castsShadow = true;
    api.addComponent(lamp, lampLight);
    makeSprite(api, whiteTex, 300.f, 395.f, 18.f, 290.f, core::Color{50, 45, 38, 255}, 11);
    makeSprite(api, whiteTex, 300.f, 238.f, 42.f, 18.f, core::Color{220, 170, 80, 255}, 12);

    // Moving cool light: useful for verifying that the data path handles
    // dynamic transforms. The future compute pass should only need the updated
    // Transform plus Light2D; no texture authoring is involved.
    auto patrol = makeSprite(api, glowTex, 930.f, 255.f, 2.0f, 2.0f,
                             core::Color{90, 170, 255, 150}, 5, 256.f, 256.f);
    engine::Light2D patrolLight{};
    patrolLight.type = engine::Light2DType::Point;
    patrolLight.color = core::Color{90, 170, 255, 255};
    patrolLight.radius = 250.f;
    patrolLight.intensity = 0.95f;
    patrolLight.softness = 24.f;
    patrolLight.castsShadow = true;
    api.addComponent(patrol, patrolLight);

    // L3 tiled-culling stress field: 30 small lamps plus the two hand-placed
    // hero lights above gives exactly 32 Light2D components. They are spread
    // across the screen so most 32x32 tiles see only a few lights; that makes
    // the benefit of the GPU tile list visible in captures and keeps the scene
    // a practical regression test instead of a synthetic full-screen overlap.
    std::vector<entt::entity> l3Lights;
    l3Lights.reserve(30);
    const core::Color l3Palette[] = {
        core::Color{255, 132, 92, 150},
        core::Color{255, 218, 116, 145},
        core::Color{104, 210, 255, 140},
        core::Color{155, 132, 255, 135},
        core::Color{118, 255, 184, 135},
    };
    for (int i = 0; i < 30; ++i) {
        const int col = i % 10;
        const int row = i / 10;
        const float x = 155.f + col * 108.f;
        const float y = 145.f + row * 135.f;
        const core::Color tint = l3Palette[i % 5];
        auto e = makeSprite(api, glowTex, x, y, 1.05f, 1.05f, tint, 4, 256.f, 256.f);

        engine::Light2D tiny{};
        tiny.type = engine::Light2DType::Point;
        tiny.color = core::Color{tint.r, tint.g, tint.b, 255};
        tiny.radius = 118.f + static_cast<float>((i % 4) * 14);
        tiny.intensity = 0.42f + static_cast<float>(i % 3) * 0.08f;
        tiny.softness = 12.f;
        tiny.castsShadow = true;
        tiny.visible = true;
        api.addComponent(e, tiny);
        l3Lights.push_back(e);
    }

    // Segment reflector marker: a thin line at the water edge. This gives the
    // future reflection pass both AABB and segment cases to test.
    auto waterEdge = makeSprite(api, whiteTex, 650.f, 570.f, 720.f, 4.f,
                                core::Color{140, 210, 255, 155}, 13);
    engine::Reflector2D edge{};
    edge.shape = engine::Reflector2D::Shape::Segment;
    edge.ax = -360.f;
    edge.ay = 0.f;
    edge.bx = 360.f;
    edge.by = 0.f;
    edge.reflectivity = 0.55f;
    edge.roughness = 0.2f;
    edge.tint = core::Color{130, 200, 255, 255};
    api.addComponent(waterEdge, edge);

    auto title = makeText(api, font, "Demo3: 2D Lighting L3 - 32 Lights + Tiled Culling", 28.f, 34.f, 24.f,
                          core::Color{235, 242, 255, 255});
    (void)title;
    makeText(api, font,
             "Glow sprites are debug markers; compute now draws dynamic light and hard shadows together.",
             28.f, 66.f, 14.f, core::Color{170, 188, 210, 255});
    makeText(api, font,
             "Real test data: 32 Light2D + LightOccluder2D + Reflector2D + Environment2D.",
             28.f, 88.f, 14.f, core::Color{170, 188, 210, 255});
    auto statusText = makeText(api, font, "", 28.f, 116.f, 14.f,
                               core::Color{145, 225, 180, 255});
    makeText(api, font, "ESC quit | --opengl fallback | --auto-exit smoke",
             28.f, 680.f, 14.f, core::Color{150, 158, 174, 255});

    float t = 0.f;
    while (ctx.scheduler.tick()) {
        const float dt = ctx.scheduler.deltaTime();
        t += dt;

        api.patchComponent<engine::Transform>(patrol, [&](engine::Transform& tf) {
            tf.x = 930.f + std::cos(t * 0.85f) * 130.f;
            tf.y = 255.f + std::sin(t * 1.15f) * 58.f;
        });

        api.patchComponent<engine::Transform>(lamp, [&](engine::Transform& tf) {
            // Tiny motion makes the warm light feel alive and keeps Transform
            // dirty tracking exercised without needing a texture animation.
            tf.scaleX = 2.4f + std::sin(t * 5.0f) * 0.04f;
            tf.scaleY = 2.4f + std::sin(t * 5.0f) * 0.04f;
        });

        for (size_t i = 0; i < l3Lights.size(); ++i) {
            api.patchComponent<engine::Transform>(l3Lights[i], [&](engine::Transform& tf) {
                // Small orbital motion continuously changes tile membership,
                // which exercises the culling pass every frame without making
                // the lights hard to visually track.
                const float phase = static_cast<float>(i) * 0.73f;
                tf.x += std::cos(t * 0.9f + phase) * dt * 9.f;
                tf.y += std::sin(t * 1.1f + phase) * dt * 7.f;
            });
        }

        const backend::RendererCapabilities& caps = ctx.renderDevice().capabilities();
        const backend::RenderFrameStats& stats = ctx.renderDevice().frameStats();
        char buf[320];
        std::snprintf(buf, sizeof(buf),
                      "backend=%s | compute=%d storageBuffer=%d storageTexture=%d lighting2D=%d dispatch=%u | lights=%u occluders=%u reflectors=%u env=%u",
                      caps.backendName,
                      caps.supportsCompute ? 1 : 0,
                      caps.supportsStorageBuffer ? 1 : 0,
                      caps.supportsStorageTexture ? 1 : 0,
                      caps.supportsLighting2D ? 1 : 0,
                      stats.computeDispatchCount,
                      stats.light2DCount,
                      stats.occluder2DCount,
                      stats.reflector2DCount,
                      stats.environment2DCount);
        api.getComponent<engine::TextComponent>(statusText).text = buf;

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
