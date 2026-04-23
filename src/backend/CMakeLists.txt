include(${CMAKE_SOURCE_DIR}/cmake/CompileShaders.cmake)

set(SHADER_SRC  "${CMAKE_SOURCE_DIR}/assets/shaders")
set(SHADER_GEN  "${CMAKE_BINARY_DIR}/generated/shaders")
set(SHADER_FALLBACK "${CMAKE_CURRENT_SOURCE_DIR}/renderer/sdl_gpu/shaders_precompiled")

compile_shader_spirv(
    "${SHADER_SRC}/sprite.vert.glsl"  vert
    "${SHADER_GEN}/sprite_vert_spv.h" sprite_vert_spv
    "${SHADER_FALLBACK}/sprite_vert_spv.h"
)
compile_shader_spirv(
    "${SHADER_SRC}/sprite.frag.glsl"  frag
    "${SHADER_GEN}/sprite_frag_spv.h" sprite_frag_spv
    "${SHADER_FALLBACK}/sprite_frag_spv.h"
)

add_library(backend STATIC
    renderer/CommandBuffer.cpp
    renderer/sdl_gpu/SDLGPURenderDevice.cpp
    audio/sdl_mixer/SDLMixerAudioDevice.cpp
)

# 让 CMake 知道这些生成文件是 backend 的依赖
set_source_files_properties(
    renderer/sdl_gpu/SDLGPURenderDevice.cpp PROPERTIES
    OBJECT_DEPENDS "${SHADER_GEN}/sprite_vert_spv.h;${SHADER_GEN}/sprite_frag_spv.h"
)

# PRIVATE — backend 头文件不向上层传播
target_include_directories(backend
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..
    PRIVATE "${SHADER_GEN}"
    PRIVATE "${SDL3_mixer_SOURCE_DIR}/include"
)

target_link_libraries(backend
    PRIVATE core
    PRIVATE platform
    PRIVATE SDL3::SDL3-static
    PRIVATE SDL3_mixer::SDL3_mixer-static
)
