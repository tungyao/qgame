cmake_minimum_required(VERSION 3.20)
project(hello_compute)

include(${CMAKE_SOURCE_DIR}/cmake/CompileShaders.cmake)

set(SHADER_SRC "${CMAKE_CURRENT_SOURCE_DIR}")
set(SHADER_GEN "${CMAKE_BINARY_DIR}/generated/tests/hello_compute")
set(SHADER_FALLBACK "${CMAKE_CURRENT_SOURCE_DIR}/shaders_precompiled")

# Compile SPIRV compute shader
compile_shader_spirv(
    "${SHADER_SRC}/hello_compute.comp"  comp
    "${SHADER_GEN}/hello_compute_spv.h" hello_compute_spv
    "${SHADER_FALLBACK}/hello_compute_spv.h"
)

# Compile DXIL compute shader (if dxc available)
set(COMPUTE_SHADER_DEPS "${SHADER_GEN}/hello_compute_spv.h")
if(DXC_EXE)
    compile_shader_dxil(
        "${SHADER_SRC}/hello_compute.hlsl"   cs
        "${SHADER_GEN}/hello_compute_dxil.h" hello_compute_dxil
        "${SHADER_FALLBACK}/hello_compute_dxil.h"
    )
    list(APPEND COMPUTE_SHADER_DEPS "${SHADER_GEN}/hello_compute_dxil.h")
endif()

add_executable(hello_compute main.cpp)

target_link_libraries(hello_compute PRIVATE SDL3::SDL3 backend engine)
target_include_directories(hello_compute PRIVATE 
    ${CMAKE_SOURCE_DIR}/src
    ${SHADER_GEN}
)

set_source_files_properties(
    main.cpp PROPERTIES
    OBJECT_DEPENDS "${COMPUTE_SHADER_DEPS}"
)

if(DXC_EXE)
    target_compile_definitions(hello_compute PRIVATE QGAME_HAS_DXIL_SHADERS)
endif()
