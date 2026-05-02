cmake_minimum_required(VERSION 3.20)
project(asset_manifest_demo)

add_executable(asset_manifest_demo main.cpp)

target_link_libraries(asset_manifest_demo PRIVATE SDL3::SDL3 engine)
target_include_directories(asset_manifest_demo PRIVATE ${CMAKE_SOURCE_DIR}/src)

if(TARGET qgame_assets)
    add_dependencies(asset_manifest_demo qgame_assets)
    target_compile_definitions(asset_manifest_demo PRIVATE
        QGAME_DEMO_MANIFEST="${QGAME_BAKED_MANIFEST}"
    )
else()
    target_compile_definitions(asset_manifest_demo PRIVATE
        QGAME_DEMO_MANIFEST="${CMAKE_SOURCE_DIR}/assets/manifest.json"
    )
endif()
