add_executable(demo8
    main.cpp
)

target_link_libraries(demo8
    PRIVATE engine
    PRIVATE SDL3::SDL3
    PRIVATE stb_image
)

if(TARGET qgame_assets)
    add_dependencies(demo8 qgame_assets)
    target_compile_definitions(demo8 PRIVATE
        QGAME_BAKED_MANIFEST="${QGAME_BAKED_MANIFEST}"
    )
endif()

if(APPLE)
    set_target_properties(demo8 PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "@executable_path"
    )
elseif(UNIX)
    set_target_properties(demo8 PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "$ORIGIN"
    )
endif()

if(WIN32)
    add_custom_command(TARGET demo8 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_RUNTIME_DLLS:demo8>" "$<TARGET_FILE_DIR:demo8>"
        COMMAND_EXPAND_LISTS
        VERBATIM
    )
    if(TARGET SDL3::SDL3main)
        target_link_libraries(demo8 PRIVATE SDL3::SDL3main)
    else()
        target_link_options(demo8 PRIVATE /ENTRY:mainCRTStartup)
    endif()
    set_target_properties(demo8 PROPERTIES WIN32_EXECUTABLE TRUE)
else()
    add_custom_command(TARGET demo8 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:engine>" "$<TARGET_FILE_DIR:demo8>"
        VERBATIM
    )
    if(TARGET SDL3::SDL3-shared)
        add_custom_command(TARGET demo8 POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:SDL3::SDL3-shared>" "$<TARGET_FILE_DIR:demo8>"
        )
    endif()
endif()
