#include "ModManager.h"

#include "ConfigRegistry.h"
#include "GameContext.h"
#include "PrefabRegistry.h"
#include "SceneManager.h"
#include "../assets/AssetManager.h"

#include <algorithm>
#include <filesystem>
#include <queue>
#include <set>

#if defined(_WIN32)
    #include <windows.h>
#else
    #include <dlfcn.h>
#endif

#include "../../core/Logger.h"

namespace engine {

namespace {

std::string normalizePath(const std::filesystem::path& path) {
    return path.lexically_normal().string();
}

std::filesystem::path resolveRelativeTo(const std::filesystem::path& baseDir,
                                        const std::string& path) {
    std::filesystem::path p(path);
    if (p.is_absolute()) return p.lexically_normal();
    return (baseDir / p).lexically_normal();
}

void modLogInfo(const char* message) {
    core::logInfo("[NativeMod] %s", message ? message : "");
}

void modLogWarn(const char* message) {
    core::logWarn("[NativeMod] %s", message ? message : "");
}

void modLogError(const char* message) {
    core::logError("[NativeMod] %s", message ? message : "");
}

bool modLoadAssetManifest(QGameModContext* ctx, const char* path) {
    if (!ctx || !ctx->userData || !path) return false;
    auto* game = static_cast<GameContext*>(ctx->userData);
    return game->assets.loadManifestOverlay(path, "native");
}

bool modRegisterScene(QGameModContext* ctx, const char* id, const char* path) {
    if (!ctx || !ctx->userData || !id || !path) return false;
    auto* game = static_cast<GameContext*>(ctx->userData);
    return game->scenes && game->scenes->registerScene(id, path);
}

bool modRegisterSceneManifest(QGameModContext* ctx, const char* path) {
    if (!ctx || !ctx->userData || !path) return false;
    auto* game = static_cast<GameContext*>(ctx->userData);
    return game->scenes && game->scenes->registerManifest(path);
}

bool modRegisterPrefabManifest(QGameModContext* ctx, const char* path) {
    if (!ctx || !ctx->userData || !path) return false;
    auto* game = static_cast<GameContext*>(ctx->userData);
    return game->prefabs && game->prefabs->registerManifest(path);
}

bool modRegisterConfigManifest(QGameModContext* ctx, const char* path) {
    if (!ctx || !ctx->userData || !path) return false;
    auto* game = static_cast<GameContext*>(ctx->userData);
    return game->configs && game->configs->registerManifest(path);
}

QGameLogAPI gLogApi{modLogInfo, modLogWarn, modLogError};
QGameAssetManagerAPI gAssetApi{modLoadAssetManifest};
QGameSceneRegistryAPI gSceneApi{modRegisterScene, modRegisterSceneManifest};
QGamePrefabRegistryAPI gPrefabApi{modRegisterPrefabManifest};
QGameConfigRegistryAPI gConfigApi{modRegisterConfigManifest};

} // namespace

ModManager::NativeLibrary::~NativeLibrary() {
    close();
}

ModManager::NativeLibrary::NativeLibrary(NativeLibrary&& other) noexcept
    : handle(other.handle) {
    other.handle = nullptr;
}

ModManager::NativeLibrary&
ModManager::NativeLibrary::operator=(NativeLibrary&& other) noexcept {
    if (this == &other) return *this;
    close();
    handle = other.handle;
    other.handle = nullptr;
    return *this;
}

bool ModManager::NativeLibrary::open(const std::string& path) {
    close();
#if defined(_WIN32)
    handle = LoadLibraryA(path.c_str());
    if (!handle) {
        core::logError("[ModManager] failed to load native mod library: %s", path.c_str());
        return false;
    }
#else
    handle = dlopen(path.c_str(), RTLD_NOW);
    if (!handle) {
        core::logError("[ModManager] failed to load native mod library %s: %s",
                       path.c_str(), dlerror());
        return false;
    }
#endif
    return true;
}

void* ModManager::NativeLibrary::symbol(const char* name) const {
    if (!handle) return nullptr;
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle), name));
#else
    return dlsym(handle, name);
#endif
}

void ModManager::NativeLibrary::close() {
    if (!handle) return;
#if defined(_WIN32)
    FreeLibrary(static_cast<HMODULE>(handle));
#else
    dlclose(handle);
#endif
    handle = nullptr;
}

