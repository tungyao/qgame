#include "AssetLoaderRegistry.h"

#include <algorithm>

#include "GameContext.h"
#include "../../core/Logger.h"

namespace engine {

AssetLoaderRegistry::AssetLoaderRegistry(GameContext& ctx) {
    ctx.assetLoaders = this;
}

bool AssetLoaderRegistry::registerLoader(const std::string& type,
                                         LoadFn load,
                                         UnloadFn unload,
                                         void* owner,
                                         const std::string& source) {
    if (type.empty() || !load) {
        core::logWarn("[AssetLoaderRegistry] ignored loader with empty type or load callback");
        return false;
    }

    loaders_[type] = LoaderDesc{std::move(load), std::move(unload), owner, source};
    core::logInfo("[AssetLoaderRegistry] registered loader %s", type.c_str());
    return true;
}

bool AssetLoaderRegistry::unregisterLoadersByOwner(void* owner) {
    bool removed = false;
    for (auto it = loaders_.begin(); it != loaders_.end(); ) {
        if (it->second.owner == owner) {
            it = loaders_.erase(it);
            removed = true;
        } else {
            ++it;
        }
    }
    return removed;
}

bool AssetLoaderRegistry::hasLoader(const std::string& type) const {
    return loaders_.find(type) != loaders_.end();
}

std::vector<std::string> AssetLoaderRegistry::loaderTypes() const {
    std::vector<std::string> types;
    types.reserve(loaders_.size());
    for (const auto& [type, _] : loaders_) types.push_back(type);
    std::sort(types.begin(), types.end());
    return types;
}

void* AssetLoaderRegistry::load(const std::string& type,
                                const char* assetId,
                                const char* path) const {
    auto it = loaders_.find(type);
    if (it == loaders_.end()) return nullptr;
    return it->second.load(assetId, path);
}

void AssetLoaderRegistry::unload(const std::string& type, void* asset) const {
    auto it = loaders_.find(type);
    if (it != loaders_.end() && it->second.unload) {
        it->second.unload(asset);
    }
}

} // namespace engine
