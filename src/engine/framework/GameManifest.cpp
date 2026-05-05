#include "GameManifest.h"

#include <fstream>

#include <nlohmann/json.hpp>

#include "../../core/Logger.h"

using json = nlohmann::json;

namespace engine {

bool GameManifestLoader::loadFromFile(const std::string& path, GameManifest& out) {
    std::ifstream input(path);
    if (!input) {
        core::logError("[GameManifest] cannot read: %s", path.c_str());
        return false;
    }

    json root;
    try {
        input >> root;
    } catch (const json::exception& ex) {
        core::logError("[GameManifest] JSON parse error in %s: %s", path.c_str(), ex.what());
        return false;
    }

    GameManifest parsed;
    parsed.id = root.value("id", "");
    parsed.name = root.value("name", "");
    parsed.version = root.value("version", "");
    parsed.startupScene = root.value("startupScene", "");
    parsed.assetManifest = root.value("assetManifest", "");
    parsed.nativeLibrary = root.value("nativeLibrary", "");

    if (root.contains("mods")) {
        if (!root["mods"].is_array()) {
            core::logError("[GameManifest] mods must be an array: %s", path.c_str());
            return false;
        }

        // Preserve the authored order. ModManager later uses this as one of the
        // deterministic tie-breakers after dependency and priority sorting.
        for (const auto& modId : root["mods"]) {
            if (!modId.is_string()) {
                core::logError("[GameManifest] mod id must be a string: %s", path.c_str());
                return false;
            }
            parsed.mods.push_back(modId.get<std::string>());
        }
    }

    if (!parsed.valid()) {
        core::logError("[GameManifest] missing required id/startupScene: %s", path.c_str());
        return false;
    }

    out = std::move(parsed);
    core::logInfo("[GameManifest] loaded %s startupScene=%s",
                  out.id.c_str(), out.startupScene.c_str());
    return true;
}

} // namespace engine