bool ModManager::scanMods(const std::string& modsDir) {
    discovered_.clear();
    mountedMods_.clear();
    modsDir_ = modsDir;

    const std::filesystem::path root(modsDir);
    if (!std::filesystem::exists(root)) {
        core::logInfo("[ModManager] mods directory not found, skipping: %s", modsDir.c_str());
        return true;
    }
    if (!std::filesystem::is_directory(root)) {
        core::logError("[ModManager] mods path is not a directory: %s", modsDir.c_str());
        return false;
    }

    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (!entry.is_directory()) continue;

        const std::filesystem::path manifestPath = entry.path() / "mod.json";
        if (!std::filesystem::exists(manifestPath)) continue;

        ModManifest manifest;
        if (!ModManifestLoader::loadFromFile(normalizePath(manifestPath), manifest)) {
            return false;
        }
        if (discovered_.find(manifest.id) != discovered_.end()) {
            core::logError("[ModManager] duplicate mod id discovered: %s", manifest.id.c_str());
            return false;
        }

        DiscoveredMod mod{};
        mod.manifest = std::move(manifest);
        mod.rootDir = normalizePath(entry.path());
        discovered_[mod.manifest.id] = std::move(mod);
    }

    core::logInfo("[ModManager] scanned %s (%zu mods)", modsDir.c_str(), discovered_.size());
    return true;
}

bool ModManager::resolveLoadOrder(const std::vector<std::string>& enabledMods,
                                  std::vector<LoadedMod>& outOrder) const {
    outOrder.clear();
    if (enabledMods.empty()) return true;

    std::unordered_map<std::string, int> enabledOrder;
    for (size_t i = 0; i < enabledMods.size(); ++i) {
        if (enabledOrder.find(enabledMods[i]) == enabledOrder.end()) {
            enabledOrder[enabledMods[i]] = static_cast<int>(i);
        }
    }

    // Collect the enabled set plus transitive dependencies first. This lets a
    // project list only its top-level mods while still mounting required data
    // packs before the mods that depend on them.
    std::set<std::string> required;
    std::vector<std::string> stack = enabledMods;
    while (!stack.empty()) {
        const std::string id = stack.back();
        stack.pop_back();
        if (!required.insert(id).second) continue;

        auto it = discovered_.find(id);
        if (it == discovered_.end()) {
            core::logError("[ModManager] enabled mod not found: %s", id.c_str());
            return false;
        }

        for (const std::string& dep : it->second.manifest.dependencies) {
            stack.push_back(dep);
            if (enabledOrder.find(dep) == enabledOrder.end()) {
                enabledOrder[dep] = -1;
            }
        }
    }

    std::unordered_map<std::string, std::vector<std::string>> dependents;
    std::unordered_map<std::string, int> indegree;
    for (const std::string& id : required) indegree[id] = 0;

    for (const std::string& id : required) {
        const auto& mod = discovered_.at(id);
        for (const std::string& dep : mod.manifest.dependencies) {
            if (required.find(dep) == required.end()) continue;
            dependents[dep].push_back(id);
            ++indegree[id];
        }
    }

    auto lessForMount = [&](const std::string& a, const std::string& b) {
        const auto& ma = discovered_.at(a).manifest;
        const auto& mb = discovered_.at(b).manifest;
        if (ma.priority != mb.priority) return ma.priority > mb.priority;
        if (enabledOrder.at(a) != enabledOrder.at(b)) return enabledOrder.at(a) > enabledOrder.at(b);
        return a > b;
    };

    std::priority_queue<std::string, std::vector<std::string>, decltype(lessForMount)> ready(lessForMount);
    for (const auto& [id, degree] : indegree) {
        if (degree == 0) ready.push(id);
    }

    while (!ready.empty()) {
        const std::string id = ready.top();
        ready.pop();

        const auto& mod = discovered_.at(id);
        LoadedMod loaded{};
        loaded.manifest = mod.manifest;
        loaded.rootDir = mod.rootDir;
        loaded.enabledOrder = enabledOrder.at(id);
        outOrder.push_back(std::move(loaded));

        for (const std::string& child : dependents[id]) {
            --indegree[child];
            if (indegree[child] == 0) ready.push(child);
        }
    }

    if (outOrder.size() != required.size()) {
        core::logError("[ModManager] dependency cycle detected while resolving mods");
        return false;
    }
    return true;
}

bool ModManager::mountGameAssetsAndMods(AssetManager& assets,
                                        const std::string& gameManifestPath,
                                        const GameManifest& gameManifest) {
    mountedMods_.clear();

    const std::filesystem::path gameFile(gameManifestPath);
    const std::filesystem::path gameRoot = gameFile.parent_path();
    const std::filesystem::path gameAssetManifest =
        resolveRelativeTo(gameRoot, gameManifest.assetManifest);

    if (!assets.loadManifest(normalizePath(gameAssetManifest))) {
        return false;
    }

    const std::filesystem::path modsRoot = gameRoot / "mods";
    if (!scanMods(normalizePath(modsRoot))) {
        return false;
    }

    std::vector<LoadedMod> order;
    if (!resolveLoadOrder(gameManifest.mods, order)) {
        return false;
    }

    for (const LoadedMod& mod : order) {
        if (mod.manifest.assetManifest.empty()) {
            mountedMods_.push_back(mod);
            core::logInfo("[ModManager] mounted mod metadata only: %s", mod.manifest.id.c_str());
            continue;
        }

        const std::filesystem::path modAssetManifest =
            resolveRelativeTo(mod.rootDir, mod.manifest.assetManifest);
        const std::string sourceName = "mod:" + mod.manifest.id;
        if (!assets.loadManifestOverlay(normalizePath(modAssetManifest), sourceName)) {
            return false;
        }

        mountedMods_.push_back(mod);
        core::logInfo("[ModManager] mounted mod assets: %s priority=%d",
                      mod.manifest.id.c_str(), mod.manifest.priority);
    }

    return true;
}

