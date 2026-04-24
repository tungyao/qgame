#include "GameAPI.h"
#include "GameAPI_ExternTemplates.h"
#include "../components/RenderComponents.h"
#include "../components/PhysicsComponents.h"
#include "../components/AnimatorComponent.h"
#include "../components/TextComponent.h"

namespace engine {

// 显式实例化：addComponent
template EntityID&    GameAPI::addComponent<EntityID>(entt::entity, EntityID);
template Transform&   GameAPI::addComponent<Transform>(entt::entity, Transform);
template Camera&      GameAPI::addComponent<Camera>(entt::entity, Camera);
template Sprite&      GameAPI::addComponent<Sprite>(entt::entity, Sprite);
template TileMap&     GameAPI::addComponent<TileMap>(entt::entity, TileMap);
template Name&        GameAPI::addComponent<Name>(entt::entity, Name);
template TextComponent& GameAPI::addComponent<TextComponent>(entt::entity, TextComponent);

template RigidBody&   GameAPI::addComponent<RigidBody>(entt::entity, RigidBody);
template Collider&    GameAPI::addComponent<Collider>(entt::entity, Collider);

template AnimatorComponent& GameAPI::addComponent<AnimatorComponent>(entt::entity, AnimatorComponent);

// 显式实例化：getComponent
template EntityID&    GameAPI::getComponent<EntityID>(entt::entity);
template Transform&   GameAPI::getComponent<Transform>(entt::entity);
template Camera&      GameAPI::getComponent<Camera>(entt::entity);
template Sprite&      GameAPI::getComponent<Sprite>(entt::entity);
template TileMap&     GameAPI::getComponent<TileMap>(entt::entity);
template Name&        GameAPI::getComponent<Name>(entt::entity);
template TextComponent& GameAPI::getComponent<TextComponent>(entt::entity);

template RigidBody&   GameAPI::getComponent<RigidBody>(entt::entity);
template Collider&    GameAPI::getComponent<Collider>(entt::entity);

template AnimatorComponent& GameAPI::getComponent<AnimatorComponent>(entt::entity);

// 显式实例化：hasComponent
template bool GameAPI::hasComponent<EntityID>(entt::entity) const;
template bool GameAPI::hasComponent<Transform>(entt::entity) const;
template bool GameAPI::hasComponent<Camera>(entt::entity) const;
template bool GameAPI::hasComponent<Sprite>(entt::entity) const;
template bool GameAPI::hasComponent<TileMap>(entt::entity) const;
template bool GameAPI::hasComponent<Name>(entt::entity) const;
template bool GameAPI::hasComponent<TextComponent>(entt::entity) const;

template bool GameAPI::hasComponent<RigidBody>(entt::entity) const;
template bool GameAPI::hasComponent<Collider>(entt::entity) const;

template bool GameAPI::hasComponent<AnimatorComponent>(entt::entity) const;

} // namespace engine
