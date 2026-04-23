cmake_minimum_required(VERSION 3.20)
project(StarEngine CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── 编译选项 ─────────────────────────────────────────────────────────────────
if(MSVC)
    add_compile_options(
        /W4 /MP
        /Zc:__cplusplus          # 让 __cplusplus 返回正确值（EnTT 等需要）
        /utf-8                   # 源文件 UTF-8，避免中文注释编译警告
    )
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

# ── FetchContent ──────────────────────────────────────────────────────────────
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# SDL3
FetchContent_Declare(SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG        release-3.4.4
    GIT_SHALLOW    TRUE
    OVERRIDE_FIND_PACKAGE   # CMake 3.24+: find_package(SDL3) → FetchContent
)
set(SDL_SHARED  OFF CACHE BOOL "" FORCE)
set(SDL_STATIC  ON  CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS   OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(SDL3)

# SDL3_mixer — Month 4 接入（AudioSystem + AudioThread）
# 依赖 SDL3::SDL3 shared target，届时改 SDL_SHARED=ON 或用 ALIAS
FetchContent_Declare(SDL3_mixer
    GIT_REPOSITORY https://github.com/libsdl-org/SDL_mixer.git
    GIT_TAG        release-3.2.0
    GIT_SHALLOW    TRUE
)
set(SDL3MIXER_VENDORED    ON  CACHE BOOL "" FORCE)
set(SDL3MIXER_OPUS        OFF CACHE BOOL "" FORCE)
set(SDL3MIXER_MOD         OFF CACHE BOOL "" FORCE)
set(SDL3MIXER_MIDI        OFF CACHE BOOL "" FORCE)
set(SDL3MIXER_WAVPACK     OFF CACHE BOOL "" FORCE)  # wavpack x64 ASM 在 MSVC 下有链接问题
set(WAVPACK_ENABLE_ASM    OFF CACHE BOOL "" FORCE)  # 保险：即使被拉进来也禁掉 ASM
set(SDL3MIXER_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS     OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(SDL3_mixer)

# EnTT
FetchContent_Declare(EnTT
    GIT_REPOSITORY https://github.com/skypjack/entt.git
    GIT_TAG        v3.16.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(EnTT)

# Dear ImGui（header-only 方式，Month 6 再接入）
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.92.7
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(imgui)

# 把 imgui 封装成 CMake target（imgui 本身不带 CMakeLists）
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp   # 占位，后期换 bgfx backend
)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui PUBLIC SDL3::SDL3-static)

# nlohmann/json
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.12.0
    GIT_SHALLOW    TRUE
)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install    OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(nlohmann_json)

# ── 引擎模块（按依赖顺序）────────────────────────────────────────────────────
add_subdirectory(src/core)
add_subdirectory(src/platform)
add_subdirectory(src/backend)
add_subdirectory(src/engine)

# ── 可选目标 ──────────────────────────────────────────────────────────────────
option(BUILD_EDITOR "Build editor" ON)
option(BUILD_GAME   "Build game"   ON)
option(BUILD_TESTS  "Build tests"  ON)

if(BUILD_GAME)
    add_subdirectory(game)
endif()

if(BUILD_EDITOR)
    # add_subdirectory(editor)   # Month 6
endif()

if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
