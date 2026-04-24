#pragma once
#include <string>
#include "../../backend/shared/ResourceHandle.h"
#include "../../core/math/Color.h"

namespace engine {

struct TextComponent {
    std::string text;
    TextureHandle fontTexture;  // 字体图集
    float        fontSize = 16.f;
    core::Color  color = core::Color::White;
    int          layer = 10;
    bool         visible = true;
};

} // namespace engine
