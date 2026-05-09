#pragma once

#include <cstdint>
#include <initializer_list>

namespace engine {

// UpdatePhase 把一帧拆成一组有明确输入/输出语义的阶段。
//
// 设计目标：
// 1. 让“系统应该在什么时候跑”变成显式声明，而不是依赖注册顺序猜测。
// 2. 把物理前输入、物理后玩法、相机、UI、Render 等时机明确区分开。
// 3. 为后续的插值渲染、网络同步、编辑器预览和回放系统保留稳定骨架。
//
// 阶段约定：
// - Input:
//     采集本帧输入快照。只写 InputState，不直接改世界实体。
// - GameplayPrePhysics:
//     把输入/AI 决策转换成物理可消费的控制量，如 RigidBody.velocity。
// - Physics:
//     固定时间步模拟、碰撞、位移结算。
// - GameplayPostPhysics:
//     消费最终物理结果，处理交互、触发器、状态机切换等。
// - Animation:
//     根据 gameplay 状态推进动画和表现层输出。
// - Camera:
//     在世界状态稳定后更新相机跟随、边界夹紧、镜头特效。
// - UI:
//     读取世界和相机的最终状态，完成 UI 布局、命中和 draw command 构建。
// - Render:
//     只读最终状态，提交 GPU work。
// - PostFrame:
//     帧尾清理、统计、延迟销毁等，不应再影响本帧画面。
enum class UpdatePhase : uint8_t {
    Input = 0,
    GameplayPrePhysics,
    Physics,
    GameplayPostPhysics,
    Animation,
    Camera,
    UI,
    Render,
    PostFrame,
    Count
};

using UpdatePhaseMask = uint32_t;

inline constexpr UpdatePhaseMask updatePhaseBit(UpdatePhase phase) {
    return 1u << static_cast<uint32_t>(phase);
}

inline constexpr UpdatePhaseMask updatePhaseBits(std::initializer_list<UpdatePhase> phases) {
    UpdatePhaseMask mask = 0u;
    for (UpdatePhase phase : phases) {
        mask |= updatePhaseBit(phase);
    }
    return mask;
}

inline constexpr const char* updatePhaseName(UpdatePhase phase) {
    switch (phase) {
        case UpdatePhase::Input:              return "Input";
        case UpdatePhase::GameplayPrePhysics: return "GameplayPrePhysics";
        case UpdatePhase::Physics:            return "Physics";
        case UpdatePhase::GameplayPostPhysics:return "GameplayPostPhysics";
        case UpdatePhase::Animation:          return "Animation";
        case UpdatePhase::Camera:             return "Camera";
        case UpdatePhase::UI:                 return "UI";
        case UpdatePhase::Render:             return "Render";
        case UpdatePhase::PostFrame:          return "PostFrame";
        case UpdatePhase::Count:
        default:                              return "Unknown";
    }
}

class ISystem {
public:
    virtual ~ISystem() = default;

    // ── 生命周期 ────────────────────────────────────────────────────────────
    virtual void init()              {}
    virtual void shutdown()          {}

    // ── 新 Phase 驱动接口 ─────────────────────────────────────────────────
    //
    // phaseMask() 声明系统希望参与哪些阶段。FrameScheduler 会按固定阶段顺序扫描
    // 所有系统；若某系统的 mask 包含当前 phase，就调用 runPhase(phase, dt)。
    //
    // 默认实现刻意保持对旧接口的兼容：
    //   - preUpdate() 映射到 GameplayPrePhysics
    //   - update(dt) 映射到 GameplayPostPhysics
    //   - postUpdate() 映射到 PostFrame
    //
    // 这样现有系统可以先不改实现，仍然按旧语义运行；但新系统应尽量显式覆写
    // phaseMask() + 对应的 phase hook，避免时序再依赖隐式约定。
    virtual UpdatePhaseMask phaseMask() const {
        return updatePhaseBits({
            UpdatePhase::GameplayPrePhysics,
            UpdatePhase::GameplayPostPhysics,
            UpdatePhase::PostFrame
        });
    }

    // 返回 true 表示调度器应继续执行；InputPhase 可用它中止主循环。
    virtual bool runPhase(UpdatePhase phase, float dt) {
        switch (phase) {
            case UpdatePhase::Input:
                return onInputPhase(dt);
            case UpdatePhase::GameplayPrePhysics:
                onGameplayPrePhysicsPhase(dt);
                return true;
            case UpdatePhase::Physics:
                onPhysicsPhase(dt);
                return true;
            case UpdatePhase::GameplayPostPhysics:
                onGameplayPostPhysicsPhase(dt);
                return true;
            case UpdatePhase::Animation:
                onAnimationPhase(dt);
                return true;
            case UpdatePhase::Camera:
                onCameraPhase(dt);
                return true;
            case UpdatePhase::UI:
                onUIPhase(dt);
                return true;
            case UpdatePhase::Render:
                onRenderPhase(dt);
                return true;
            case UpdatePhase::PostFrame:
                onPostFramePhase(dt);
                return true;
            case UpdatePhase::Count:
            default:
                return true;
        }
    }

    // ── 旧接口（兼容层）───────────────────────────────────────────────────
    //
    // 旧系统和 native mod 仍可以继续覆写 preUpdate/update/postUpdate。
    // 新调度器会把它们映射到：
    //   preUpdate  -> GameplayPrePhysics
    //   update     -> GameplayPostPhysics（manual system 除外）
    //   postUpdate -> PostFrame
    virtual void preUpdate()         {}
    virtual void update(float dt)    { (void)dt; }
    virtual void postUpdate()        {}

    // 返回 true 表示此 System 仍由某个特定 phase 显式驱动，不应走旧的
    // GameplayPostPhysics -> update(dt) 兼容映射。
    //
    // 这主要服务于兼容层；新代码应优先通过 phaseMask()/runPhase() 建模。
    virtual bool isManuallyScheduled() const { return false; }

protected:
    // ── Phase Hook 默认实现 ───────────────────────────────────────────────
    //
    // 这些 hook 让新系统可以按语义直接覆写目标 phase，而不需要自己写 runPhase
    // switch。默认实现只做最小兼容映射，避免旧系统瞬间全部失效。
    virtual bool onInputPhase(float /*dt*/) {
        return true;
    }

    virtual void onGameplayPrePhysicsPhase(float /*dt*/) {
        preUpdate();
    }

    virtual void onPhysicsPhase(float /*dt*/) {}

    virtual void onGameplayPostPhysicsPhase(float dt) {
        if (!isManuallyScheduled()) {
            update(dt);
        }
    }

    virtual void onAnimationPhase(float /*dt*/) {}
    virtual void onCameraPhase(float /*dt*/) {}
    virtual void onUIPhase(float /*dt*/) {}
    virtual void onRenderPhase(float /*dt*/) {}

    virtual void onPostFramePhase(float /*dt*/) {
        postUpdate();
    }
};

} // namespace engine
