add_library(platform STATIC
    Window.cpp
)

target_include_directories(platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_libraries(platform
    PUBLIC  core
    PRIVATE SDL3::SDL3-static
)
