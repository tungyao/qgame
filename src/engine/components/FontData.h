#pragma once
#include <unordered_map>
#include <cstdint>
#include "../../backend/shared/ResourceHandle.h"

namespace engine {

struct Glyph {
    uint32_t codepoint = 0;
    float u0 = 0.f, v0 = 0.f, u1 = 0.f, v1 = 0.f;
    float width = 0.f;
    float height = 0.f;
    float bearingX = 0.f;
    // bearingY: 基线到字形顶端的距离（向上为正），FreeType / stbtt 的标准约定。
    float bearingY = 0.f;
    float advance = 0.f;
};

using FontHandle = core::Handle<struct FontTag>;

struct FontData {
    TextureHandle texture;
    std::unordered_map<uint32_t, Glyph> glyphs;
    float lineHeight = 1.2f;
    float baseline = 0.f;
    float fontSize = 32.f;
    float atlasWidth = 0.f;
    float atlasHeight = 0.f;
    // MSDF 生成时的 distance range（像素）。着色器里按 atlas→屏幕缩放后得到 screenPxRange。
    float pxRange = 4.f;
    
    const Glyph* getGlyph(uint32_t codepoint) const {
        auto it = glyphs.find(codepoint);
        return (it != glyphs.end()) ? &it->second : nullptr;
    }
    
    bool hasGlyph(uint32_t codepoint) const {
        return glyphs.find(codepoint) != glyphs.end();
    }
};

}
