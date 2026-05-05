#pragma once

#include <cstdint>

namespace engine {

inline constexpr uint32_t QGAME_MOD_API_VERSION = 1;

struct QGameModContext;

using QGameModInitFn = bool (*)(QGameModContext* ctx);
using QGameModShutdownFn = void (*)(QGameModContext* ctx);

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

// C ABI surface passed to native mods. The pointed-to function tables are owned
// by ModManager for the duration of qgame_mod_init/qgame_mod_shutdown calls.
struct QGameModContext {
    uint32_t apiVersion;
    void* userData;

    QGameAssetManagerAPI* assets;
    QGameSceneRegistryAPI* scenes;
    QGamePrefabRegistryAPI* prefabs;
    QGameConfigRegistryAPI* configs;
    QGameLogAPI* log;
};

} // namespace engine

#if defined(_WIN32)
    #define QGAME_MOD_EXPORT extern "C" __declspec(dllexport)
#else
    #define QGAME_MOD_EXPORT extern "C" __attribute__((visibility("default")))
#endif
