include(${CMAKE_SOURCE_DIR}/cmake/CompileShaders.cmake)

set(SHADER_SRC      "${CMAKE_SOURCE_DIR}/assets/shaders")
set(SHADER_GEN      "${CMAKE_BINARY_DIR}/generated/shaders")
set(SHADER_FALLBACK "${CMAKE_CURRENT_SOURCE_DIR}/renderer/sdl_gpu/shaders_precompiled")

# ── Sprite 基础着色器 ────────────────────────────────────────────────────────
compile_hlsl_to_spirv(
    "${SHADER_SRC}/sprite.vert.hlsl"  vert
    "${SHADER_GEN}/sprite_vert_spv.h" sprite_vert_spv
    "${SHADER_FALLBACK}/sprite_vert_spv.h"
)
compile_hlsl_to_spirv(
    "${SHADER_SRC}/sprite.frag.hlsl"  frag
    "${SHADER_GEN}/sprite_frag_spv.h" sprite_frag_spv
    "${SHADER_FALLBACK}/sprite_frag_spv.h"
)

# ── MSDF 字体着色器 ──────────────────────────────────────────────────────────
compile_hlsl_to_spirv(
    "${SHADER_SRC}/msdf.frag.hlsl"  frag
    "${SHADER_GEN}/msdf_frag_spv.h" msdf_frag_spv
    "${SHADER_FALLBACK}/msdf_frag_spv.h"
)

# ── GPU 驱动渲染着色器 ──────────────────────────────────────────────────────
compile_hlsl_to_spirv(
    "${SHADER_SRC}/sprite_gpu.vert.hlsl"  vert
    "${SHADER_GEN}/sprite_gpu_vert_spv.h" sprite_gpu_vert_spv
    "${SHADER_FALLBACK}/sprite_gpu_vert_spv.h"
)
compile_hlsl_to_spirv(
    "${SHADER_SRC}/sprite_gpu.frag.hlsl"  frag
    "${SHADER_GEN}/sprite_gpu_frag_spv.h" sprite_gpu_frag_spv
    "${SHADER_FALLBACK}/sprite_gpu_frag_spv.h"
)
compile_hlsl_to_spirv(
    "${SHADER_SRC}/particle_gpu.vert.hlsl"  vert
    "${SHADER_GEN}/particle_gpu_vert_spv.h" particle_gpu_vert_spv
    "${SHADER_FALLBACK}/particle_gpu_vert_spv.h"
)
compile_hlsl_to_spirv(
    "${SHADER_SRC}/particle_gpu.frag.hlsl"  frag
    "${SHADER_GEN}/particle_gpu_frag_spv.h" particle_gpu_frag_spv
    "${SHADER_FALLBACK}/particle_gpu_frag_spv.h"
)

# ── Compute 着色器 (Culling + Sort) ─────────────────────────────────────────
compile_hlsl_to_spirv(
    "${SHADER_SRC}/sprite_culling.comp.hlsl"  comp
    "${SHADER_GEN}/sprite_culling_spv.h" sprite_culling_spv
    "${SHADER_FALLBACK}/sprite_culling_spv.h"
)
compile_hlsl_to_spirv(
    "${SHADER_SRC}/sprite_sort.comp.hlsl"  comp
    "${SHADER_GEN}/sprite_sort_spv.h" sprite_sort_spv
    "${SHADER_FALLBACK}/sprite_sort_spv.h"
)
compile_hlsl_to_spirv(
    "${SHADER_SRC}/particle_update.comp.hlsl"  comp
    "${SHADER_GEN}/particle_update_spv.h" particle_update_spv
    "${SHADER_FALLBACK}/particle_update_spv.h"
)
compile_hlsl_to_spirv(
    "${SHADER_SRC}/particle_sort.comp.hlsl"  comp
    "${SHADER_GEN}/particle_sort_spv.h" particle_sort_spv
    "${SHADER_FALLBACK}/particle_sort_spv.h"
)
compile_hlsl_to_spirv(
    "${SHADER_SRC}/lighting2d.comp.hlsl"  comp
    "${SHADER_GEN}/lighting2d_spv.h" lighting2d_spv
    "${SHADER_FALLBACK}/lighting2d_spv.h"
)
compile_hlsl_to_spirv(
    "${SHADER_SRC}/lighting2d_cull.comp.hlsl"  comp
    "${SHADER_GEN}/lighting2d_cull_spv.h" lighting2d_cull_spv
    "${SHADER_FALLBACK}/lighting2d_cull_spv.h"
)
compile_hlsl_to_spirv(
    "${SHADER_SRC}/lighting2d_blur.comp.hlsl"  comp
    "${SHADER_GEN}/lighting2d_blur_spv.h" lighting2d_blur_spv
    "${SHADER_FALLBACK}/lighting2d_blur_spv.h"
)
compile_hlsl_to_spirv(
    "${SHADER_SRC}/lighting2d_composite.frag.hlsl"  frag
    "${SHADER_GEN}/lighting2d_composite_frag_spv.h" lighting2d_composite_frag_spv
    "${SHADER_FALLBACK}/sprite_frag_spv.h"
)

