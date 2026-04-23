# compile_shader_spirv(glsl_file stage out_header symbol fallback_header)
#
# 有 glslc/glslangValidator 时：编译 GLSL → SPIRV → 生成嵌入头文件
# 没有时：直接使用 fallback_header（预编译好的版本，存在仓库中）

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
    message(STATUS "[Shaders] No shader compiler found — using pre-compiled SPIRV fallback")
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
