#include "ConfigRegistry.h"

#include <algorithm>
#include <fstream>

#include "GameContext.h"
#include "../../core/Logger.h"

namespace engine {

ConfigRegistry::ConfigRegistry(GameContext& ctx) {
    ctx.configs = this;
}

bool ConfigRegistry::registerConfig(const std::string& id,
                                    const nlohmann::json& value,
                                    const std::string& source) {
    if (id.empty()) {
        core::logWarn("[ConfigRegistry] ignored config with empty id");
        return false;
    }

    ConfigDesc desc;
    desc.id = id;
    desc.value = value;
    desc.source = source;
    configs_[id] = std::move(desc);

    core::logInfo("[ConfigRegistry] registered config %s", id.c_str());
    return true;
}

bool ConfigRegistry::registerConfigJson(const nlohmann::json& configJson,
                                        const std::string& source) {
    if (!configJson.is_object()) {
        core::logWarn("[ConfigRegistry] config JSON must be an object");
        return false;
    }

    const std::string id = configJson.value("id", "");
    if (configJson.contains("value")) {
        return registerConfig(id, configJson["value"], source);
    }

    nlohmann::json value = configJson;
    value.erase("id");
    return registerConfig(id, value, source);
}

bool ConfigRegistry::registerManifest(const std::string& path) {
    std::ifstream input(path);
    if (!input) {
        core::logError("[ConfigRegistry] cannot read manifest: %s", path.c_str());
        return false;
    }

    nlohmann::json root;
    try {
        input >> root;
    } catch (const nlohmann::json::exception& ex) {
        core::logError("[ConfigRegistry] manifest parse error in %s: %s", path.c_str(), ex.what());
        return false;
    }

    size_t count = 0;
    if (root.contains("configs")) {
        if (!root["configs"].is_array()) {
            core::logError("[ConfigRegistry] configs must be an array: %s", path.c_str());
            return false;
        }
        for (const auto& configJson : root["configs"]) {
            if (!registerConfigJson(configJson, path)) return false;
            ++count;
        }
    } else {
        if (!registerConfigJson(root, path)) return false;
        count = 1;
    }

    core::logInfo("[ConfigRegistry] loaded %zu config(s) from %s", count, path.c_str());
    return true;
}

bool ConfigRegistry::hasConfig(const std::string& id) const {
    return configs_.find(id) != configs_.end();
}

const ConfigDesc* ConfigRegistry::findConfig(const std::string& id) const {
    auto it = configs_.find(id);
    return it != configs_.end() ? &it->second : nullptr;
}

std::vector<std::string> ConfigRegistry::configIds() const {
    std::vector<std::string> ids;
    ids.reserve(configs_.size());
    for (const auto& kv : configs_) ids.push_back(kv.first);
    std::sort(ids.begin(), ids.end());
    return ids;
}

} // namespace engine
