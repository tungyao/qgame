#include "ModManager.h"

#include "AssetLoaderRegistry.h"
#include "ConfigRegistry.h"
#include "GameContext.h"
#include "PrefabRegistry.h"
#include "SceneManager.h"
#include "../assets/AssetManager.h"
#include "../runtime/SystemRegistry.h"
#include "../systems/ISystem.h"

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

UpdatePhaseMask nativePhaseMaskToEnginePhaseMask(uint32_t nativeMask) {
    UpdatePhaseMask engineMask = 0u;
    if ((nativeMask & QGAME_UPDATE_PHASE_BIT_INPUT) != 0u) {
        engineMask |= updatePhaseBit(UpdatePhase::Input);
    }
    if ((nativeMask & QGAME_UPDATE_PHASE_BIT_GAMEPLAY_PRE_PHYSICS) != 0u) {
        engineMask |= updatePhaseBit(UpdatePhase::GameplayPrePhysics);
    }
    if ((nativeMask & QGAME_UPDATE_PHASE_BIT_PHYSICS) != 0u) {
        engineMask |= updatePhaseBit(UpdatePhase::Physics);
    }
    if ((nativeMask & QGAME_UPDATE_PHASE_BIT_GAMEPLAY_POST_PHYSICS) != 0u) {
        engineMask |= updatePhaseBit(UpdatePhase::GameplayPostPhysics);
    }
    if ((nativeMask & QGAME_UPDATE_PHASE_BIT_ANIMATION) != 0u) {
        engineMask |= updatePhaseBit(UpdatePhase::Animation);
    }
    if ((nativeMask & QGAME_UPDATE_PHASE_BIT_CAMERA) != 0u) {
        engineMask |= updatePhaseBit(UpdatePhase::Camera);
    }
    if ((nativeMask & QGAME_UPDATE_PHASE_BIT_UI) != 0u) {
        engineMask |= updatePhaseBit(UpdatePhase::UI);
    }
    if ((nativeMask & QGAME_UPDATE_PHASE_BIT_RENDER) != 0u) {
        engineMask |= updatePhaseBit(UpdatePhase::Render);
    }
    if ((nativeMask & QGAME_UPDATE_PHASE_BIT_POST_FRAME) != 0u) {
        engineMask |= updatePhaseBit(UpdatePhase::PostFrame);
    }
    return engineMask;
}

uint32_t enginePhaseToNativePhase(UpdatePhase phase) {
    switch (phase) {
        case UpdatePhase::Input:
            return QGAME_UPDATE_PHASE_INPUT;
        case UpdatePhase::GameplayPrePhysics:
            return QGAME_UPDATE_PHASE_GAMEPLAY_PRE_PHYSICS;
        case UpdatePhase::Physics:
            return QGAME_UPDATE_PHASE_PHYSICS;
        case UpdatePhase::GameplayPostPhysics:
            return QGAME_UPDATE_PHASE_GAMEPLAY_POST_PHYSICS;
        case UpdatePhase::Animation:
            return QGAME_UPDATE_PHASE_ANIMATION;
        case UpdatePhase::Camera:
            return QGAME_UPDATE_PHASE_CAMERA;
        case UpdatePhase::UI:
            return QGAME_UPDATE_PHASE_UI;
        case UpdatePhase::Render:
            return QGAME_UPDATE_PHASE_RENDER;
        case UpdatePhase::PostFrame:
            return QGAME_UPDATE_PHASE_POST_FRAME;
        case UpdatePhase::Count:
        default:
            return QGAME_UPDATE_PHASE_POST_FRAME;
    }
}

class NativeCallbackSystem final : public ISystem {
public:
    explicit NativeCallbackSystem(const QGameNativeSystemDesc& desc)
        : id_(desc.id ? desc.id : "")
        , userData_(desc.userData)
        , init_(desc.init)
        , preUpdate_(desc.pre_update)
        , update_(desc.update)
        , postUpdate_(desc.post_update)
        , shutdown_(desc.shutdown)
        , manuallyScheduled_(desc.manuallyScheduled) {}

