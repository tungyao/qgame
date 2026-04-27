add_library(engine SHARED
    runtime/EngineContext.cpp
    runtime/FrameScheduler.cpp
    api/GameAPI.cpp
    api/GameAPI_ExplicitInstantiation.cpp
    api/EditorAPI.cpp
    systems/RenderSystem.cpp
    systems/AudioSystem.cpp
    systems/PhysicsSystem.cpp
    systems/InputSystem.cpp
    systems/AnimatorSystem.cpp
    systems/TweenSystem.cpp
    systems/UISystem.cpp
    input/SDLInputProvider.cpp
    assets/AssetManager.cpp
    assets/FontLoader.cpp
    scene/SceneSerializer.cpp
    resources/GPUDrivenRenderer.cpp
)

target_compile_definitions(engine PRIVATE QGAME_ENGINE_EXPORTS)

# Windows: 自动导出所有符号，避免手动添加 dllexport
if(WIN32)
    set_target_properties(engine PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

target_include_directories(engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/.. ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(engine PRIVATE "${CMAKE_BINARY_DIR}/generated/shaders")

set(ENGINE_SHADER_DEPS "${CMAKE_BINARY_DIR}/generated/shaders/sprite_culling_spv.h")
if(DXC_EXE)
    list(APPEND ENGINE_SHADER_DEPS "${CMAKE_BINARY_DIR}/generated/shaders/sprite_culling_dxil.h")
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
    PRIVATE nlohmann_json::nlohmann_json
)

if(BUILD_EDITOR)
    target_compile_definitions(engine PUBLIC BUILD_EDITOR)
endif()
