# CompileShaders.cmake
# 统一使用 HLSL 作为源文件，编译为 SPIRV 和 DXIL
#
# HLSL (唯一源文件)
#     │
#     ├──► DXC -spirv ──► SPIRV ──► Vulkan/SDL GPU
#     │    或 glslangValidator -D -V
#     │
#     └──► DXC ──► DXIL ──► D3D12

# ── 查找 DXC (DirectX Shader Compiler) ──────────────────────────────────────
find_program(DXC_EXE NAMES dxc
    HINTS
        "$ENV{VULKAN_SDK}/Bin"
        "$ENV{VULKAN_SDK}/bin"
        "$ENV{VK_SDK_PATH}/Bin"
        "C:/VulkanSDK/1.4.341.1/Bin"
        "C:/VulkanSDK/1.3.296.0/Bin"
        "C:/VulkanSDK/1.3.283.0/Bin"
        "C:/VulkanSDK/1.3.261.0/Bin"
        "C:/Program Files (x86)/Windows Kits/10/bin/10.0.22621.0/x64"
        "C:/Program Files (x86)/Windows Kits/10/bin/10.0.19041.0/x64"
        "/usr/bin"
        "/usr/local/bin"
    NO_DEFAULT_PATH
)
if(NOT DXC_EXE)
    find_program(DXC_EXE NAMES dxc)
endif()

# ── 查找 glslangValidator (支持 HLSL → SPIRV) ───────────────────────────────
find_program(GLSLANG_EXE NAMES glslangValidator
    HINTS
        "$ENV{VULKAN_SDK}/Bin"
        "$ENV{VULKAN_SDK}/bin"
        "C:/VulkanSDK/1.4.341.1/Bin"
        "C:/VulkanSDK/1.3.296.0/Bin"
        "/usr/bin"
        "/usr/local/bin"
    NO_DEFAULT_PATH
)
if(NOT GLSLANG_EXE)
    find_program(GLSLANG_EXE NAMES glslangValidator)
endif()

# 优先使用 DXC，其次 glslangValidator
if(DXC_EXE)
    set(SHADER_SPIRV_COMPILER "${DXC_EXE}")
    set(SHADER_SPIRV_FLAGS "-spirv;-fspv-target-env=vulkan1.2")
    message(STATUS "[Shaders] Using dxc for HLSL → SPIRV: ${DXC_EXE}")
elseif(GLSLANG_EXE)
    set(SHADER_SPIRV_COMPILER "${GLSLANG_EXE}")
    set(SHADER_SPIRV_FLAGS "-D;-V")  # -D: HLSL input, -V: SPIRV output
    message(STATUS "[Shaders] Using glslangValidator for HLSL → SPIRV: ${GLSLANG_EXE}")
else()
    message(WARNING "[Shaders] No SPIRV compiler found (dxc/glslangValidator) — using fallback")
endif()

if(DXC_EXE)
    message(STATUS "[Shaders] DXC found — DXIL support enabled: ${DXC_EXE}")
else()
    message(STATUS "[Shaders] DXC not found — DXIL disabled")
endif()

# ── compile_hlsl_to_spirv: HLSL → SPIRV ─────────────────────────────────────
# 参数: hlsl_file stage out_header symbol fallback_header
# stage: vert | frag | comp
function(compile_hlsl_to_spirv hlsl_file stage out_header symbol fallback_header)
    set(spv_out_dir "${CMAKE_BINARY_DIR}/shaders/spirv")
    set(spv_file    "${spv_out_dir}/${symbol}.spv")
    set(gen_dir     "${CMAKE_BINARY_DIR}/generated/shaders")

    if(SHADER_SPIRV_COMPILER)
        # 确定 stage profile
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
            # 使用 DXC
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
            # 使用 glslangValidator
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

        # SPIRV → C++ 头文件
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
    else()
        # 无编译器：使用 fallback
        add_custom_command(
            OUTPUT  "${out_header}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${gen_dir}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${fallback_header}" "${out_header}"
            DEPENDS "${fallback_header}"
            COMMENT "Using pre-compiled fallback for ${symbol}_spv"
        )
    endif()
endfunction()

# ── compile_hlsl_to_dxil: HLSL → DXIL ───────────────────────────────────────
# 参数: hlsl_file stage out_header symbol fallback_header
# stage: vert | frag | comp
function(compile_hlsl_to_dxil hlsl_file stage out_header symbol fallback_header)
    set(dxil_out_dir "${CMAKE_BINARY_DIR}/shaders/dxil")
    set(dxil_file    "${dxil_out_dir}/${symbol}.dxil")
    set(gen_dir      "${CMAKE_BINARY_DIR}/generated/shaders")

    if(DXC_EXE)
        if("${stage}" STREQUAL "vert")
            set(profile "vs_6_0")
        elseif("${stage}" STREQUAL "comp")
            set(profile "cs_6_0")
        else()
            set(profile "ps_6_0")
        endif()

        add_custom_command(
            OUTPUT  "${dxil_file}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${dxil_out_dir}"
            COMMAND "${DXC_EXE}" -T ${profile} -E main -Fo "${dxil_file}" "${hlsl_file}"
            DEPENDS "${hlsl_file}"
            COMMENT "dxc: ${hlsl_file} → ${symbol}.dxil"
        )

        add_custom_command(
            OUTPUT  "${out_header}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${gen_dir}"
            COMMAND "${CMAKE_COMMAND}"
                    -DSPV_FILE="${dxil_file}"
                    -DOUTPUT="${out_header}"
                    -DSYMBOL="${symbol}"
                    -P "${CMAKE_SOURCE_DIR}/cmake/EmbedBinary.cmake"
            DEPENDS "${dxil_file}"
            COMMENT "Embedding ${symbol}.dxil → ${symbol}_dxil.h"
        )
    else()
        add_custom_command(
            OUTPUT  "${out_header}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${gen_dir}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${fallback_header}" "${out_header}"
            DEPENDS "${fallback_header}"
            COMMENT "Using pre-compiled fallback for ${symbol}_dxil"
        )
    endif()
endfunction()