    explicit NativeCallbackSystem(const QGameNativePhasedSystemDesc& desc)
        : id_(desc.id ? desc.id : "")
        , userData_(desc.userData)
        , init_(desc.init)
        , runPhase_(desc.run_phase)
        , shutdown_(desc.shutdown)
        , explicitPhaseMask_(nativePhaseMaskToEnginePhaseMask(desc.phaseMask)) {}

    void init() override {
        if (init_) init_(userData_);
    }

    UpdatePhaseMask phaseMask() const override {
        // 显式 phase 注册优先。如果 native mod 提供了 phased descriptor，
        // 它的生命周期完全由 phaseMask/runPhase 驱动，不再回退到旧的
        // pre/update/post 兼容映射。
        if (runPhase_) {
            return explicitPhaseMask_;
        }
        return ISystem::phaseMask();
    }

    bool runPhase(UpdatePhase phase, float dt) override {
        if (runPhase_) {
            return runPhase_(userData_, enginePhaseToNativePhase(phase), dt);
        }
        return ISystem::runPhase(phase, dt);
    }

    void preUpdate() override {
        if (preUpdate_) preUpdate_(userData_, 0.0f);
    }

    void update(float dt) override {
        if (update_) update_(userData_, dt);
    }

    void postUpdate() override {
        if (postUpdate_) postUpdate_(userData_, 0.0f);
    }

    void shutdown() override {
        if (shutdown_) shutdown_(userData_);
    }

    bool isManuallyScheduled() const override {
        return manuallyScheduled_;
    }

    const std::string& id() const { return id_; }

private:
    std::string id_;
    void* userData_ = nullptr;
    QGameNativeSystemInitFn init_ = nullptr;
    QGameNativeSystemUpdateFn preUpdate_ = nullptr;
    QGameNativeSystemUpdateFn update_ = nullptr;
    QGameNativeSystemUpdateFn postUpdate_ = nullptr;
    QGameNativeSystemRunPhaseFn runPhase_ = nullptr;
    QGameNativeSystemShutdownFn shutdown_ = nullptr;
    bool manuallyScheduled_ = false;
    UpdatePhaseMask explicitPhaseMask_ = 0u;
};

std::string normalizePath(const std::filesystem::path& path) {
    return path.lexically_normal().string();
}

std::filesystem::path resolveRelativeTo(const std::filesystem::path& baseDir,
                                        const std::string& path) {
    std::filesystem::path p(path);
    if (p.is_absolute()) return p.lexically_normal();
    return (baseDir / p).lexically_normal();
}

ModManager::NativeRuntime* runtimeFrom(QGameModContext* ctx) {
    if (!ctx || !ctx->userData) return nullptr;
    return static_cast<ModManager::NativeRuntime*>(ctx->userData);
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
    auto* runtime = runtimeFrom(ctx);
    if (!runtime || !runtime->game || !runtime->mod || !path) return false;
    return runtime->game->assets.loadManifestOverlay(
        path, "native:" + runtime->mod->mod.manifest.id);
}

bool modRegisterScene(QGameModContext* ctx, const char* id, const char* path) {
    auto* runtime = runtimeFrom(ctx);
    if (!runtime || !runtime->game || !id || !path) return false;
    return runtime->game->scenes && runtime->game->scenes->registerScene(id, path);
}

bool modRegisterSceneManifest(QGameModContext* ctx, const char* path) {
    auto* runtime = runtimeFrom(ctx);
    if (!runtime || !runtime->game || !path) return false;
    return runtime->game->scenes && runtime->game->scenes->registerManifest(path);
}

bool modRegisterPrefabManifest(QGameModContext* ctx, const char* path) {
    auto* runtime = runtimeFrom(ctx);
    if (!runtime || !runtime->game || !path) return false;
    return runtime->game->prefabs && runtime->game->prefabs->registerManifest(path);
}

bool modRegisterConfigManifest(QGameModContext* ctx, const char* path) {
    auto* runtime = runtimeFrom(ctx);
    if (!runtime || !runtime->game || !path) return false;
    return runtime->game->configs && runtime->game->configs->registerManifest(path);
}

