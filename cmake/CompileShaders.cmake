# CompileShaders.cmake
# HLSL → SPIRV for Vulkan / SDL GPU. Requires glslangValidator or dxc.

# ── Find SPIRV compiler ─────────────────────────────────────────────────────
find_program(DXC_EXE NAMES dxc
    HINTS
        "$ENV{VULKAN_SDK}/Bin"
        "$ENV{VULKAN_SDK}/bin"
        "$ENV{VK_SDK_PATH}/Bin"
        "/usr/bin"
        "/usr/local/bin"
    NO_DEFAULT_PATH
)
if(NOT DXC_EXE)
    find_program(DXC_EXE NAMES dxc)
endif()

find_program(GLSLANG_EXE NAMES glslangValidator
    HINTS
        "$ENV{VULKAN_SDK}/Bin"
        "$ENV{VULKAN_SDK}/bin"
        "/usr/bin"
        "/usr/local/bin"
    NO_DEFAULT_PATH
)
if(NOT GLSLANG_EXE)
    find_program(GLSLANG_EXE NAMES glslangValidator)
endif()

if(DXC_EXE)
    set(SHADER_SPIRV_COMPILER "${DXC_EXE}")
    message(STATUS "[Shaders] Using dxc for HLSL → SPIRV: ${DXC_EXE}")
elseif(GLSLANG_EXE)
    set(SHADER_SPIRV_COMPILER "${GLSLANG_EXE}")
    message(STATUS "[Shaders] Using glslangValidator for HLSL → SPIRV: ${GLSLANG_EXE}")
else()
    message(FATAL_ERROR "[Shaders] No SPIRV compiler found. Install glslangValidator (glslang-tools) or dxc.")
endif()

# ── compile_hlsl_to_spirv ───────────────────────────────────────────────────
# 编译 HLSL 为 SPIRV 并内嵌为 C++ 头文件。
# 用法: compile_hlsl_to_spirv(hlsl_file stage out_header symbol)
# stage: vert | frag | comp
function(compile_hlsl_to_spirv hlsl_file stage out_header symbol)
    set(spv_out_dir "${CMAKE_BINARY_DIR}/shaders/spirv")
    set(spv_file    "${spv_out_dir}/${symbol}.spv")
    set(gen_dir     "${CMAKE_BINARY_DIR}/generated/shaders")

    if("${stage}" STREQUAL "vert")
        set(profile "vs_6_0")
        set(glslang_stage "vert")
    elseif("${stage}" STREQUAL "comp")
        set(profile "cs_6_0")
        set(glslang_stage "comp")
    else()
        set(profile "ps_6_0")
        set(glslang_stage "frag")
    endif()

    if(DXC_EXE)
        add_custom_command(
            OUTPUT  "${spv_file}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${spv_out_dir}"
            COMMAND "${DXC_EXE}" -T ${profile} -E main -spirv
                    -fspv-target-env=vulkan1.2
                    -Fo "${spv_file}" "${hlsl_file}"
            DEPENDS "${hlsl_file}"
            COMMENT "dxc -spirv: ${hlsl_file} → ${symbol}.spv"
        )
    else()
        add_custom_command(
            OUTPUT  "${spv_file}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${spv_out_dir}"
            COMMAND "${GLSLANG_EXE}" -D -V -S ${glslang_stage}
                    --entry-point main
                    "${hlsl_file}" -o "${spv_file}"
            DEPENDS "${hlsl_file}"
            COMMENT "glslangValidator: ${hlsl_file} → ${symbol}.spv"
        )
    endif()

    add_custom_command(
        OUTPUT  "${out_header}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${gen_dir}"
        COMMAND "${CMAKE_COMMAND}"
                -DSPV_FILE="${spv_file}"
                -DOUTPUT="${out_header}"
                -DSYMBOL="${symbol}"
                -P "${CMAKE_SOURCE_DIR}/cmake/EmbedBinary.cmake"
        DEPENDS "${spv_file}"
        COMMENT "Embedding ${symbol}.spv → ${symbol}_spv.h"
    )
endfunction()


