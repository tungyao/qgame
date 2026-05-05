#include <cstdio>
#include <fstream>
#include <algorithm>

#include <engine/components/PhysicsComponents.h>
#include <engine/components/RenderComponents.h>
#include <engine/framework/GameContext.h>
#include <engine/framework/GameInstance.h>
#include <engine/framework/GameManifest.h>
#include <engine/framework/ModManifest.h>
#include <engine/framework/ModManager.h>
#include <engine/framework/ConfigRegistry.h>
#include <engine/framework/PrefabRegistry.h>
#include <engine/framework/SceneManager.h>
#include <engine/assets/AssetManager.h>
#include <engine/runtime/EngineContext.h>

#include <filesystem>

#ifndef QGAME_SMOKE_NATIVE_MOD_LIB
#define QGAME_SMOKE_NATIVE_MOD_LIB ""
#endif

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
    engine::ConfigRegistry configs(gameCtx);
    engine::PrefabRegistry prefabs(gameCtx);
    engine::SceneManager scenes(gameCtx);

    if (gameCtx.scenes != &scenes) {
        return fail("SceneManager was not exposed through GameContext");
    }
    if (gameCtx.prefabs != &prefabs) {
        return fail("PrefabRegistry was not exposed through GameContext");
    }
    if (gameCtx.configs != &configs) {
        return fail("ConfigRegistry was not exposed through GameContext");
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

    const std::filesystem::path modProject = "framework_smoke_mod_project";
    std::filesystem::create_directories(modProject / "assets");
    std::filesystem::create_directories(modProject / "mods" / "base_items" / "assets");
    std::filesystem::create_directories(modProject / "mods" / "base_items" / "prefabs");
    std::filesystem::create_directories(modProject / "mods" / "hd_textures" / "assets");
    std::filesystem::create_directories(modProject / "mods" / "hd_textures" / "scenes");
    std::filesystem::create_directories(modProject / "mods" / "balance_patch" / "assets");
    std::filesystem::create_directories(modProject / "mods" / "balance_patch" / "configs");
    std::filesystem::create_directories(modProject / "mods" / "native_smoke");
    {
        std::ofstream out(modProject / "assets" / "manifest.json");
        out << R"({
  "assets": [
    { "id": "texture.demo.character", "type": "texture", "source": "game_character.png" }
  ]
})";
    }
    {
        std::ofstream out(modProject / "mods" / "base_items" / "mod.json");
        out << R"({
  "id": "base_items",
  "name": "Base Items",
  "version": "0.1.0",
  "type": "data",
  "priority": 0,
  "assetManifest": "assets/manifest.json",
  "prefabManifest": "prefabs/manifest.json",
  "dependencies": []
})";
    }
    {
        std::ofstream out(modProject / "mods" / "base_items" / "assets" / "manifest.json");
        out << R"({
  "assets": [
    { "id": "texture.mod.base_items.marker", "type": "texture", "source": "marker.png" }
  ]
})";
    }
    {
        std::ofstream out(modProject / "mods" / "base_items" / "prefabs" / "manifest.json");
        out << R"({
  "prefabs": [
    {
      "id": "prefab.mod.base_items.crate",
      "components": {
        "Name": { "s": "BaseCrate" },
        "Transform": { "x": 7.0, "y": 8.0, "rot": 0.0, "sx": 1.0, "sy": 1.0 }
      }
    }
  ]
})";
    }
    {
        std::ofstream out(modProject / "mods" / "hd_textures" / "mod.json");
        out << R"({
  "id": "hd_textures",
  "name": "HD Textures",
  "version": "0.1.0",
  "type": "data",
  "priority": 10,
  "assetManifest": "assets/manifest.json",
  "sceneManifest": "scenes/manifest.json",
  "dependencies": [ "base_items" ]
})";
    }
    {
        std::ofstream out(modProject / "mods" / "hd_textures" / "assets" / "manifest.json");
        out << R"({
  "assets": [
    { "id": "texture.demo.character", "type": "texture", "source": "hd_character.png" }
  ]
})";
    }
    {
        std::ofstream out(modProject / "mods" / "hd_textures" / "scenes" / "manifest.json");
        out << R"({
  "scenes": [
    { "id": "scene.mod.hd.preview", "path": "preview.scene.json" }
  ]
})";
    }
    {
        std::ofstream out(modProject / "mods" / "hd_textures" / "scenes" / "preview.scene.json");
        out << R"({
  "version": 1,
  "entities": [
    {
      "Name": { "s": "HdPreview" },
      "Transform": { "x": 1.0, "y": 2.0, "rot": 0.0, "sx": 1.0, "sy": 1.0 }
    }
  ]
})";
    }
    {
        std::ofstream out(modProject / "mods" / "balance_patch" / "mod.json");
        out << R"({
  "id": "balance_patch",
  "name": "Balance Patch",
  "version": "0.1.0",
  "type": "data",
  "priority": 20,
  "assetManifest": "assets/manifest.json",
  "configManifest": "configs/manifest.json",
  "dependencies": []
})";
    }
    {
        std::ofstream out(modProject / "mods" / "balance_patch" / "assets" / "manifest.json");
        out << R"({
  "assets": [
    { "id": "texture.demo.character", "type": "texture", "source": "balance_character.png" }
  ]
})";
    }
    {
        std::ofstream out(modProject / "mods" / "balance_patch" / "configs" / "manifest.json");
        out << R"({
  "configs": [
    {
      "id": "config.game.balance",
      "value": { "playerDamage": 42, "source": "balance_patch" }
    }
  ]
})";
    }
    {
        std::string nativeLib = QGAME_SMOKE_NATIVE_MOD_LIB;
        std::replace(nativeLib.begin(), nativeLib.end(), '\\', '/');
        std::ofstream out(modProject / "mods" / "native_smoke" / "mod.json");
        out << R"({
  "id": "native_smoke",
  "name": "Native Smoke",
  "version": "0.1.0",
  "type": "native",
  "priority": 30,
  "library": ")" << nativeLib << R"(",
  "dependencies": [ "balance_patch" ]
})";
    }

    engine::GameManifest modGameManifest;
    modGameManifest.id = "mod_smoke_game";
    modGameManifest.startupScene = "scene.smoke";
    modGameManifest.assetManifest = "assets/manifest.json";
    modGameManifest.mods = { "hd_textures", "balance_patch", "native_smoke" };

    engine::ModManager mods;
    gameCtx.assets.shutdown();
    gameCtx.assets.init(nullptr, nullptr);
    if (!mods.mountGameAndMods(gameCtx,
                               (modProject / "game.json").string(),
                               modGameManifest)) {
        return fail("mod manager failed to mount game assets and mods");
    }
    if (!gameCtx.assets.hasAsset("texture.demo.character") ||
        !gameCtx.assets.hasAsset("texture.mod.base_items.marker")) {
        return fail("mod assets were not mounted");
    }
    auto chain = gameCtx.assets.assetOverrideChain("texture.demo.character");
    if (chain.size() != 3 ||
        chain[0].sourceName != "game" ||
        chain[1].sourceName != "mod:hd_textures" ||
        chain[2].sourceName != "mod:balance_patch") {
        return fail("mod override chain was not deterministic");
    }
    const auto& mounted = mods.mountedMods();
    if (mounted.size() != 4 ||
        mounted[0].manifest.id != "base_items" ||
        mounted[1].manifest.id != "hd_textures" ||
        mounted[2].manifest.id != "balance_patch" ||
        mounted[3].manifest.id != "native_smoke") {
        return fail("mod load order did not respect dependencies and priority");
    }
    if (!prefabs.hasPrefab("prefab.mod.base_items.crate")) {
        return fail("data mod prefab was not registered");
    }
    if (!scenes.hasScene("scene.mod.hd.preview")) {
        return fail("data mod scene was not registered");
    }
    const engine::ConfigDesc* balance = configs.findConfig("config.game.balance");
    if (!balance || balance->value.value("playerDamage", 0) != 42) {
        return fail("data mod config was not registered");
    }
    std::filesystem::remove("framework_smoke_native_init.txt");
    std::filesystem::remove("framework_smoke_native_shutdown.txt");
    if (!mods.loadNativeMods(gameCtx)) {
        return fail("native mod load failed");
    }
    if (!std::filesystem::exists("framework_smoke_native_init.txt")) {
        return fail("native mod init did not run");
    }
    mods.shutdownNativeMods();
    if (!std::filesystem::exists("framework_smoke_native_shutdown.txt")) {
        return fail("native mod shutdown did not run");
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

    const char* prefabManifestPath = "framework_smoke.prefabs.json";
    {
        std::ofstream out(prefabManifestPath);
        out << R"({
  "prefabs": [
    {
      "id": "prefab.smoke.actor",
      "components": {
        "Name": { "s": "PrefabBase" },
        "Transform": { "x": 1.0, "y": 2.0, "rot": 0.0, "sx": 1.0, "sy": 1.0 },
        "RigidBody": { "vx": 3.0, "vy": 4.0, "gs": 0.0, "kin": true }
      }
    }
  ]
})";
    }

    if (!prefabs.registerManifest(prefabManifestPath)) {
        return fail("prefab manifest registration failed");
    }
    if (!prefabs.hasPrefab("prefab.smoke.actor")) {
        return fail("registered prefab was not found");
    }

    const char* prefabScenePath = "framework_smoke.prefab.scene.json";
    {
        std::ofstream out(prefabScenePath);
        out << R"({
  "version": 2,
  "entities": [
    {
      "prefab": "prefab.smoke.actor",
      "components": {
        "Name": { "s": "PrefabInstance" },
        "Transform": { "x": 50.0, "y": 60.0, "rot": 0.0, "sx": 2.0, "sy": 2.0 }
      }
    }
  ]
})";
    }

    if (!scenes.registerScene("scene.prefab_smoke", prefabScenePath)) {
        return fail("prefab scene registration failed");
    }
    if (!scenes.loadScene("scene.prefab_smoke")) {
        return fail("prefab scene load failed");
    }

    bool foundPrefabInstance = false;
    auto prefabView = gameCtx.world.view<engine::Name, engine::Transform, engine::RigidBody>();
    for (auto ent : prefabView) {
        const auto& name = prefabView.get<engine::Name>(ent);
        const auto& transform = prefabView.get<engine::Transform>(ent);
        const auto& rb = prefabView.get<engine::RigidBody>(ent);
        if (std::string(name.c_str()) == "PrefabInstance" &&
            transform.x == 50.0f &&
            transform.y == 60.0f &&
            transform.scaleX == 2.0f &&
            rb.velocityX == 3.0f &&
            rb.isKinematic) {
            foundPrefabInstance = true;
        }
    }
    if (!foundPrefabInstance) {
        return fail("prefab instance with scene overrides was not found");
    }

    scenes.unloadScene();

    game.onShutdown(gameCtx);
    if (game.shutdownCount != 1) {
        return fail("GameInstance onShutdown did not run");
    }

    return 0;
}
