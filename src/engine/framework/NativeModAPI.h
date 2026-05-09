#pragma once

#include <cstdint>

namespace engine {

inline constexpr uint32_t QGAME_MOD_API_VERSION = 1;

struct QGameModContext;

using QGameModInitFn = bool (*)(QGameModContext* ctx);
using QGameModShutdownFn = void (*)(QGameModContext* ctx);
using QGameNativeSystemInitFn = void (*)(void* userData);
using QGameNativeSystemUpdateFn = void (*)(void* userData, float dt);
using QGameNativeSystemRunPhaseFn = bool (*)(void* userData, uint32_t phase, float dt);
using QGameNativeSystemShutdownFn = void (*)(void* userData);
using QGameEventHandlerFn = void (*)(void* userData, const char* eventName, const char* payload);
using QGameAssetLoadFn = void* (*)(void* userData, const char* assetId, const char* path);
using QGameAssetUnloadFn = void (*)(void* userData, void* asset);

// C ABI phase ids used by native mods. These values intentionally mirror the
// engine's internal phase order, but are defined here independently so native
// code does not need to include any C++-only engine headers.
enum QGameUpdatePhase : uint32_t {
    QGAME_UPDATE_PHASE_INPUT = 0,
    QGAME_UPDATE_PHASE_GAMEPLAY_PRE_PHYSICS = 1,
    QGAME_UPDATE_PHASE_PHYSICS = 2,
    QGAME_UPDATE_PHASE_GAMEPLAY_POST_PHYSICS = 3,
    QGAME_UPDATE_PHASE_ANIMATION = 4,
    QGAME_UPDATE_PHASE_CAMERA = 5,
    QGAME_UPDATE_PHASE_UI = 6,
    QGAME_UPDATE_PHASE_RENDER = 7,
    QGAME_UPDATE_PHASE_POST_FRAME = 8
};

inline constexpr uint32_t QGAME_UPDATE_PHASE_BIT_INPUT =
    1u << static_cast<uint32_t>(QGAME_UPDATE_PHASE_INPUT);
inline constexpr uint32_t QGAME_UPDATE_PHASE_BIT_GAMEPLAY_PRE_PHYSICS =
    1u << static_cast<uint32_t>(QGAME_UPDATE_PHASE_GAMEPLAY_PRE_PHYSICS);
inline constexpr uint32_t QGAME_UPDATE_PHASE_BIT_PHYSICS =
    1u << static_cast<uint32_t>(QGAME_UPDATE_PHASE_PHYSICS);
inline constexpr uint32_t QGAME_UPDATE_PHASE_BIT_GAMEPLAY_POST_PHYSICS =
    1u << static_cast<uint32_t>(QGAME_UPDATE_PHASE_GAMEPLAY_POST_PHYSICS);
inline constexpr uint32_t QGAME_UPDATE_PHASE_BIT_ANIMATION =
    1u << static_cast<uint32_t>(QGAME_UPDATE_PHASE_ANIMATION);
inline constexpr uint32_t QGAME_UPDATE_PHASE_BIT_CAMERA =
    1u << static_cast<uint32_t>(QGAME_UPDATE_PHASE_CAMERA);
inline constexpr uint32_t QGAME_UPDATE_PHASE_BIT_UI =
    1u << static_cast<uint32_t>(QGAME_UPDATE_PHASE_UI);
inline constexpr uint32_t QGAME_UPDATE_PHASE_BIT_RENDER =
    1u << static_cast<uint32_t>(QGAME_UPDATE_PHASE_RENDER);
inline constexpr uint32_t QGAME_UPDATE_PHASE_BIT_POST_FRAME =
    1u << static_cast<uint32_t>(QGAME_UPDATE_PHASE_POST_FRAME);

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

// New native-system registration path for the explicit phase scheduler.
//
// Unlike the legacy QGameNativeSystemDesc, this descriptor does not rely on the
// old pre/update/post triad. The mod declares the phases it wants to
// participate in via phaseMask, and the host calls run_phase for each selected
// phase with the matching QGameUpdatePhase value.
//
// The legacy registration API is still supported for backward compatibility.
struct QGameNativePhasedSystemDesc {
    const char* id;
    void* userData;
    QGameNativeSystemInitFn init;
    QGameNativeSystemRunPhaseFn run_phase;
    QGameNativeSystemShutdownFn shutdown;
    uint32_t phaseMask;
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
    bool (*register_phased_system)(QGameModContext* ctx, const QGameNativePhasedSystemDesc* desc);
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