# ── DXIL 着色器 (D3D12) ──────────────────────────────────────────────────────
set(DXIL_SHADER_DEPS "")
if(DXC_EXE)
    compile_hlsl_to_dxil(
        "${SHADER_SRC}/sprite.vert.hlsl"   vert
        "${SHADER_GEN}/sprite_vert_dxil.h" sprite_vert_dxil
        "${SHADER_FALLBACK}/sprite_vert_dxil.h"
    )
    compile_hlsl_to_dxil(
        "${SHADER_SRC}/sprite.frag.hlsl"   frag
        "${SHADER_GEN}/sprite_frag_dxil.h" sprite_frag_dxil
        "${SHADER_FALLBACK}/sprite_frag_dxil.h"
    )
    compile_hlsl_to_dxil(
        "${SHADER_SRC}/particle_gpu.vert.hlsl"   vert
        "${SHADER_GEN}/particle_gpu_vert_dxil.h" particle_gpu_vert_dxil
        "${SHADER_FALLBACK}/particle_gpu_vert_dxil.h"
    )
    compile_hlsl_to_dxil(
        "${SHADER_SRC}/particle_gpu.frag.hlsl"   frag
        "${SHADER_GEN}/particle_gpu_frag_dxil.h" particle_gpu_frag_dxil
        "${SHADER_FALLBACK}/particle_gpu_frag_dxil.h"
    )
    compile_hlsl_to_dxil(
        "${SHADER_SRC}/msdf.frag.hlsl"   frag
        "${SHADER_GEN}/msdf_frag_dxil.h" msdf_frag_dxil
        "${SHADER_FALLBACK}/msdf_frag_dxil.h"
    )
    compile_hlsl_to_dxil(
        "${SHADER_SRC}/sprite_culling.comp.hlsl"   comp
        "${SHADER_GEN}/sprite_culling_dxil.h" sprite_culling_dxil
        "${SHADER_FALLBACK}/sprite_culling_dxil.h"
    )
    compile_hlsl_to_dxil(
        "${SHADER_SRC}/sprite_sort.comp.hlsl"   comp
        "${SHADER_GEN}/sprite_sort_dxil.h" sprite_sort_dxil
        "${SHADER_FALLBACK}/sprite_sort_dxil.h"
    )
    compile_hlsl_to_dxil(
        "${SHADER_SRC}/particle_update.comp.hlsl"   comp
        "${SHADER_GEN}/particle_update_dxil.h" particle_update_dxil
        "${SHADER_FALLBACK}/particle_update_dxil.h"
    )
    compile_hlsl_to_dxil(
        "${SHADER_SRC}/particle_sort.comp.hlsl"   comp
        "${SHADER_GEN}/particle_sort_dxil.h" particle_sort_dxil
        "${SHADER_FALLBACK}/particle_sort_dxil.h"
    )
    compile_hlsl_to_dxil(
        "${SHADER_SRC}/lighting2d.comp.hlsl"   comp
        "${SHADER_GEN}/lighting2d_dxil.h" lighting2d_dxil
        "${SHADER_FALLBACK}/lighting2d_dxil.h"
    )
    compile_hlsl_to_dxil(
        "${SHADER_SRC}/lighting2d_cull.comp.hlsl"   comp
        "${SHADER_GEN}/lighting2d_cull_dxil.h" lighting2d_cull_dxil
        "${SHADER_FALLBACK}/lighting2d_cull_dxil.h"
    )
    compile_hlsl_to_dxil(
        "${SHADER_SRC}/lighting2d_blur.comp.hlsl"   comp
        "${SHADER_GEN}/lighting2d_blur_dxil.h" lighting2d_blur_dxil
        "${SHADER_FALLBACK}/lighting2d_blur_dxil.h"
    )
    compile_hlsl_to_dxil(
        "${SHADER_SRC}/lighting2d_composite.frag.hlsl"   frag
        "${SHADER_GEN}/lighting2d_composite_frag_dxil.h" lighting2d_composite_frag_dxil
        "${SHADER_FALLBACK}/sprite_frag_dxil.h"
    )
    list(APPEND DXIL_SHADER_DEPS
        "${SHADER_GEN}/sprite_vert_dxil.h"
        "${SHADER_GEN}/sprite_frag_dxil.h"
        "${SHADER_GEN}/particle_gpu_vert_dxil.h"
        "${SHADER_GEN}/particle_gpu_frag_dxil.h"
        "${SHADER_GEN}/msdf_frag_dxil.h"
        "${SHADER_GEN}/sprite_culling_dxil.h"
        "${SHADER_GEN}/sprite_sort_dxil.h"
        "${SHADER_GEN}/particle_update_dxil.h"
        "${SHADER_GEN}/particle_sort_dxil.h"
        "${SHADER_GEN}/lighting2d_dxil.h"
        "${SHADER_GEN}/lighting2d_cull_dxil.h"
        "${SHADER_GEN}/lighting2d_blur_dxil.h"
        "${SHADER_GEN}/lighting2d_composite_frag_dxil.h"
    )
