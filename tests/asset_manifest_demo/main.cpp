#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include <engine/api/GameAPI.h>
#include <engine/components/RenderComponents.h>
#include <engine/components/TextComponent.h>
#include <engine/runtime/EngineConfig.h>
#include <engine/runtime/EngineContext.h>

#ifndef QGAME_DEMO_MANIFEST
#define QGAME_DEMO_MANIFEST "assets/manifest.json"
#endif

namespace {

bool hasArg(int argc, char** argv, const char* name) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) return true;
    }
    return false;
}

entt::entity makeText(engine::GameAPI& api,
                      engine::FontHandle font,
                      const char* value,
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
    text.layer = 10;
    api.addComponent(e, text);
    return e;
}

} // namespace

int main(int argc, char** argv) {
    const bool useOpenGL = hasArg(argc, argv, "--opengl");
    const bool autoExit = hasArg(argc, argv, "--auto-exit");

    engine::EngineConfig cfg;
    cfg.windowTitle = "QGame Asset Manifest Demo";
    cfg.windowWidth = 960;
    cfg.windowHeight = 540;
    cfg.vsync = true;
    cfg.debug = false;
    if (useOpenGL) {
        cfg.renderBackend = engine::RenderBackend::OpenGL;
    }

    engine::EngineContext ctx;
    ctx.init(cfg);
    engine::GameAPI api{ctx};

    const bool manifestOk = api.loadAssetManifest(QGAME_DEMO_MANIFEST);
    TextureHandle character = api.loadTextureById("texture.demo.character");
    engine::FontHandle font = api.loadFontById("font.demo.main");

    auto worldCam = api.spawnEntity();
    api.addComponent(worldCam, engine::Transform{0.f, 0.f});
    engine::Camera wcam{};
    wcam.zoom = 1.f;
    wcam.primary = true;
    wcam.depth = 0;
    wcam.layerMask = engine::renderPassBit(engine::RenderPass::World);
    wcam.clear = true;
    wcam.clearColor = core::Color{18, 22, 28, 255};
    wcam.cullEnabled = false;
    api.addComponent(worldCam, wcam);

    auto uiCam = api.spawnEntity();
    api.addComponent(uiCam, engine::Transform{0.f, 0.f});
    engine::Camera ucam{};
    ucam.zoom = 1.f;
    ucam.primary = true;
    ucam.depth = 1;
    ucam.layerMask = engine::renderPassBit(engine::RenderPass::UI) |
                     engine::renderPassBit(engine::RenderPass::Screen);
    ucam.clear = false;
    ucam.cullEnabled = false;
    api.addComponent(uiCam, ucam);

    auto sprite = api.spawnEntity();
    api.addComponent(sprite, engine::Transform{480.f, 280.f, 0.f, 4.f, 4.f});
    engine::Sprite spr{};
    spr.texture = character;
    spr.srcRect = core::Rect{0.f, 0.f, 32.f, 48.f};
    spr.pass = engine::RenderPass::World;
    spr.layer = 0;
    spr.tint = core::Color::White;
    api.addComponent(sprite, spr);

    char status[256];
    std::snprintf(status, sizeof(status),
                  "manifest: %s | texture.demo.character: %s | font.demo.main: %s",
                  manifestOk ? "loaded" : "failed",
                  character.valid() ? "loaded" : "failed",
                  font.valid() ? "loaded" : "failed");

    makeText(api, font, "Asset Manifest Demo", 32.f, 44.f, 30.f,
             core::Color{240, 245, 255, 255});
    makeText(api, font, status, 32.f, 90.f, 16.f,
             manifestOk && character.valid() && font.valid()
                 ? core::Color{120, 235, 160, 255}
                 : core::Color{255, 110, 110, 255});
    makeText(api, font, "ESC closes. Pass --opengl to force OpenGL, --auto-exit for smoke tests.",
             32.f, 500.f, 14.f, core::Color{170, 180, 195, 255});

    float t = 0.f;
    while (ctx.scheduler.tick()) {
        const float dt = ctx.scheduler.deltaTime();
        t += dt;

        api.patchComponent<engine::Transform>(sprite, [&](engine::Transform& tf) {
            tf.rotation = std::sin(t * 1.5f) * 0.12f;
            tf.y = 280.f + std::sin(t * 2.0f) * 18.f;
        });

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
    return manifestOk && character.valid() && font.valid() ? 0 : 1;
}
