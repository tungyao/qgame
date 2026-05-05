#include <cstdio>
#include <fstream>

#include <engine/components/RenderComponents.h>
#include <engine/framework/GameContext.h>
#include <engine/framework/GameInstance.h>
#include <engine/framework/GameManifest.h>
#include <engine/framework/ModManifest.h>
#include <engine/framework/SceneManager.h>
#include <engine/runtime/EngineContext.h>

namespace {

int fail(const char* msg) {
    std::fprintf(stderr, "framework_smoke: %s\n", msg);
    return 1;
}

class CountingGame final : public engine::GameInstance {
public:
    bool onInit(engine::GameContext& ctx) override {
        ++initCount;
        return ctx.scenes != nullptr;
    }

    void onUpdate(engine::GameContext&, float dt) override {
        ++updateCount;
        lastDt = dt;
    }

    void onShutdown(engine::GameContext&) override {
        ++shutdownCount;
    }

    int initCount = 0;
    int updateCount = 0;
    int shutdownCount = 0;
    float lastDt = 0.0f;
};

} // namespace

int main() {
    engine::EngineContext engineCtx;
    engine::GameContext gameCtx(engineCtx);
    engine::SceneManager scenes(gameCtx);

    if (gameCtx.scenes != &scenes) {
        return fail("SceneManager was not exposed through GameContext");
    }

    CountingGame game;
    if (!game.onInit(gameCtx) || game.initCount != 1) {
        return fail("GameInstance onInit did not run");
    }

    game.onUpdate(gameCtx, 0.016f);
    if (game.updateCount != 1 || game.lastDt != 0.016f) {
        return fail("GameInstance onUpdate did not receive dt");
    }

    const char* manifestPath = "framework_smoke.game.json";
    {
        std::ofstream out(manifestPath);
        out << R"({
  "id": "smoke_game",
  "name": "Smoke Game",
  "version": "0.1.0",
  "startupScene": "scene.smoke",
  "assetManifest": "assets/manifest.json",
  "nativeLibrary": "native/smoke_game.dll",
  "mods": [ "first_mod", "second_mod" ]
})";
    }

    engine::GameManifest manifest;
    if (!engine::GameManifestLoader::loadFromFile(manifestPath, manifest)) {
        return fail("game manifest load failed");
    }
    if (manifest.id != "smoke_game" ||
        manifest.startupScene != "scene.smoke" ||
        manifest.mods.size() != 2 ||
        manifest.mods[1] != "second_mod") {
        return fail("game manifest fields were not parsed");
    }

    const char* modManifestPath = "framework_smoke.mod.json";
    {
        std::ofstream out(modManifestPath);
        out << R"({
  "id": "fire_weapons",
  "name": "Fire Weapons",
  "version": "0.1.0",
  "engineVersion": "0.1.x",
  "type": "native",
  "priority": 10,
  "assetManifest": "assets/manifest.json",
  "library": "native/fire_weapons.dll",
  "dependencies": [ "base_items" ]
})";
    }

    engine::ModManifest modManifest;
    if (!engine::ModManifestLoader::loadFromFile(modManifestPath, modManifest)) {
        return fail("mod manifest load failed");
    }
    if (modManifest.id != "fire_weapons" ||
        modManifest.type != engine::ModType::Native ||
        modManifest.priority != 10 ||
        modManifest.dependencies.size() != 1 ||
        modManifest.dependencies[0] != "base_items") {
        return fail("mod manifest fields were not parsed");
    }

    const char* scenePath = "framework_smoke.scene.json";
    {
        std::ofstream out(scenePath);
        out << R"({
  "version": 1,
  "entities": [
    {
      "Name": { "s": "SmokeEntity" },
      "Transform": { "x": 12.0, "y": 34.0, "rot": 0.0, "sx": 1.0, "sy": 1.0 }
    }
  ]
})";
    }

    if (!scenes.registerScene("scene.smoke", scenePath)) {
        return fail("scene registration failed");
    }
    if (!scenes.loadScene("scene.smoke")) {
        return fail("scene load failed");
    }
    if (scenes.currentSceneId() != "scene.smoke") {
        return fail("current scene id was not updated");
    }

    bool foundEntity = false;
    auto view = gameCtx.world.view<engine::Name, engine::Transform>();
    for (auto ent : view) {
        const auto& name = view.get<engine::Name>(ent);
        const auto& transform = view.get<engine::Transform>(ent);
        if (std::string(name.c_str()) == "SmokeEntity" &&
            transform.x == 12.0f &&
            transform.y == 34.0f) {
            foundEntity = true;
        }
    }
    if (!foundEntity) {
        return fail("loaded scene entity was not found");
    }

    scenes.unloadScene();
    auto remainingNames = gameCtx.world.view<engine::Name>();
    if (remainingNames.begin() != remainingNames.end() || !scenes.currentSceneId().empty()) {
        return fail("scene unload did not clear state");
    }

    game.onShutdown(gameCtx);
    if (game.shutdownCount != 1) {
        return fail("GameInstance onShutdown did not run");
    }

    return 0;
}
