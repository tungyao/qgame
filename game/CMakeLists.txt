add_executable(game
    main.cpp
)

target_link_libraries(game
    PRIVATE engine
    PRIVATE SDL3::SDL3-static
)

if(WIN32)
    # SDL3::SDL3main 提供 WinMain → SDL_main → 我们的 main 的转发
    # 某些 FetchContent 配置下该 target 不导出，用 if(TARGET) 保护
    if(TARGET SDL3::SDL3main)
        target_link_libraries(game PRIVATE SDL3::SDL3main)
    else()
        # 回退：保持 main() 为入口，MSVC 不再要求 WinMain
        target_link_options(game PRIVATE /ENTRY:mainCRTStartup)
    endif()

    # 隐藏控制台窗口
    set_target_properties(game PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
