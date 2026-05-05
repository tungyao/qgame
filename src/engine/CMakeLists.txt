add_library(engine SHARED
    runtime/EngineContext.cpp
    runtime/FrameScheduler.cpp
    framework/AssetLoaderRegistry.cpp
    framework/ConfigRegistry.cpp
    framework/GameInstance.cpp
    framework/GameManifest.cpp
    framework/ModManifest.cpp
    framework/ModManager.cpp
    framework/PrefabRegistry.cpp
    framework/SceneManager.cpp
    api/GameAPI.cpp
    api/GameAPI_ExplicitInstantiation.cpp
    api/EditorAPI.cpp
    systems/RenderSystem.cpp
    systems/AudioSystem.cpp
    systems/PhysicsSystem.cpp
    systems/InputSystem.cpp
    systems/SceneSystem.cpp
    systems/AnimatorSystem.cpp
    systems/TweenSystem.cpp
    systems/UISystem.cpp
    input/SDLInputProvider.cpp
    assets/AssetManager.cpp
    assets/FontLoader.cpp
    prefabs/PlayerPrefab.cpp
    scene/ComponentJson.cpp
    scene/SceneSerializer.cpp
    resources/GPUDrivenRenderer.cpp
    resources/GPUParticleRenderer.cpp
)

target_compile_definitions(engine PRIVATE QGAME_ENGINE_EXPORTS)

# Windows: 自动导出所有符号，避免手动添加 dllexport
if(WIN32)
    set_target_properties(engine PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

target_include_directories(engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/.. ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(engine PRIVATE "${CMAKE_BINARY_DIR}/generated/shaders")

set(ENGINE_SHADER_DEPS
    "${CMAKE_BINARY_DIR}/generated/shaders/sprite_culling_spv.h"
    "${CMAKE_BINARY_DIR}/generated/shaders/particle_update_spv.h"
    "${CMAKE_BINARY_DIR}/generated/shaders/particle_sort_spv.h"
)
if(DXC_EXE)
    list(APPEND ENGINE_SHADER_DEPS "${CMAKE_BINARY_DIR}/generated/shaders/sprite_culling_dxil.h")
    list(APPEND ENGINE_SHADER_DEPS "${CMAKE_BINARY_DIR}/generated/shaders/particle_update_dxil.h")
    list(APPEND ENGINE_SHADER_DEPS "${CMAKE_BINARY_DIR}/generated/shaders/particle_sort_dxil.h")
    target_compile_definitions(engine PRIVATE QGAME_HAS_DXIL_SHADERS)


endif()
set_source_files_properties(
    resources/GPUDrivenRenderer.cpp PROPERTIES
    OBJECT_DEPENDS "${ENGINE_SHADER_DEPS}"
)

target_link_libraries(engine
    PUBLIC  core
    PUBLIC  EnTT::EnTT
    PRIVATE backend
    PRIVATE platform
    PRIVATE SDL3::SDL3
    PRIVATE stb_image
    PUBLIC  nlohmann_json::nlohmann_json
)

if(CMAKE_DL_LIBS)
    target_link_libraries(engine PRIVATE ${CMAKE_DL_LIBS})
endif()

if(BUILD_EDITOR)
    target_compile_definitions(engine PUBLIC BUILD_EDITOR)
endif()