endif()

add_library(backend STATIC
    renderer/CommandBuffer.cpp
    renderer/RenderGraph.cpp
    renderer/sdl_gpu/SDLGPURenderDevice.cpp
    renderer/opengl/GLRenderDevice.cpp
    audio/sdl_mixer/SDLMixerAudioDevice.cpp
    
    # GPU-Driven 2D Rendering (架构设计文档，参考用)
    # renderer/gpu_driven/GPUDrivenRenderer2D.cpp
)

set(ALL_SHADER_DEPS 
    "${SHADER_GEN}/sprite_vert_spv.h"
    "${SHADER_GEN}/sprite_frag_spv.h"
    "${SHADER_GEN}/msdf_frag_spv.h"
    "${SHADER_GEN}/sprite_culling_spv.h"
    "${SHADER_GEN}/sprite_sort_spv.h"
    "${SHADER_GEN}/sprite_gpu_vert_spv.h"
    "${SHADER_GEN}/sprite_gpu_frag_spv.h"
    "${SHADER_GEN}/particle_gpu_vert_spv.h"
    "${SHADER_GEN}/particle_gpu_frag_spv.h"
    "${SHADER_GEN}/particle_update_spv.h"
    "${SHADER_GEN}/particle_sort_spv.h"
    "${SHADER_GEN}/lighting2d_spv.h"
    "${SHADER_GEN}/lighting2d_cull_spv.h"
    "${SHADER_GEN}/lighting2d_blur_spv.h"
    "${SHADER_GEN}/lighting2d_composite_frag_spv.h"
)
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

target_include_directories(backend
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..
    PRIVATE "${SHADER_GEN}"
    PRIVATE "${SDL3_mixer_SOURCE_DIR}/include"
)

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
