add_executable(tests
    test_core.cpp
)
target_link_libraries(tests PRIVATE core)
add_test(NAME CoreTests COMMAND tests)

# RenderSystem 测试：不需要 GPU，只测 CommandBuffer + ECS 逻辑
add_executable(tests_render
    test_render_system.cpp
)
target_link_libraries(tests_render PRIVATE engine)
add_test(NAME RenderTests COMMAND tests_render)

# AudioSystem 测试：不需要音频硬件，只测队列/dispatch/handle 逻辑
add_executable(tests_audio
    test_audio_system.cpp
)
target_include_directories(tests_audio PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tests_audio PRIVATE core)
add_test(NAME AudioTests COMMAND tests_audio)

# InputState 单元测试：纯逻辑，不需要窗口/SDL
add_executable(tests_input
    test_input_system.cpp
)
target_link_libraries(tests_input PRIVATE engine)
add_test(NAME InputTests COMMAND tests_input)

# 交互式输入测试：需要窗口，手动运行
add_executable(test_input_interactive
    test_input_interactive.cpp
)
target_link_libraries(test_input_interactive PRIVATE engine SDL3::SDL3-static)
if(WIN32)
    # Windows 控制台窗口：让 printf 可见
    set_target_properties(test_input_interactive PROPERTIES
        WIN32_EXECUTABLE FALSE
    )
endif()

# PhysicsSystem 测试：不需要 GPU/音频
add_executable(tests_physics
    test_physics_system.cpp
)
target_link_libraries(tests_physics PRIVATE engine)
add_test(NAME PhysicsTests COMMAND tests_physics)
