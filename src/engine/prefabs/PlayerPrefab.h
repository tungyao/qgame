#pragma once

#include <entt/entt.hpp>

#include "../Export.h"
#include "../components/FontData.h"
#include "../../backend/shared/ResourceHandle.h"
#include "../../core/math/Color.h"

namespace engine {

class GameAPI;

namespace prefabs {

// PlayerPrefabDesc is intentionally small: it only exposes values the current
// game demo already needs. More gameplay components can be added here later,
// but the first prefab should stay close to "create an entity with the common
// visible player components".
struct PlayerPrefabDesc {
    float x = 0.f;
    float y = 0.f;
    float scaleX = 1.f;
    float scaleY = 1.f;

    TextureHandle texture;
    float srcX = 0.f;
    float srcY = 0.f;
    float srcW = 32.f;
    float srcH = 32.f;

    FontHandle font;
    const char* label = "Player";
    float labelFontSize = 16.f;

    int spriteLayer = 0;
    int textLayer = 10;
    core::Color spriteTint = core::Color::White;
    core::Color textColor = core::Color::White;
};

// Code-factory prefab for the most common player shape:
// one entity with Transform + Sprite + TextComponent.
//
// This deliberately does not become a data-driven editor feature. The goal is
// to make game code concise while keeping all construction logic in ordinary
// C++ where a solo developer can debug and refactor it cheaply.
QGAME_ENGINE_API entt::entity createPlayer(GameAPI& api, const PlayerPrefabDesc& desc = {});

} // namespace prefabs
} // namespace engine
