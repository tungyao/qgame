add_executable(game
    main.cpp
)

target_link_libraries(game
    PRIVATE engine
    PRIVATE SDL3::SDL3
    PRIVATE stb_image
)

# 让游戏可执行文件能在自身目录旁找到 engine/SDL3 共享库
if(APPLE)
    set_target_properties(game PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "@executable_path"
    )
elseif(UNIX)
    set_target_properties(game PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "$ORIGIN"
    )
endif()

# 把 engine 及其依赖的运行时库（SDL3 等）拷贝到 game 可执行文件旁
if(WIN32)
    # CMake 3.21+：自动收集所有运行时 DLL
    add_custom_command(TARGET game POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_RUNTIME_DLLS:game>" "$<TARGET_FILE_DIR:game>"
        COMMAND_EXPAND_LISTS
        VERBATIM
    )
else()
    add_custom_command(TARGET game POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:engine>" "$<TARGET_FILE_DIR:game>"
        VERBATIM
    )
    if(TARGET SDL3::SDL3-shared)
        add_custom_command(TARGET game POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:SDL3::SDL3-shared>" "$<TARGET_FILE_DIR:game>"
            VERBATIM
        )
    endif()
endif()

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
