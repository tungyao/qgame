include(${CMAKE_SOURCE_DIR}/cmake/CompileShaders.cmake)

set(SHADER_SRC      "${CMAKE_SOURCE_DIR}/assets/shaders")
set(SHADER_GEN      "${CMAKE_BINARY_DIR}/generated/shaders")
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
compile_shader_spirv(
    "${SHADER_SRC}/msdf.frag.glsl"  frag
    "${SHADER_GEN}/msdf_frag_spv.h" msdf_frag_spv
    "${SHADER_FALLBACK}/msdf_frag_spv.h"
)

set(DXIL_SHADER_DEPS "")
if(DXC_EXE)
    compile_shader_dxil(
        "${SHADER_SRC}/sprite.vert.hlsl"   vs
        "${SHADER_GEN}/sprite_vert_dxil.h" sprite_vert_dxil
        "${SHADER_FALLBACK}/sprite_vert_dxil.h"
    )
    compile_shader_dxil(
        "${SHADER_SRC}/sprite.frag.hlsl"   ps
        "${SHADER_GEN}/sprite_frag_dxil.h" sprite_frag_dxil
        "${SHADER_FALLBACK}/sprite_frag_dxil.h"
    )
    compile_shader_dxil(
        "${SHADER_SRC}/msdf.frag.hlsl"   ps
        "${SHADER_GEN}/msdf_frag_dxil.h" msdf_frag_dxil
        "${SHADER_FALLBACK}/msdf_frag_dxil.h"
    )
    list(APPEND DXIL_SHADER_DEPS
        "${SHADER_GEN}/sprite_vert_dxil.h"
        "${SHADER_GEN}/sprite_frag_dxil.h"
        "${SHADER_GEN}/msdf_frag_dxil.h"
    )
endif()

add_library(backend STATIC
    renderer/CommandBuffer.cpp
    renderer/sdl_gpu/SDLGPURenderDevice.cpp
    renderer/opengl/GLRenderDevice.cpp
    audio/sdl_mixer/SDLMixerAudioDevice.cpp
)

set(ALL_SHADER_DEPS "${SHADER_GEN}/sprite_vert_spv.h;${SHADER_GEN}/sprite_frag_spv.h;${SHADER_GEN}/msdf_frag_spv.h")
if(DXIL_SHADER_DEPS)
    list(JOIN DXIL_SHADER_DEPS ";" dxil_deps_str)
    string(APPEND ALL_SHADER_DEPS ";${dxil_deps_str}")
endif()
set_source_files_properties(
    renderer/sdl_gpu/SDLGPURenderDevice.cpp PROPERTIES
    OBJECT_DEPENDS "${ALL_SHADER_DEPS}"
)

if(DXC_EXE)
    target_compile_definitions(backend PRIVATE QGAME_HAS_DXIL_SHADERS)
endif()

# PRIVATE — backend 头文件不向上层传播
target_include_directories(backend
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..
    PRIVATE "${SHADER_GEN}"
    PRIVATE "${SDL3_mixer_SOURCE_DIR}/include"
)

# OpenGL 链接（跨平台）
find_package(OpenGL QUIET)
if(OpenGL_FOUND)
    target_link_libraries(backend PRIVATE OpenGL::GL)
elseif(UNIX)
    target_link_libraries(backend PRIVATE GL)
endif()

target_link_libraries(backend
    PRIVATE core
    PRIVATE platform
    PRIVATE SDL3::SDL3
    PRIVATE SDL3_mixer::SDL3_mixer
)
