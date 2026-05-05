#pragma once

#include <cstdint>

namespace engine {

inline constexpr uint32_t QGAME_MOD_API_VERSION = 1;

struct QGameModContext;

using QGameModInitFn = bool (*)(QGameModContext* ctx);
using QGameModShutdownFn = void (*)(QGameModContext* ctx);
using QGameNativeSystemInitFn = void (*)(void* userData);
using QGameNativeSystemUpdateFn = void (*)(void* userData, float dt);
using QGameNativeSystemShutdownFn = void (*)(void* userData);
using QGameEventHandlerFn = void (*)(void* userData, const char* eventName, const char* payload);
using QGameAssetLoadFn = void* (*)(void* userData, const char* assetId, const char* path);
using QGameAssetUnloadFn = void (*)(void* userData, void* asset);

struct QGameNativeSystemDesc {
    const char* id;
    void* userData;
    QGameNativeSystemInitFn init;
    QGameNativeSystemUpdateFn pre_update;
    QGameNativeSystemUpdateFn update;
    QGameNativeSystemUpdateFn post_update;
    QGameNativeSystemShutdownFn shutdown;
    bool manuallyScheduled;
};

struct QGameLogAPI {
    void (*info)(const char* message);
    void (*warn)(const char* message);
    void (*error)(const char* message);
};

struct QGameAssetManagerAPI {
    bool (*load_manifest)(QGameModContext* ctx, const char* path);
};

struct QGameSceneRegistryAPI {
    bool (*register_scene)(QGameModContext* ctx, const char* id, const char* path);
    bool (*register_manifest)(QGameModContext* ctx, const char* path);
};

struct QGamePrefabRegistryAPI {
    bool (*register_manifest)(QGameModContext* ctx, const char* path);
};

struct QGameConfigRegistryAPI {
    bool (*register_manifest)(QGameModContext* ctx, const char* path);
};

struct QGameSystemRegistryAPI {
    bool (*register_system)(QGameModContext* ctx, const QGameNativeSystemDesc* desc);
};

struct QGameEventBusAPI {
    bool (*subscribe)(QGameModContext* ctx,
                      const char* eventName,
                      void* userData,
                      QGameEventHandlerFn handler);
    bool (*emit)(QGameModContext* ctx, const char* eventName, const char* payload);
};

struct QGameAssetLoaderRegistryAPI {
    bool (*register_loader)(QGameModContext* ctx,
                            const char* type,
                            void* userData,
                            QGameAssetLoadFn load,
                            QGameAssetUnloadFn unload);
};

struct QGameComponentRegistryAPI {
    uint32_t reserved;
};

// C ABI surface passed to native mods. The pointed-to function tables are owned
// by ModManager for the duration of qgame_mod_init/qgame_mod_shutdown calls.
struct QGameModContext {
    uint32_t apiVersion;
    void* userData;

    QGameAssetManagerAPI* assets;
    QGameSceneRegistryAPI* scenes;
    QGamePrefabRegistryAPI* prefabs;
    QGameConfigRegistryAPI* configs;
    QGameSystemRegistryAPI* systems;
    QGameEventBusAPI* events;
    QGameAssetLoaderRegistryAPI* assetLoaders;
    QGameComponentRegistryAPI* components;
    QGameLogAPI* log;
};

} // namespace engine

#if defined(_WIN32)
    #define QGAME_MOD_EXPORT extern "C" __declspec(dllexport)
#else
    #define QGAME_MOD_EXPORT extern "C" __attribute__((visibility("default")))
#endif
