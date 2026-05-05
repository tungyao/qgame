#pragma once

#include "GameManifest.h"
#include "ModManifest.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace engine {

class AssetManager;

// ModManager owns the S3 resource-package layer:
// - discover mod.json files under a project mods/ directory;
// - resolve enabled mods plus dependencies into a deterministic mount order;
// - mount each mod asset manifest as an AssetManager overlay.
//
// It deliberately does not load native libraries here. Native Mod loading is S5,
// and keeping that boundary clear makes pure resource mods testable without ABI
// or platform-loader concerns.
class ModManager {
public:
    struct LoadedMod {
        ModManifest manifest;
        std::string rootDir;
        int enabledOrder = 0;
    };

    bool scanMods(const std::string& modsDir);
    bool resolveLoadOrder(const std::vector<std::string>& enabledMods,
                          std::vector<LoadedMod>& outOrder) const;

    bool mountGameAssetsAndMods(AssetManager& assets,
                                const std::string& gameManifestPath,
                                const GameManifest& gameManifest);

    const std::vector<LoadedMod>& mountedMods() const { return mountedMods_; }

private:
    struct DiscoveredMod {
        ModManifest manifest;
        std::string rootDir;
    };

    std::unordered_map<std::string, DiscoveredMod> discovered_;
    std::string modsDir_;
    std::vector<LoadedMod> mountedMods_;
};

} // namespace engine
