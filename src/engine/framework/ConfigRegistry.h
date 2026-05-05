#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace engine {

struct GameContext;

struct ConfigDesc {
    std::string id;
    nlohmann::json value;
    std::string source;
};

// ConfigRegistry is the small S4 data-mod registry for arbitrary JSON config.
// It mirrors PrefabRegistry's stable-ID rule: registering the same config ID
// later replaces the previous value, so ModManager's deterministic load order
// naturally defines the final winner.
class ConfigRegistry {
public:
    ConfigRegistry() = default;
    explicit ConfigRegistry(GameContext& ctx);

    bool registerConfig(const std::string& id,
                        const nlohmann::json& value,
                        const std::string& source = {});
    bool registerConfigJson(const nlohmann::json& configJson,
                            const std::string& source = {});
    bool registerManifest(const std::string& path);

    bool hasConfig(const std::string& id) const;
    const ConfigDesc* findConfig(const std::string& id) const;
    std::vector<std::string> configIds() const;

private:
    std::unordered_map<std::string, ConfigDesc> configs_;
};

} // namespace engine
