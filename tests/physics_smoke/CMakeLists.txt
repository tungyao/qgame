cmake_minimum_required(VERSION 3.20)
project(physics_smoke)

add_executable(physics_smoke main.cpp)
target_link_libraries(physics_smoke PRIVATE box2d::box2d)
add_test(NAME physics_smoke COMMAND physics_smoke)

if(WIN32)
    add_custom_command(TARGET physics_smoke POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:physics_smoke>
                $<TARGET_FILE_DIR:physics_smoke>
        COMMAND_EXPAND_LISTS
    )
endif()
