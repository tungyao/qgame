#pragma once

#include "GameManifest.h"
#include "ModManifest.h"
#include "NativeModAPI.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine {

class AssetManager;
class ISystem;
struct GameContext;

// ModManager owns the S3 resource-package layer:
// - discover mod.json files under a project mods/ directory;
// - resolve enabled mods plus dependencies into a deterministic mount order;
// - mount each mod asset manifest as an AssetManager overlay.
//
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
    bool mountDataMods(GameContext& ctx);
    bool mountGameAndMods(GameContext& ctx,
                          const std::string& gameManifestPath,
                          const GameManifest& gameManifest);

    bool loadNativeMods(GameContext& ctx);
    void shutdownNativeMods();
    bool emitNativeEvent(const std::string& eventName, const std::string& payload);

    const std::vector<LoadedMod>& mountedMods() const { return mountedMods_; }

    struct NativeLibrary {
        NativeLibrary() = default;
        ~NativeLibrary();
        NativeLibrary(const NativeLibrary&) = delete;
        NativeLibrary& operator=(const NativeLibrary&) = delete;
        NativeLibrary(NativeLibrary&& other) noexcept;
        NativeLibrary& operator=(NativeLibrary&& other) noexcept;

        bool open(const std::string& path);
        void* symbol(const char* name) const;
        void close();

        void* handle = nullptr;
    };

    struct NativeMod;

    struct NativeRuntime {
        GameContext* game = nullptr;
        ModManager* manager = nullptr;
        NativeMod* mod = nullptr;
    };

    struct NativeMod {
        LoadedMod mod;
        NativeLibrary library;
        QGameModInitFn init = nullptr;
        QGameModShutdownFn shutdown = nullptr;
        QGameModContext context{};
        std::vector<ISystem*> systems;
        NativeRuntime runtime;
    };

    struct NativeEventHandler {
        NativeMod* owner = nullptr;
        std::string eventName;
        void* userData = nullptr;
        QGameEventHandlerFn handler = nullptr;
    };

    bool registerNativeSystem(QGameModContext* ctx, const QGameNativeSystemDesc* desc);
    bool registerNativePhasedSystem(QGameModContext* ctx,
                                    const QGameNativePhasedSystemDesc* desc);
    bool subscribeNativeEvent(QGameModContext* ctx,
                              const char* eventName,
                              void* userData,
                              QGameEventHandlerFn handler);
    bool registerNativeAssetLoader(QGameModContext* ctx,
                                   const char* type,
                                   void* userData,
                                   QGameAssetLoadFn load,
                                   QGameAssetUnloadFn unload);

private:
    struct DiscoveredMod {
        ModManifest manifest;
        std::string rootDir;
    };

    bool mountDataForMod(GameContext& ctx, const LoadedMod& mod);
    bool loadNativeMod(GameContext& ctx, const LoadedMod& mod);
    void cleanupNativeRegistrations(NativeMod& mod);

    std::unordered_map<std::string, DiscoveredMod> discovered_;
    std::string modsDir_;
    std::vector<LoadedMod> mountedMods_;
    std::vector<std::unique_ptr<NativeMod>> nativeMods_;
    std::vector<NativeEventHandler> eventHandlers_;
};

} // namespace engine
