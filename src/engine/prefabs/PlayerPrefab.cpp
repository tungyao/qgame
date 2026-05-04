#include "PlayerPrefab.h"

#include "../api/GameAPI.h"
#include "../components/RenderComponents.h"
#include "../components/TextComponent.h"

namespace engine::prefabs {

entt::entity createPlayer(GameAPI& api, const PlayerPrefabDesc& desc) {
    // Keep the factory boring on purpose: create one entity, attach the three
    // components the player demo expects, and copy the caller-provided values
    // into those components. No hidden child entities or asset loading happen
    // here, so the call site remains easy to reason about.
    const entt::entity player = api.spawnEntity();

    engine::Transform transform{};
    transform.x = desc.x;
    transform.y = desc.y;
    transform.scaleX = desc.scaleX;
    transform.scaleY = desc.scaleY;
    api.addComponent(player, transform);

    engine::Sprite sprite{};
    sprite.texture = desc.texture;
    sprite.srcRect = { desc.srcX, desc.srcY, desc.srcW, desc.srcH };
    sprite.layer = desc.spriteLayer;
    sprite.pass = engine::RenderPass::World;
    sprite.tint = desc.spriteTint;
    sprite.ySort = true;
    api.addComponent(player, sprite);

    engine::TextComponent text{};
    text.text = desc.label ? desc.label : "";
    text.font = desc.font;
    text.fontSize = desc.labelFontSize;
    text.color = desc.textColor;
    text.layer = desc.textLayer;
    text.pass = engine::RenderPass::World;
    api.addComponent(player, text);

    return player;
}

} // namespace engine::prefabs
