#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine {

struct GameContext;

class AssetLoaderRegistry {
public:
    using LoadFn = std::function<void*(const char* assetId, const char* path)>;
    using UnloadFn = std::function<void(void* asset)>;

    AssetLoaderRegistry() = default;
    explicit AssetLoaderRegistry(GameContext& ctx);

    bool registerLoader(const std::string& type,
                        LoadFn load,
                        UnloadFn unload,
                        void* owner,
                        const std::string& source = {});
    bool unregisterLoadersByOwner(void* owner);

    bool hasLoader(const std::string& type) const;
    std::vector<std::string> loaderTypes() const;

    void* load(const std::string& type, const char* assetId, const char* path) const;
    void unload(const std::string& type, void* asset) const;

private:
    struct LoaderDesc {
        LoadFn load;
        UnloadFn unload;
        void* owner = nullptr;
        std::string source;
    };

    std::unordered_map<std::string, LoaderDesc> loaders_;
};

} // namespace engine