bool modRegisterSystem(QGameModContext* ctx, const QGameNativeSystemDesc* desc) {
    auto* runtime = runtimeFrom(ctx);
    return runtime && runtime->manager && runtime->manager->registerNativeSystem(ctx, desc);
}

bool modRegisterPhasedSystem(QGameModContext* ctx, const QGameNativePhasedSystemDesc* desc) {
    auto* runtime = runtimeFrom(ctx);
    return runtime && runtime->manager &&
           runtime->manager->registerNativePhasedSystem(ctx, desc);
}

bool modSubscribeEvent(QGameModContext* ctx,
                       const char* eventName,
                       void* userData,
                       QGameEventHandlerFn handler) {
    auto* runtime = runtimeFrom(ctx);
    return runtime && runtime->manager &&
           runtime->manager->subscribeNativeEvent(ctx, eventName, userData, handler);
}

bool modEmitEvent(QGameModContext* ctx, const char* eventName, const char* payload) {
    auto* runtime = runtimeFrom(ctx);
    if (!runtime || !runtime->manager || !eventName) return false;
    return runtime->manager->emitNativeEvent(eventName, payload ? payload : "");
}

bool modRegisterAssetLoader(QGameModContext* ctx,
                            const char* type,
                            void* userData,
                            QGameAssetLoadFn load,
                            QGameAssetUnloadFn unload) {
    auto* runtime = runtimeFrom(ctx);
    return runtime && runtime->manager &&
           runtime->manager->registerNativeAssetLoader(ctx, type, userData, load, unload);
}

QGameLogAPI gLogApi{modLogInfo, modLogWarn, modLogError};
QGameAssetManagerAPI gAssetApi{modLoadAssetManifest};
QGameSceneRegistryAPI gSceneApi{modRegisterScene, modRegisterSceneManifest};
QGamePrefabRegistryAPI gPrefabApi{modRegisterPrefabManifest};
QGameConfigRegistryAPI gConfigApi{modRegisterConfigManifest};
QGameSystemRegistryAPI gSystemApi{modRegisterSystem, modRegisterPhasedSystem};
QGameEventBusAPI gEventApi{modSubscribeEvent, modEmitEvent};
QGameAssetLoaderRegistryAPI gAssetLoaderApi{modRegisterAssetLoader};
QGameComponentRegistryAPI gComponentApi{0};

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

    auto native = std::make_unique<NativeMod>();
    native->mod = mod;
    if (!native->library.open(normalizePath(libraryPath))) {
        return false;
    }

    native->init = reinterpret_cast<QGameModInitFn>(native->library.symbol("qgame_mod_init"));
    native->shutdown = reinterpret_cast<QGameModShutdownFn>(native->library.symbol("qgame_mod_shutdown"));
    if (!native->init || !native->shutdown) {
        core::logError("[ModManager] native mod missing qgame_mod_init/qgame_mod_shutdown: %s",
                       mod.manifest.id.c_str());
        return false;
    }

    native->runtime.game = &ctx;
    native->runtime.manager = this;
    native->runtime.mod = native.get();

    native->context.apiVersion = QGAME_MOD_API_VERSION;
    native->context.userData = &native->runtime;
    native->context.assets = &gAssetApi;
    native->context.scenes = &gSceneApi;
    native->context.prefabs = &gPrefabApi;
    native->context.configs = &gConfigApi;
    native->context.systems = &gSystemApi;
    native->context.events = &gEventApi;
    native->context.assetLoaders = &gAssetLoaderApi;
    native->context.components = &gComponentApi;
    native->context.log = &gLogApi;

    if (!native->init(&native->context)) {
        core::logError("[ModManager] qgame_mod_init failed: %s", mod.manifest.id.c_str());
        cleanupNativeRegistrations(*native);
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
        NativeMod& mod = *nativeMods_[static_cast<size_t>(i)];
        cleanupNativeRegistrations(mod);
        if (mod.shutdown) {
            mod.shutdown(&mod.context);
            core::logInfo("[ModManager] shutdown native mod: %s", mod.mod.manifest.id.c_str());
        }
    }
    nativeMods_.clear();
}

