# compile_shader_spirv(glsl_file stage out_header symbol fallback_header)
# compile_shader_dxil(hlsl_file stage out_header symbol fallback_header)
#
# SPIRV: 有 glslc/glslangValidator 时编译 GLSL → SPIRV，否则用 fallback
# DXIL:  有 dxc 时编译 HLSL → DXIL，否则跳过（D3D12 支持禁用）

find_program(GLSLC_EXE NAMES glslc
    HINTS
        "$ENV{VULKAN_SDK}/Bin"     # Windows Vulkan SDK (大写 Bin)
        "$ENV{VULKAN_SDK}/bin"     # Linux/macOS
        "$ENV{VK_SDK_PATH}/Bin"    # 备用环境变量
        "C:/VulkanSDK/1.3.296.0/Bin"  # 常见 Windows 默认安装路径
        "C:/VulkanSDK/1.3.283.0/Bin"
        "C:/VulkanSDK/1.3.261.0/Bin"
        "/usr/bin"
    NO_DEFAULT_PATH
)
if(NOT GLSLC_EXE)
    find_program(GLSLC_EXE NAMES glslc)   # 再尝试 PATH
endif()

find_program(GLSLANG_EXE NAMES glslangValidator
    HINTS
        "$ENV{VULKAN_SDK}/Bin"
        "$ENV{VULKAN_SDK}/bin"
        "/usr/bin"
)

if(GLSLC_EXE)
    message(STATUS "[Shaders] glslc found: ${GLSLC_EXE}")
elseif(GLSLANG_EXE)
    message(STATUS "[Shaders] glslangValidator found: ${GLSLANG_EXE}")
else()
    message(STATUS "[Shaders] No SPIRV compiler found — using pre-compiled SPIRV fallback")
endif()

# dxc: HLSL → DXIL (D3D12). Included in Vulkan SDK alongside glslc.
find_program(DXC_EXE NAMES dxc
    HINTS
        "$ENV{VULKAN_SDK}/Bin"
        "$ENV{VULKAN_SDK}/bin"
        "$ENV{VK_SDK_PATH}/Bin"
        "C:/VulkanSDK/1.3.296.0/Bin"
        "C:/VulkanSDK/1.3.283.0/Bin"
        "C:/VulkanSDK/1.3.261.0/Bin"
        "C:/Program Files (x86)/Windows Kits/10/bin/10.0.22621.0/x64"
        "C:/Program Files (x86)/Windows Kits/10/bin/10.0.19041.0/x64"
    NO_DEFAULT_PATH
)
if(NOT DXC_EXE)
    find_program(DXC_EXE NAMES dxc)
endif()

if(DXC_EXE)
    message(STATUS "[Shaders] dxc found: ${DXC_EXE} — D3D12/DXIL support enabled")
else()
    message(STATUS "[Shaders] dxc not found — D3D12/DXIL disabled; Vulkan/SPIRV will be used on Windows")
endif()

function(compile_shader_spirv glsl_file stage out_header symbol fallback_header)
    set(spv_out_dir "${CMAKE_BINARY_DIR}/shaders")
    set(spv_file    "${spv_out_dir}/${symbol}.spv")
    set(gen_dir     "${CMAKE_BINARY_DIR}/generated/shaders")

    if(GLSLC_EXE OR GLSLANG_EXE)
        # ── 编译 GLSL → SPIRV ──────────────────────────────────────────────
        if(GLSLC_EXE)
            add_custom_command(
                OUTPUT  "${spv_file}"
                COMMAND "${CMAKE_COMMAND}" -E make_directory "${spv_out_dir}"
                COMMAND "${GLSLC_EXE}" -fshader-stage=${stage}
                        "${glsl_file}" -o "${spv_file}"
                DEPENDS "${glsl_file}"
                COMMENT "glslc: ${glsl_file} → ${symbol}.spv"
            )
        else()
            add_custom_command(
                OUTPUT  "${spv_file}"
                COMMAND "${CMAKE_COMMAND}" -E make_directory "${spv_out_dir}"
                COMMAND "${GLSLANG_EXE}" -S ${stage} -V
                        "${glsl_file}" -o "${spv_file}"
                DEPENDS "${glsl_file}"
                COMMENT "glslangValidator: ${glsl_file} → ${symbol}.spv"
            )
        endif()

        # ── SPIRV → C++ 头文件 ─────────────────────────────────────────────
        add_custom_command(
            OUTPUT  "${out_header}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${gen_dir}"
            COMMAND "${CMAKE_COMMAND}"
                    -DSPV_FILE="${spv_file}"
                    -DOUTPUT="${out_header}"
                    -DSYMBOL="${symbol}"
                    -P "${CMAKE_SOURCE_DIR}/cmake/EmbedBinary.cmake"
            DEPENDS "${spv_file}"
            COMMENT "Embedding ${symbol}.spv → ${symbol}.h"
        )
    else()
        # ── 无编译器：复制 fallback 到 generated 目录 ──────────────────────
        add_custom_command(
            OUTPUT  "${out_header}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${gen_dir}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${fallback_header}" "${out_header}"
            DEPENDS "${fallback_header}"
            COMMENT "Using pre-compiled fallback for ${symbol}"
        )
    endif()
endfunction()

# compile_shader_dxil(hlsl_file stage out_header symbol fallback_header)
#   stage: "vs" | "ps"
# Only generates output when DXC_EXE is found; caller checks DXC_EXE before calling.
function(compile_shader_dxil hlsl_file stage out_header symbol fallback_header)
    set(dxil_out_dir "${CMAKE_BINARY_DIR}/shaders")
    set(dxil_file    "${dxil_out_dir}/${symbol}.dxil")
    set(gen_dir      "${CMAKE_BINARY_DIR}/generated/shaders")

    if("${stage}" STREQUAL "vs")
        set(profile "vs_6_0")
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
        COMMENT "Embedding ${symbol}.dxil → ${symbol}.h"
    )
endfunction()
