#pragma once
#include <string>
#include "FontData.h"
#include "RenderComponents.h"
#include "../../core/math/Color.h"

namespace engine {

struct TextComponent {
    std::string text;
    FontHandle  font;
    float       fontSize = 16.f;
    core::Color color = core::Color::White;
    int         layer = 10;
    int         sortOrder = 0;
    bool        ySort = false;
    RenderPass  pass = RenderPass::UI;
    bool        visible = true;
};

} // namespace engine