bool ModManager::mountDataForMod(GameContext& ctx, const LoadedMod& mod) {
    const std::filesystem::path modRoot(mod.rootDir);
    const std::string sourceName = "mod:" + mod.manifest.id;

    for (const std::string& manifest : mod.manifest.sceneManifests) {
        if (!ctx.scenes) {
            core::logError("[ModManager] scene manifest requires SceneManager: %s", sourceName.c_str());
            return false;
        }
        const std::filesystem::path path = resolveRelativeTo(modRoot, manifest);
        if (!ctx.scenes->registerManifest(normalizePath(path))) return false;
    }

    for (const std::string& manifest : mod.manifest.prefabManifests) {
        if (!ctx.prefabs) {
            core::logError("[ModManager] prefab manifest requires PrefabRegistry: %s", sourceName.c_str());
            return false;
        }
        const std::filesystem::path path = resolveRelativeTo(modRoot, manifest);
        if (!ctx.prefabs->registerManifest(normalizePath(path))) return false;
    }

    for (const std::string& manifest : mod.manifest.configManifests) {
        if (!ctx.configs) {
            core::logError("[ModManager] config manifest requires ConfigRegistry: %s", sourceName.c_str());
            return false;
        }
        const std::filesystem::path path = resolveRelativeTo(modRoot, manifest);
        if (!ctx.configs->registerManifest(normalizePath(path))) return false;
    }

    return true;
}

bool ModManager::mountDataMods(GameContext& ctx) {
    for (const LoadedMod& mod : mountedMods_) {
        if (!mountDataForMod(ctx, mod)) return false;
    }
    return true;
}

bool ModManager::mountGameAndMods(GameContext& ctx,
                                  const std::string& gameManifestPath,
                                  const GameManifest& gameManifest) {
    if (!mountGameAssetsAndMods(ctx.assets, gameManifestPath, gameManifest)) {
        return false;
    }
    return mountDataMods(ctx);
}

bool ModManager::loadNativeMod(GameContext& ctx, const LoadedMod& mod) {
    if (mod.manifest.type != ModType::Native) return true;

    const std::filesystem::path libraryPath =
        resolveRelativeTo(std::filesystem::path(mod.rootDir), mod.manifest.library);

    NativeMod native{};
    native.mod = mod;
    if (!native.library.open(normalizePath(libraryPath))) {
        return false;
    }

    native.init = reinterpret_cast<QGameModInitFn>(native.library.symbol("qgame_mod_init"));
    native.shutdown = reinterpret_cast<QGameModShutdownFn>(native.library.symbol("qgame_mod_shutdown"));
    if (!native.init || !native.shutdown) {
        core::logError("[ModManager] native mod missing qgame_mod_init/qgame_mod_shutdown: %s",
                       mod.manifest.id.c_str());
        return false;
    }

    native.context.apiVersion = QGAME_MOD_API_VERSION;
    native.context.userData = &ctx;
    native.context.assets = &gAssetApi;
    native.context.scenes = &gSceneApi;
    native.context.prefabs = &gPrefabApi;
    native.context.configs = &gConfigApi;
    native.context.log = &gLogApi;

    if (!native.init(&native.context)) {
        core::logError("[ModManager] qgame_mod_init failed: %s", mod.manifest.id.c_str());
        return false;
    }

    core::logInfo("[ModManager] initialized native mod: %s", mod.manifest.id.c_str());
    nativeMods_.push_back(std::move(native));
    return true;
}

bool ModManager::loadNativeMods(GameContext& ctx) {
    shutdownNativeMods();
    for (const LoadedMod& mod : mountedMods_) {
        if (!loadNativeMod(ctx, mod)) {
            shutdownNativeMods();
            return false;
        }
    }
    return true;
}

void ModManager::shutdownNativeMods() {
    for (int i = static_cast<int>(nativeMods_.size()) - 1; i >= 0; --i) {
        NativeMod& mod = nativeMods_[static_cast<size_t>(i)];
        if (mod.shutdown) {
            mod.shutdown(&mod.context);
            core::logInfo("[ModManager] shutdown native mod: %s", mod.mod.manifest.id.c_str());
        }
    }
    nativeMods_.clear();
}

} // namespace engine
