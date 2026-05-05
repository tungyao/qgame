cmake_minimum_required(VERSION 3.20)
project(framework_smoke)

add_executable(framework_smoke main.cpp)
add_library(framework_smoke_native_mod SHARED sample_native_mod.cpp)

target_link_libraries(framework_smoke PRIVATE engine SDL3::SDL3)
target_include_directories(framework_smoke PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_include_directories(framework_smoke_native_mod PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_compile_definitions(framework_smoke PRIVATE
    QGAME_SMOKE_NATIVE_MOD_LIB="$<TARGET_FILE:framework_smoke_native_mod>"
)
add_dependencies(framework_smoke framework_smoke_native_mod)

add_test(NAME framework_smoke COMMAND framework_smoke)

if(WIN32)
    add_custom_command(TARGET framework_smoke POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:framework_smoke>
                $<TARGET_FILE_DIR:framework_smoke>
        COMMAND_EXPAND_LISTS
    )
endif()
