#include "FontLoader.h"
#include "../../core/Logger.h"
#include <cstdio>
#include <cstring>
#include <vector>

namespace engine {

namespace {
template<typename T>
bool readPOD(std::FILE* f, T& out) {
    return std::fread(&out, sizeof(T), 1, f) == 1;
}
} // namespace

bool loadFontFile(const std::string& path,
                  FontData& outData,
                  std::vector<uint8_t>& outAtlasRGBA) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        core::logError("[FontLoader] cannot open %s", path.c_str());
        return false;
    }

    char magic[4];
    if (std::fread(magic, 1, 4, f) != 4 || std::memcmp(magic, "FONT", 4) != 0) {
        core::logError("[FontLoader] bad magic in %s", path.c_str());
        std::fclose(f);
        return false;
    }

    uint32_t version = 0, atlasW = 0, atlasH = 0, glyphCount = 0;
    float fontSize = 0.f, pxRange = 0.f, lineHeight = 0.f, baseline = 0.f;

    bool ok =
        readPOD(f, version) && version == 1u
        && readPOD(f, atlasW) && readPOD(f, atlasH)
        && readPOD(f, fontSize) && readPOD(f, pxRange)
        && readPOD(f, lineHeight) && readPOD(f, baseline)
        && readPOD(f, glyphCount);

    if (!ok) {
        core::logError("[FontLoader] malformed header in %s", path.c_str());
        std::fclose(f);
        return false;
    }

    outData.glyphs.clear();
    outData.glyphs.reserve(glyphCount);
    for (uint32_t i = 0; i < glyphCount; ++i) {
        Glyph g{};
        if (!readPOD(f, g.codepoint)
            || !readPOD(f, g.u0) || !readPOD(f, g.v0)
            || !readPOD(f, g.u1) || !readPOD(f, g.v1)
            || !readPOD(f, g.width) || !readPOD(f, g.height)
            || !readPOD(f, g.bearingX) || !readPOD(f, g.bearingY)
            || !readPOD(f, g.advance)) {
            core::logError("[FontLoader] truncated glyph table in %s", path.c_str());
            std::fclose(f);
            return false;
        }
        outData.glyphs.emplace(g.codepoint, g);
    }

    const size_t bytes = static_cast<size_t>(atlasW) * atlasH * 4u;
    outAtlasRGBA.resize(bytes);
    if (std::fread(outAtlasRGBA.data(), 1, bytes, f) != bytes) {
        core::logError("[FontLoader] truncated atlas in %s", path.c_str());
        std::fclose(f);
        return false;
    }
    std::fclose(f);

    outData.atlasWidth  = static_cast<float>(atlasW);
    outData.atlasHeight = static_cast<float>(atlasH);
    outData.fontSize    = fontSize;
    outData.pxRange     = pxRange;
    outData.lineHeight  = lineHeight;
    outData.baseline    = baseline;
    return true;
}

} // namespace engine
