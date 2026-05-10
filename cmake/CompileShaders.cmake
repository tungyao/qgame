# CompileShaders.cmake
# HLSL → SPIR-V, only glslangValidator

find_program(GLSLANG_EXE NAMES glslangValidator
    HINTS
        "$ENV{VULKAN_SDK}/Bin"
        "$ENV{VULKAN_SDK}/bin"
        "/usr/bin"
        "/usr/local/bin"
)

if(NOT GLSLANG_EXE)
    message(FATAL_ERROR "[Shaders] glslangValidator not found. Install Vulkan SDK or glslang-tools.")
endif()

message(STATUS "[Shaders] Using glslangValidator: ${GLSLANG_EXE}")

function(compile_hlsl_to_spirv hlsl_file stage out_header symbol)
    set(spv_out_dir "${CMAKE_BINARY_DIR}/shaders/spirv")
    set(spv_file    "${spv_out_dir}/${symbol}.spv")
    set(gen_dir     "${CMAKE_BINARY_DIR}/generated/shaders")

    if("${stage}" STREQUAL "vert")
        set(glslang_stage "vert")
    elseif("${stage}" STREQUAL "frag")
        set(glslang_stage "frag")
    elseif("${stage}" STREQUAL "comp")
        set(glslang_stage "comp")
    else()
        message(FATAL_ERROR "Unknown shader stage: ${stage}")
    endif()

    add_custom_command(
        OUTPUT "${spv_file}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${spv_out_dir}"
        COMMAND "${GLSLANG_EXE}"
                -D
                -V
                --target-env vulkan1.1
                -S ${glslang_stage}
                -e main
                "${hlsl_file}"
                -o "${spv_file}"
        DEPENDS "${hlsl_file}"
        COMMENT "glslangValidator HLSL → SPIR-V: ${hlsl_file} → ${symbol}.spv"
        VERBATIM
    )

    add_custom_command(
        OUTPUT "${out_header}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${gen_dir}"
        COMMAND "${CMAKE_COMMAND}"
                -DSPV_FILE="${spv_file}"
                -DOUTPUT="${out_header}"
                -DSYMBOL="${symbol}"
                -P "${CMAKE_SOURCE_DIR}/cmake/EmbedBinary.cmake"
        DEPENDS "${spv_file}"
        COMMENT "Embedding ${symbol}.spv → ${symbol}_spv.h"
        VERBATIM
    )
endfunction()