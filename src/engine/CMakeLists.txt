add_library(engine STATIC
    runtime/EngineContext.cpp
    runtime/FrameScheduler.cpp
    api/GameAPI.cpp
    api/EditorAPI.cpp
    systems/RenderSystem.cpp
    systems/AudioSystem.cpp
    systems/PhysicsSystem.cpp
    systems/InputSystem.cpp
    input/SDLInputProvider.cpp
)

target_include_directories(engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

if(BUILD_EDITOR)
    target_include_directories(engine PRIVATE
        "${imgui_SOURCE_DIR}"
        "${imgui_SOURCE_DIR}/backends"
    )
endif()

target_link_libraries(engine
    PUBLIC  core
    PUBLIC  EnTT::EnTT
    PRIVATE backend
    PRIVATE platform
    PRIVATE SDL3::SDL3-static
)

if(BUILD_EDITOR)
    target_compile_definitions(engine PUBLIC BUILD_EDITOR)
    target_link_libraries(engine PRIVATE imgui)
endif()
