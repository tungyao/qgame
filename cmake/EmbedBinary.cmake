# 被 add_custom_command 调用，将二进制文件转成 C++ 头文件
# 参数：SPV_FILE  OUTPUT  SYMBOL
cmake_minimum_required(VERSION 3.20)

file(READ "${SPV_FILE}" raw_content HEX)

# 每两个 hex 字符插入 "0x" 前缀和 "," 后缀
string(REGEX REPLACE "([0-9a-fA-F][0-9a-fA-F])" "0x\\1," bytes "${raw_content}")
# 去掉最后多余的逗号
string(REGEX REPLACE ",$" "" bytes "${bytes}")

file(WRITE "${OUTPUT}"
    "#pragma once\n"
    "#include <cstdint>\n"
    "#include <cstddef>\n"
    "// Auto-generated — do not edit\n"
    "inline const uint8_t ${SYMBOL}[] = {${bytes}};\n"
    "inline const size_t  ${SYMBOL}_size = sizeof(${SYMBOL});\n"
)
