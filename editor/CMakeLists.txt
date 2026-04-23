add_executable(editor
    main.cpp
    EditorApplication.cpp
)

target_link_libraries(editor
    PRIVATE engine
    PRIVATE imgui
    PRIVATE SDL3::SDL3-static
)

if(WIN32)
    if(TARGET SDL3::SDL3main)
        target_link_libraries(editor PRIVATE SDL3::SDL3main)
    else()
        target_link_options(editor PRIVATE /ENTRY:mainCRTStartup)
    endif()

    set_target_properties(editor PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
