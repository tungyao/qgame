#include "PrefabRegistry.h"

#include <fstream>

#include "GameContext.h"
#include "../assets/AssetManager.h"
#include "../scene/ComponentJson.h"
#include "../../core/Logger.h"

namespace engine {

PrefabRegistry::PrefabRegistry(GameContext& ctx) {
    // GameContext only points at framework services; ownership stays with the
    // host game/application so setup order stays explicit.
    ctx.prefabs = this;
}

bool PrefabRegistry::registerPrefab(const std::string& id,
                                    const nlohmann::json& components,
                                    const std::string& source) {
    if (id.empty() || !components.is_object()) {
        core::logWarn("[PrefabRegistry] ignored prefab with empty id or non-object components");
        return false;
    }

    PrefabDesc desc;
    desc.id = id;
    desc.components = components;
    desc.source = source;
    prefabs_[id] = std::move(desc);

    core::logInfo("[PrefabRegistry] registered prefab %s", id.c_str());
    return true;
}

bool PrefabRegistry::registerPrefabJson(const nlohmann::json& prefabJson,
                                        const std::string& source) {
    if (!prefabJson.is_object()) {
        core::logWarn("[PrefabRegistry] prefab JSON must be an object");
        return false;
    }

    const std::string id = prefabJson.value("id", "");
    nlohmann::json components = nlohmann::json::object();

    // Preferred S2 shape:
    //   { "id": "prefab.game.player", "components": { ... } }
    // During migration, also allow component fields at the prefab object top
    // level so small hand-authored files can reuse the scene entity shape.
    if (prefabJson.contains("components") && prefabJson["components"].is_object()) {
        components = prefabJson["components"];
    } else {
        components = scene_json::collectComponentObject(prefabJson);
    }

    return registerPrefab(id, components, source);
}

bool PrefabRegistry::registerManifest(const std::string& path) {
    std::ifstream input(path);
    if (!input) {
        core::logError("[PrefabRegistry] cannot read manifest: %s", path.c_str());
        return false;
    }

    nlohmann::json root;
    try {
        input >> root;
    } catch (const nlohmann::json::exception& ex) {
        core::logError("[PrefabRegistry] manifest parse error in %s: %s", path.c_str(), ex.what());
        return false;
    }

    size_t count = 0;
    if (root.contains("prefabs")) {
        if (!root["prefabs"].is_array()) {
            core::logError("[PrefabRegistry] prefabs must be an array: %s", path.c_str());
            return false;
        }

        for (const auto& prefabJson : root["prefabs"]) {
            if (!registerPrefabJson(prefabJson, path)) {
                return false;
            }
            ++count;
        }
    } else {
        // A single prefab file is useful for quick tests and small content packs.
        if (!registerPrefabJson(root, path)) {
            return false;
        }
        count = 1;
    }

    core::logInfo("[PrefabRegistry] loaded %zu prefab(s) from %s", count, path.c_str());
    return true;
}

bool PrefabRegistry::hasPrefab(const std::string& id) const {
    return prefabs_.find(id) != prefabs_.end();
}

const PrefabDesc* PrefabRegistry::findPrefab(const std::string& id) const {
    auto it = prefabs_.find(id);
    return it != prefabs_.end() ? &it->second : nullptr;
}

std::vector<std::string> PrefabRegistry::prefabIds() const {
    std::vector<std::string> ids;
    ids.reserve(prefabs_.size());
    for (const auto& kv : prefabs_) {
        ids.push_back(kv.first);
    }
    return ids;
}

entt::entity PrefabRegistry::instantiate(const std::string& id,
                                         entt::registry& world,
                                         AssetManager& assets,
                                         const nlohmann::json& overrides) const {
    const PrefabDesc* prefab = findPrefab(id);
    if (!prefab) {
        core::logError("[PrefabRegistry] prefab id not registered: %s", id.c_str());
        return entt::null;
    }
    if (!overrides.is_object()) {
        core::logError("[PrefabRegistry] overrides must be an object for prefab: %s", id.c_str());
        return entt::null;
    }

    entt::entity entity = world.create();
    scene_json::applyKnownComponents(world, entity, assets, prefab->components);

    // Overrides are applied after base components. This is intentionally simple
    // and deterministic: component JSON in the scene wins over prefab defaults.
    scene_json::applyKnownComponents(world, entity, assets, overrides);
    return entity;
}

} // namespace engine
