cmake_minimum_required(VERSION 3.20)
project(asset_pack_smoke)

add_executable(asset_pack_smoke main.cpp)

target_link_libraries(asset_pack_smoke PRIVATE engine backend SDL3::SDL3)
target_include_directories(asset_pack_smoke PRIVATE ${CMAKE_SOURCE_DIR}/src)

if(TARGET qgame_assets)
    add_dependencies(asset_pack_smoke qgame_assets)
    target_compile_definitions(asset_pack_smoke PRIVATE
        QGAME_PACK_TEST_MANIFEST="${QGAME_BAKED_MANIFEST}"
    )
else()
    target_compile_definitions(asset_pack_smoke PRIVATE
        QGAME_PACK_TEST_MANIFEST="${CMAKE_SOURCE_DIR}/assets/manifest.json"
    )
endif()

add_test(NAME asset_pack_smoke COMMAND asset_pack_smoke)

if(WIN32)
    add_custom_command(TARGET asset_pack_smoke POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:asset_pack_smoke>
                $<TARGET_FILE_DIR:asset_pack_smoke>
        COMMAND_EXPAND_LISTS
    )
endif()
