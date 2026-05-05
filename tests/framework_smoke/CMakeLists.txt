cmake_minimum_required(VERSION 3.20)
project(framework_smoke)

add_executable(framework_smoke main.cpp)

target_link_libraries(framework_smoke PRIVATE engine SDL3::SDL3)
target_include_directories(framework_smoke PRIVATE ${CMAKE_SOURCE_DIR}/src)

add_test(NAME framework_smoke COMMAND framework_smoke)

if(WIN32)
    add_custom_command(TARGET framework_smoke POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:framework_smoke>
                $<TARGET_FILE_DIR:framework_smoke>
        COMMAND_EXPAND_LISTS
    )
endif()
