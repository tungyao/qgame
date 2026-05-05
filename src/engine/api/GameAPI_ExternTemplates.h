#pragma once

#include "GameAPI.h"
#include "../components/RenderComponents.h"
#include "../components/PhysicsComponents.h"
#include "../components/AnimatorComponent.h"
#include "../components/TextComponent.h"
#include "../components/LightComponents.h"

namespace engine {

// extern template 声明：防止在客户端代码中实例化
// 实际实例化在 GameAPI_ExplicitInstantiation.cpp 中

extern template QGAME_ENGINE_API EntityID&    GameAPI::addComponent<EntityID>(entt::entity, EntityID);
extern template QGAME_ENGINE_API Transform&   GameAPI::addComponent<Transform>(entt::entity, Transform);
extern template QGAME_ENGINE_API Camera&      GameAPI::addComponent<Camera>(entt::entity, Camera);
extern template QGAME_ENGINE_API Sprite&      GameAPI::addComponent<Sprite>(entt::entity, Sprite);
extern template QGAME_ENGINE_API TileMap&     GameAPI::addComponent<TileMap>(entt::entity, TileMap);
extern template QGAME_ENGINE_API Name&        GameAPI::addComponent<Name>(entt::entity, Name);
extern template QGAME_ENGINE_API TextComponent& GameAPI::addComponent<TextComponent>(entt::entity, TextComponent);
extern template QGAME_ENGINE_API Light2D& GameAPI::addComponent<Light2D>(entt::entity, Light2D);
extern template QGAME_ENGINE_API LightOccluder2D& GameAPI::addComponent<LightOccluder2D>(entt::entity, LightOccluder2D);
extern template QGAME_ENGINE_API Reflector2D& GameAPI::addComponent<Reflector2D>(entt::entity, Reflector2D);
extern template QGAME_ENGINE_API Environment2D& GameAPI::addComponent<Environment2D>(entt::entity, Environment2D);

extern template QGAME_ENGINE_API RigidBody&   GameAPI::addComponent<RigidBody>(entt::entity, RigidBody);
extern template QGAME_ENGINE_API Collider&    GameAPI::addComponent<Collider>(entt::entity, Collider);

extern template QGAME_ENGINE_API AnimatorComponent& GameAPI::addComponent<AnimatorComponent>(entt::entity, AnimatorComponent);

extern template QGAME_ENGINE_API EntityID&    GameAPI::getComponent<EntityID>(entt::entity);
extern template QGAME_ENGINE_API Transform&   GameAPI::getComponent<Transform>(entt::entity);
extern template QGAME_ENGINE_API Camera&      GameAPI::getComponent<Camera>(entt::entity);
extern template QGAME_ENGINE_API Sprite&      GameAPI::getComponent<Sprite>(entt::entity);
extern template QGAME_ENGINE_API TileMap&     GameAPI::getComponent<TileMap>(entt::entity);
extern template QGAME_ENGINE_API Name&        GameAPI::getComponent<Name>(entt::entity);
extern template QGAME_ENGINE_API TextComponent& GameAPI::getComponent<TextComponent>(entt::entity);
extern template QGAME_ENGINE_API Light2D& GameAPI::getComponent<Light2D>(entt::entity);
extern template QGAME_ENGINE_API LightOccluder2D& GameAPI::getComponent<LightOccluder2D>(entt::entity);
extern template QGAME_ENGINE_API Reflector2D& GameAPI::getComponent<Reflector2D>(entt::entity);
extern template QGAME_ENGINE_API Environment2D& GameAPI::getComponent<Environment2D>(entt::entity);

extern template QGAME_ENGINE_API RigidBody&   GameAPI::getComponent<RigidBody>(entt::entity);
extern template QGAME_ENGINE_API Collider&    GameAPI::getComponent<Collider>(entt::entity);

extern template QGAME_ENGINE_API AnimatorComponent& GameAPI::getComponent<AnimatorComponent>(entt::entity);

extern template QGAME_ENGINE_API bool GameAPI::hasComponent<EntityID>(entt::entity) const;
extern template QGAME_ENGINE_API bool GameAPI::hasComponent<Transform>(entt::entity) const;
extern template QGAME_ENGINE_API bool GameAPI::hasComponent<Camera>(entt::entity) const;
extern template QGAME_ENGINE_API bool GameAPI::hasComponent<Sprite>(entt::entity) const;
extern template QGAME_ENGINE_API bool GameAPI::hasComponent<TileMap>(entt::entity) const;
extern template QGAME_ENGINE_API bool GameAPI::hasComponent<Name>(entt::entity) const;
extern template QGAME_ENGINE_API bool GameAPI::hasComponent<TextComponent>(entt::entity) const;
extern template QGAME_ENGINE_API bool GameAPI::hasComponent<Light2D>(entt::entity) const;
extern template QGAME_ENGINE_API bool GameAPI::hasComponent<LightOccluder2D>(entt::entity) const;
extern template QGAME_ENGINE_API bool GameAPI::hasComponent<Reflector2D>(entt::entity) const;
extern template QGAME_ENGINE_API bool GameAPI::hasComponent<Environment2D>(entt::entity) const;

extern template QGAME_ENGINE_API bool GameAPI::hasComponent<RigidBody>(entt::entity) const;
extern template QGAME_ENGINE_API bool GameAPI::hasComponent<Collider>(entt::entity) const;

extern template QGAME_ENGINE_API bool GameAPI::hasComponent<AnimatorComponent>(entt::entity) const;

} // namespace engine