bool ModManager::registerNativeSystem(QGameModContext* ctx,
                                      const QGameNativeSystemDesc* desc) {
    auto* runtime = runtimeFrom(ctx);
    if (!runtime || !runtime->game || !runtime->mod || !desc || !desc->update) {
        core::logError("[ModManager] invalid native system registration");
        return false;
    }

    auto system = std::make_unique<NativeCallbackSystem>(*desc);
    NativeCallbackSystem* raw = system.get();
    ISystem& registered = runtime->game->systems.registerSystem(std::move(system));
    runtime->mod->systems.push_back(&registered);

    // Native mods are usually loaded after EngineContext::init() has already
    // called SystemRegistry::initAll(), so the wrapper is initialized eagerly.
    raw->init();
    core::logInfo("[ModManager] registered native system %s",
                  raw->id().empty() ? "(unnamed)" : raw->id().c_str());
    return true;
}

bool ModManager::registerNativePhasedSystem(QGameModContext* ctx,
                                            const QGameNativePhasedSystemDesc* desc) {
    auto* runtime = runtimeFrom(ctx);
    if (!runtime || !runtime->game || !runtime->mod || !desc || !desc->run_phase ||
        desc->phaseMask == 0u) {
        core::logError("[ModManager] invalid native phased system registration");
        return false;
    }

    auto system = std::make_unique<NativeCallbackSystem>(*desc);
    NativeCallbackSystem* raw = system.get();
    ISystem& registered = runtime->game->systems.registerSystem(std::move(system));
    runtime->mod->systems.push_back(&registered);

    // 和旧 API 一样，native mod 往往在引擎初始化完成后才被加载，因此这里要
    // 立即触发一次 init，避免错过 SystemRegistry::initAll()。
    raw->init();
    core::logInfo("[ModManager] registered native phased system %s",
                  raw->id().empty() ? "(unnamed)" : raw->id().c_str());
    return true;
}

bool ModManager::subscribeNativeEvent(QGameModContext* ctx,
                                      const char* eventName,
                                      void* userData,
                                      QGameEventHandlerFn handler) {
    auto* runtime = runtimeFrom(ctx);
    if (!runtime || !runtime->mod || !eventName || !handler) return false;

    NativeEventHandler sub{};
    sub.owner = runtime->mod;
    sub.eventName = eventName;
    sub.userData = userData;
    sub.handler = handler;
    eventHandlers_.push_back(sub);
    core::logInfo("[ModManager] native event handler registered: %s", eventName);
    return true;
}

bool ModManager::emitNativeEvent(const std::string& eventName, const std::string& payload) {
    bool delivered = false;
    for (const NativeEventHandler& sub : eventHandlers_) {
        if (sub.eventName == eventName && sub.handler) {
            sub.handler(sub.userData, eventName.c_str(), payload.c_str());
            delivered = true;
        }
    }
    return delivered;
}

bool ModManager::registerNativeAssetLoader(QGameModContext* ctx,
                                           const char* type,
                                           void* userData,
                                           QGameAssetLoadFn load,
                                           QGameAssetUnloadFn unload) {
    auto* runtime = runtimeFrom(ctx);
    if (!runtime || !runtime->game || !runtime->game->assetLoaders ||
        !runtime->mod || !type || !load) {
        return false;
    }

    NativeMod* owner = runtime->mod;
    return runtime->game->assetLoaders->registerLoader(
        type,
        [userData, load](const char* assetId, const char* path) {
            return load(userData, assetId, path);
        },
        [userData, unload](void* asset) {
            if (unload) unload(userData, asset);
        },
        owner,
        "native:" + owner->mod.manifest.id);
}

void ModManager::cleanupNativeRegistrations(NativeMod& mod) {
    if (mod.runtime.game) {
        for (ISystem* system : mod.systems) {
            mod.runtime.game->systems.unregisterSystem(system, true);
        }
        mod.systems.clear();

        if (mod.runtime.game->assetLoaders) {
            mod.runtime.game->assetLoaders->unregisterLoadersByOwner(&mod);
        }
    }

    eventHandlers_.erase(
        std::remove_if(eventHandlers_.begin(), eventHandlers_.end(),
                       [&](const NativeEventHandler& sub) { return sub.owner == &mod; }),
        eventHandlers_.end());
}

} // namespace engine
