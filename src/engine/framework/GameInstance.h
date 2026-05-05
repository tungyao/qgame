#pragma once

namespace engine {

struct GameContext;

// GameInstance 是游戏侧的生命周期入口。
//
// 这个类刻意保持很薄：引擎仍然负责窗口、输入泵、系统调度、渲染提交和
// 资源缓存；游戏只在这些稳定阶段里注册内容、更新规则、释放自己持有的状态。
// 后续 Native game / Native mod 都可以复用同一个生命周期形状。
class GameInstance {
public:
    GameInstance();
    virtual ~GameInstance();

    // 初始化阶段用于加载游戏级 manifest、注册场景/Prefab/System，以及创建
    // 首帧前必须存在的游戏状态。返回 false 表示游戏启动失败。
    virtual bool onInit(GameContext& ctx) {
        (void)ctx;
        return true;
    }

    // 每帧逻辑更新入口。dt 使用秒为单位，来自 EngineContext::deltaTime，
    // 这样游戏层不需要知道 FrameScheduler 的内部时间来源。
    virtual void onUpdate(GameContext& ctx, float dt) {
        (void)ctx;
        (void)dt;
    }

    // 关闭阶段用于注销游戏层回调、释放 Native 侧资源、清理持久化状态。
    // 引擎对象仍然有效，但游戏不应在这里启动新的异步加载或切场景。
    virtual void onShutdown(GameContext& ctx) {
        (void)ctx;
    }
};

} // namespace engine
