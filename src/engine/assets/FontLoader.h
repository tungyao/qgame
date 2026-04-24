#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include "../components/FontData.h"

// .font 二进制格式（小端，version = 1）
//
//   magic       char[4] = "FONT"
//   version     u32     = 1
//   atlasWidth  u32     （像素）
//   atlasHeight u32
//   fontSize    f32     （atlas 烘焙时的像素尺寸，= glyph 度量基准）
//   pxRange     f32     （MSDF distance range，像素）
//   lineHeight  f32     （像素，基于 fontSize）
//   baseline    f32     （ascender，像素；正值）
//   glyphCount  u32
//   glyphs[glyphCount] {
//       codepoint                 u32
//       u0, v0, u1, v1            f32 (normalized [0,1]，左上为原点)
//       width, height             f32 (像素)
//       bearingX, bearingY        f32 (像素；bearingY = baseline 到字形顶端，向上为正)
//       advance                   f32 (像素)
//   }
//   atlasRGBA   u8[atlasWidth * atlasHeight * 4]   (R8G8B8A8，行主序，左上为原点)
//
// 未来的编译阶段负责产出；当前运行时只负责反序列化。

namespace engine {

// 反序列化一个 .font 文件到 outData 与 outAtlasRGBA。outData.texture 需调用方另外填充。
// 成功返回 true；失败打日志并返回 false，输出参数未定义。
bool loadFontFile(const std::string& path,
                  FontData& outData,
                  std::vector<uint8_t>& outAtlasRGBA);

} // namespace engine
