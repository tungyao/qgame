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
    input/SDLInputProvider.cpp
    assets/AssetManager.cpp
    scene/SceneSerializer.cpp
)

target_compile_definitions(engine PRIVATE QGAME_ENGINE_EXPORTS)

# Windows: 自动导出所有符号，避免手动添加 dllexport
if(WIN32)
    set_target_properties(engine PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

target_include_directories(engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/.. ${CMAKE_CURRENT_SOURCE_DIR})

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
