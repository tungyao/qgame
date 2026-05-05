#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include <engine/api/GameAPI.h>
#include <engine/components/RenderComponents.h>
#include <engine/components/TextComponent.h>
#include <engine/framework/ConfigRegistry.h>
#include <engine/framework/GameContext.h>
#include <engine/framework/GameManifest.h>
#include <engine/framework/ModManager.h>
#include <engine/framework/PrefabRegistry.h>
#include <engine/framework/SceneManager.h>
#include <engine/runtime/EngineConfig.h>
#include <engine/runtime/EngineContext.h>

#ifndef QGAME_DEMO2_GAME_MANIFEST
#define QGAME_DEMO2_GAME_MANIFEST "game/demo2/game.json"
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
    text.layer = 20;
    api.addComponent(e, text);
    return e;
}

} // namespace

int main(int argc, char** argv) {
    const bool useOpenGL = hasArg(argc, argv, "--opengl");
    const bool autoExit = hasArg(argc, argv, "--auto-exit");

    engine::EngineConfig cfg;
    cfg.windowTitle = "QGame Demo2 - Mod Resource Pack";
    cfg.windowWidth = 960;
    cfg.windowHeight = 540;
    cfg.vsync = true;
    if (useOpenGL) {
        cfg.renderBackend = engine::RenderBackend::OpenGL;
    }

    engine::EngineContext ctx;
    ctx.init(cfg);
    engine::GameAPI api{ctx};
    engine::GameContext gameCtx{ctx};
    engine::ConfigRegistry configs{gameCtx};
    engine::PrefabRegistry prefabs{gameCtx};
    engine::SceneManager scenes{gameCtx};
    gameCtx.api = &api;

    engine::GameManifest manifest;
    const bool gameManifestOk =
        engine::GameManifestLoader::loadFromFile(QGAME_DEMO2_GAME_MANIFEST, manifest);

    engine::ModManager mods;
    const bool modMountOk =
        gameManifestOk &&
        mods.mountGameAndMods(gameCtx, QGAME_DEMO2_GAME_MANIFEST, manifest);

    TextureHandle character = api.loadTextureById("texture.demo.character");
    engine::FontHandle font = api.loadFontById("font.demo.main");
    const auto chain = ctx.assetManager.assetOverrideChain("texture.demo.character");
    const auto& mountedMods = mods.mountedMods();
    std::string loadOrder = "load order:";
    for (const auto& mod : mountedMods) {
        loadOrder += " " + mod.manifest.id + "(p" + std::to_string(mod.manifest.priority) + ")";
    }
    const bool prefabOk = prefabs.hasPrefab("prefab.mod.hd_textures.preview");
    const bool basePrefabOk = prefabs.hasPrefab("prefab.mod.base_items.token");
    const bool sceneOk = scenes.hasScene("scene.mod.hd_textures.preview");
    const bool balanceSceneOk = scenes.hasScene("scene.mod.balance_patch.preview");
    const engine::ConfigDesc* demoConfig = configs.findConfig("config.mod.hd_textures.demo2");
    const engine::ConfigDesc* multiConfig = configs.findConfig("config.demo2.multi");
    const std::string multiWinner = multiConfig ? multiConfig->value.value("winner", "") : "";
    const int multiBonus = multiConfig ? multiConfig->value.value("bonus", 0) : 0;

    const bool orderOk =
        mountedMods.size() == 3 &&
        mountedMods[0].manifest.id == "base_items" &&
        mountedMods[1].manifest.id == "hd_textures" &&
        mountedMods[2].manifest.id == "balance_patch";
    const bool overrideOk =
        chain.size() == 3 &&
        chain[0].sourceName == "game" &&
        chain[1].sourceName == "mod:hd_textures" &&
        chain[2].sourceName == "mod:balance_patch";
    const bool dataOk =
        prefabOk && basePrefabOk && sceneOk && balanceSceneOk &&
        demoConfig && multiConfig &&
        multiWinner == "balance_patch" && multiBonus == 99;
    const bool multiModOk = gameManifestOk && modMountOk && orderOk && overrideOk && dataOk;

    auto worldCam = api.spawnEntity();
    api.addComponent(worldCam, engine::Transform{0.f, 0.f});
    engine::Camera wcam{};
    wcam.zoom = 1.f;
    wcam.primary = true;
    wcam.depth = 0;
    wcam.layerMask = engine::renderPassBit(engine::RenderPass::World);
    wcam.clear = true;
    wcam.clearColor = core::Color{16, 18, 24, 255};
    wcam.cullEnabled = false;
    api.addComponent(worldCam, wcam);

    auto uiCam = api.spawnEntity();
    api.addComponent(uiCam, engine::Transform{0.f, 0.f});
    engine::Camera ucam{};
    ucam.zoom = 1.f;
    ucam.primary = true;
    ucam.depth = 1;
    ucam.layerMask = engine::renderPassBit(engine::RenderPass::UI);
    ucam.clear = false;
    ucam.cullEnabled = false;
    api.addComponent(uiCam, ucam);

    auto sprite = api.spawnEntity();
    api.addComponent(sprite, engine::Transform{480.f, 285.f, 0.f, 5.f, 5.f});
    engine::Sprite spr{};
    spr.texture = character;
    spr.srcRect = core::Rect{0.f, 0.f, 32.f, 48.f};
    spr.pass = engine::RenderPass::World;
    spr.tint = core::Color::White;
    api.addComponent(sprite, spr);

    const std::string path = character.valid() ? api.assetManager().texturePath(character) : "(not loaded)";
    std::string chainText = "override chain:";
    for (const auto& item : chain) {
        chainText += " " + item.sourceName;
    }

    makeText(api, font, "Demo2: Multi-Mod Load Test", 32.f, 42.f, 30.f,
             core::Color{242, 246, 255, 255});
    makeText(api, font,
             std::string("game.json + 3 mods mounted: ") + (multiModOk ? "OK" : "FAILED"),
             32.f, 88.f, 16.f,
             multiModOk ? core::Color{120, 235, 160, 255} : core::Color{255, 110, 110, 255});
    makeText(api, font, loadOrder, 32.f, 116.f, 15.f,
             orderOk ? core::Color{130, 220, 255, 255} : core::Color{255, 150, 100, 255});
    makeText(api, font, chainText, 32.f, 144.f, 15.f,
             overrideOk ? core::Color{120, 210, 255, 255} : core::Color{255, 190, 120, 255});
    makeText(api, font, "winner path: " + path, 32.f, 172.f, 13.f,
             core::Color{185, 195, 210, 255});
    makeText(api, font,
             std::string("data: base prefab ") + (basePrefabOk ? "OK" : "missing") +
                 " | hd prefab " + (prefabOk ? "OK" : "missing") +
                 " | scenes " + (sceneOk && balanceSceneOk ? "OK" : "missing"),
             32.f, 200.f, 15.f,
             basePrefabOk && prefabOk && sceneOk && balanceSceneOk
                 ? core::Color{150, 235, 210, 255}
                 : core::Color{255, 130, 110, 255});
    makeText(api, font,
             "config.demo2.multi winner: " + multiWinner +
                 " | bonus " + std::to_string(multiBonus),
             32.f, 228.f, 15.f,
             multiWinner == "balance_patch" && multiBonus == 99
                 ? core::Color{150, 235, 210, 255}
                 : core::Color{255, 130, 110, 255});
    makeText(api, font, "ESC closes. Pass --opengl to force OpenGL, --auto-exit for smoke tests.",
             32.f, 500.f, 14.f, core::Color{165, 175, 190, 255});

    float t = 0.f;
    while (ctx.scheduler.tick()) {
        const float dt = ctx.scheduler.deltaTime();
        t += dt;

        api.patchComponent<engine::Transform>(sprite, [&](engine::Transform& tf) {
            tf.rotation = std::sin(t * 1.6f) * 0.16f;
            tf.scaleX = 5.f + std::sin(t * 2.1f) * 0.25f;
            tf.scaleY = 5.f + std::cos(t * 1.7f) * 0.25f;
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
    return multiModOk && character.valid() && font.valid() ? 0 : 1;
}
