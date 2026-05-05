#pragma once

#include <entt/entt.hpp>

#include "../assets/AssetManager.h"
#include "../input/InputState.h"
#include "../runtime/EngineContext.h"
#include "../runtime/SystemRegistry.h"

namespace engine {

class GameAPI;
class SceneManager;

// GameContext 是 Game Framework 暴露给游戏/Mod 的“公开工作台”。
//
// 它只保存对现有引擎对象的引用或公开门面指针，不拥有底层资源。这样可以把
// Game Framework 放在 EngineContext 之上，而不改变当前渲染、音频、ECS、
// AssetManager 的生命周期。
struct GameContext {
    explicit GameContext(EngineContext& engineContext)
        : engine(engineContext)
        , world(engineContext.world)
        , events(engineContext.dispatcher)
        , assets(engineContext.assetManager)
        , input(engineContext.inputState)
        , systems(engineContext.systems) {}

    // 完整引擎上下文仍保留给游戏本体使用。Native Mod API 后续会收窄到
    // C ABI 函数表，不直接暴露 EngineContext。
    EngineContext& engine;

    // World 是当前运行场景的 ECS registry。SceneManager::loadScene 会重建
    // 这个 registry 内的实体，因此游戏层不要跨场景长期保存裸 entity。
    entt::registry& world;

    // 事件分发器用于游戏系统之间通信；后续 EventBusAPI 会包装这层。
    entt::dispatcher& events;

    // 资源入口统一走 stable asset ID 或 manifest 记录，方便 Mod 覆盖。
    AssetManager& assets;

    // 输入状态由 InputSystem 每帧填充，GameInstance 只读取，不主动泵事件。
    InputState& input;

    // 系统注册表目前仍是 C++ 层入口；Native Mod 阶段会在其上提供 C ABI 包装。
    SystemRegistry& systems;

    // 可选门面。由宿主在创建 SceneManager / GameAPI 后回填，避免 GameContext
    // 自己拥有这些对象，从而保持初始化顺序简单明确。
    SceneManager* scenes = nullptr;
    GameAPI* api = nullptr;

    // 便捷读取当前帧 dt。保留函数形式，后续如果加入暂停/时间缩放策略，
    // 调用点不需要知道 EngineContext 的字段细节。
    float deltaTime() const { return engine.deltaTime; }
};

} // namespace engine
