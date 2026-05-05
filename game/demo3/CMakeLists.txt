add_executable(demo3
    main.cpp
)

target_link_libraries(demo3
    PRIVATE engine
    PRIVATE SDL3::SDL3
)

if(TARGET qgame_assets)
    add_dependencies(demo3 qgame_assets)
    target_compile_definitions(demo3 PRIVATE
        QGAME_BAKED_MANIFEST="${QGAME_BAKED_MANIFEST}"
    )
endif()

if(APPLE)
    set_target_properties(demo3 PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "@executable_path"
    )
elseif(UNIX)
    set_target_properties(demo3 PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "$ORIGIN"
    )
endif()

if(WIN32)
    add_custom_command(TARGET demo3 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_RUNTIME_DLLS:demo3>" "$<TARGET_FILE_DIR:demo3>"
        COMMAND_EXPAND_LISTS
        VERBATIM
    )

    if(TARGET SDL3::SDL3main)
        target_link_libraries(demo3 PRIVATE SDL3::SDL3main)
    else()
        target_link_options(demo3 PRIVATE /ENTRY:mainCRTStartup)
    endif()

    set_target_properties(demo3 PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
