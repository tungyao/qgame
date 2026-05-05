#include "SceneSerializer.h"
#include "../assets/AssetManager.h"
#include "../framework/PrefabRegistry.h"
#include "../../core/Logger.h"
#include "../../platform/FileSystem.h"
#include "ComponentJson.h"

#include <nlohmann/json.hpp>
#include <fstream>
#include <unordered_map>

using json = nlohmann::json;

namespace engine {

// ── Save ──────────────────────────────────────────────────────────────────────

bool SceneSerializer::saveScene(entt::registry& reg,
                                AssetManager& mgr,
                                const std::string& path) {
    json root;
    root["version"] = 1;
    json& entities  = root["entities"];

    for (auto e : reg.storage<entt::entity>()) {
        json je;
        scene_json::writeKnownComponents(reg, e, mgr, je);

        entities.push_back(std::move(je));
    }

    std::ofstream ofs(path);
    if (!ofs) {
        core::logError("[SceneSerializer] cannot write: %s", path.c_str());
        return false;
    }
    ofs << root.dump(2);
    return true;
}

// ── Load ──────────────────────────────────────────────────────────────────────

bool SceneSerializer::loadScene(entt::registry& reg,
                                AssetManager& mgr,
                                const std::string& path) {
    return loadScene(reg, mgr, path, nullptr);
}

bool SceneSerializer::loadScene(entt::registry& reg,
                                AssetManager& mgr,
                                const std::string& path,
                                const PrefabRegistry* prefabs) {
    std::ifstream ifs(path);
    if (!ifs) {
        core::logError("[SceneSerializer] cannot read: %s", path.c_str());
        return false;
    }

    json root;
    try { ifs >> root; }
    catch (const json::exception& ex) {
        core::logError("[SceneSerializer] JSON parse error: %s", ex.what());
        return false;
    }

    reg.clear();

    for (const auto& je : root["entities"]) {
        const scene_json::Json overrides = scene_json::collectComponentObject(je);

        // New S2 entity shape:
        //   { "prefab": "prefab.game.player", "components": { "Transform": ... } }
        // Prefab instantiation owns the create() call so it can apply base
        // components before scene-local overrides.
        const std::string prefabId = je.value("prefab", "");
        if (!prefabId.empty()) {
            if (!prefabs) {
                core::logError("[SceneSerializer] prefab used but no PrefabRegistry supplied: %s", prefabId.c_str());
                return false;
            }
            if (prefabs->instantiate(prefabId, reg, mgr, overrides) == entt::null) {
                return false;
            }
            continue;
        }

        entt::entity e = reg.create();
        scene_json::applyKnownComponents(reg, e, mgr, overrides);
    }

    core::logInfo("[SceneSerializer] loaded %zu entities from %s",
                       root["entities"].size(), path.c_str());
    return true;
}

} // namespace engine
