add_executable(demo2
    main.cpp
)

set(QGAME_DEMO2_GAME_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/game.json")

target_link_libraries(demo2
    PRIVATE engine
    PRIVATE SDL3::SDL3
)

target_compile_definitions(demo2 PRIVATE
    QGAME_DEMO2_GAME_MANIFEST="${QGAME_DEMO2_GAME_MANIFEST}"
)

if(TARGET qgame_assets)
    add_dependencies(demo2 qgame_assets)
endif()

if(APPLE)
    set_target_properties(demo2 PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "@executable_path"
    )
elseif(UNIX)
    set_target_properties(demo2 PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "$ORIGIN"
    )
endif()

if(WIN32)
    add_custom_command(TARGET demo2 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_RUNTIME_DLLS:demo2>" "$<TARGET_FILE_DIR:demo2>"
        COMMAND_EXPAND_LISTS
        VERBATIM
    )

    if(TARGET SDL3::SDL3main)
        target_link_libraries(demo2 PRIVATE SDL3::SDL3main)
    else()
        target_link_options(demo2 PRIVATE /ENTRY:mainCRTStartup)
    endif()

    set_target_properties(demo2 PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
