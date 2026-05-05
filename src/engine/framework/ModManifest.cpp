#include "ModManifest.h"

#include <fstream>

#include <nlohmann/json.hpp>

#include "../../core/Logger.h"

using json = nlohmann::json;

namespace engine {

namespace {

bool parseModType(const std::string& text, ModType& out) {
    if (text == "data") {
        out = ModType::Data;
        return true;
    }
    if (text == "native") {
        out = ModType::Native;
        return true;
    }
    return false;
}

bool readStringList(const json& root,
                    const char* singleName,
                    const char* arrayName,
                    std::vector<std::string>& out,
                    const std::string& path) {
    if (root.contains(singleName)) {
        if (!root[singleName].is_string()) {
            core::logError("[ModManifest] %s must be a string: %s", singleName, path.c_str());
            return false;
        }
        out.push_back(root[singleName].get<std::string>());
    }

    if (root.contains(arrayName)) {
        if (!root[arrayName].is_array()) {
            core::logError("[ModManifest] %s must be an array: %s", arrayName, path.c_str());
            return false;
        }
        for (const auto& item : root[arrayName]) {
            if (!item.is_string()) {
                core::logError("[ModManifest] %s entry must be a string: %s", arrayName, path.c_str());
                return false;
            }
            out.push_back(item.get<std::string>());
        }
    }

    return true;
}

} // namespace

bool ModManifestLoader::loadFromFile(const std::string& path, ModManifest& out) {
    std::ifstream input(path);
    if (!input) {
        core::logError("[ModManifest] cannot read: %s", path.c_str());
        return false;
    }

    json root;
    try {
        input >> root;
    } catch (const json::exception& ex) {
        core::logError("[ModManifest] JSON parse error in %s: %s", path.c_str(), ex.what());
        return false;
    }

    ModManifest parsed;
    parsed.id = root.value("id", "");
    parsed.name = root.value("name", "");
    parsed.version = root.value("version", "");
    parsed.engineVersion = root.value("engineVersion", "");
    parsed.priority = root.value("priority", 0);
    parsed.assetManifest = root.value("assetManifest", "");
    parsed.library = root.value("library", "");

    if (!readStringList(root, "sceneManifest", "sceneManifests",
                        parsed.sceneManifests, path) ||
        !readStringList(root, "prefabManifest", "prefabManifests",
                        parsed.prefabManifests, path) ||
        !readStringList(root, "configManifest", "configManifests",
                        parsed.configManifests, path)) {
        return false;
    }

    const std::string typeText = root.value("type", "data");
    if (!parseModType(typeText, parsed.type)) {
        core::logError("[ModManifest] unknown type '%s' in %s", typeText.c_str(), path.c_str());
        return false;
    }

    if (root.contains("dependencies")) {
        if (!root["dependencies"].is_array()) {
            core::logError("[ModManifest] dependencies must be an array: %s", path.c_str());
            return false;
        }

        // Keep dependency IDs as authored. ModManager will validate existence
        // and report cycles when it owns the full enabled-mod set.
        for (const auto& dep : root["dependencies"]) {
            if (!dep.is_string()) {
                core::logError("[ModManifest] dependency id must be a string: %s", path.c_str());
                return false;
            }
            parsed.dependencies.push_back(dep.get<std::string>());
        }
    }

    if (!parsed.valid()) {
        core::logError("[ModManifest] missing required id/version: %s", path.c_str());
        return false;
    }

    if (parsed.type == ModType::Native && parsed.library.empty()) {
        core::logError("[ModManifest] native mod missing library: %s", path.c_str());
        return false;
    }

    out = std::move(parsed);
    core::logInfo("[ModManifest] loaded %s priority=%d", out.id.c_str(), out.priority);
    return true;
}

} // namespace engine
