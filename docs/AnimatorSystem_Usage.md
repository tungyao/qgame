# AnimatorSystem 使用文档

> 覆盖 Phase 1–4 已落地能力：优先级/打断/队列、帧事件、参数+FSM+Crossfade、动画分层
> 源码：`src/engine/components/AnimatorComponent.h`、`src/engine/systems/AnimatorSystem.cpp`
> Phase 5（程序化动画 / Tween / Spring / hit-stop）尚未实现，参见 `PLAN_AnimatorSystem_Evolution.md`

---

## 目录

1. [资产数据结构](#1-资产数据结构)
2. [组件挂载](#2-组件挂载)
3. [Phase 1：play / queue / 优先级](#3-phase-1playqueue优先级)
4. [Phase 2：帧事件 (Animation Notify)](#4-phase-2帧事件-animation-notify)
5. [Phase 3：参数 + FSM + Crossfade](#5-phase-3参数--fsm--crossfade)
6. [Phase 4：动画分层 (Layers)](#6-phase-4动画分层-layers)
7. [系统执行顺序](#7-系统执行顺序)
8. [事件命名约定](#8-事件命名约定)
9. [常见用法配方](#9-常见用法配方)
10. [限制与注意事项](#10-限制与注意事项)

---

## 1. 资产数据结构

### AnimationClip
```cpp
struct AnimationFrame {
    core::Rect srcRect;     // spritesheet 内像素区域
    float      duration;    // 本帧时长 (秒)
};

struct AnimEvent {
    float       time;       // 在 clip 内的时间 (秒)
    std::string name;       // 见 §8 命名约定
    int         intParam;
    float       floatParam;
    std::string stringParam;
};

struct AnimationClip {
    std::string                 name;
    TextureHandle               texture;
    std::vector<AnimationFrame> frames;
    float                       duration;   // 自动 = sum(frame.duration)
    bool                        loop;
    std::vector<AnimEvent>      events;     // 必须按 time 升序
};
```

### AnimatorController
```cpp
struct AnimState {
    std::string     name;
    AnimationHandle clip;
    float           speed = 1.f;
    PlayMode        mode  = PlayMode::ClipDefault; // Once/Loop/PingPong/ClipDefault
};

struct AnimTransition {
    int    from = kAnyState;             // -1 = AnyState
    int    to;
    std::vector<TransitionCondition> conditions;
    bool   hasExitTime = false;
    float  exitTime    = 1.f;            // 归一化 [0,1]
    float  duration    = 0.f;            // crossfade 秒
    bool   interruptible = true;
};

struct AnimatorController {
    // ── Base 层 (扁平字段，向后兼容) ─────────────────
    std::vector<AnimState>      states;
    std::vector<AnimTransition> transitions;
    int                         defaultState = 0;

    // ── Phase 4: 额外层 ─────────────────────────────
    std::vector<AnimatorLayer>  layers;
};
```

---

## 2. 组件挂载

```cpp
auto ctrl = std::make_shared<engine::AnimatorController>();
ctrl->states = {
    { "Idle",   idleAnim,   0.5f, engine::PlayMode::Loop },
    { "Walk",   walkAnim,   1.5f, engine::PlayMode::Loop },
    { "Attack", attackAnim, 1.0f, engine::PlayMode::Once },
};
ctrl->transitions = {
    { 0, 1, { { "speed",  engine::ConditionOp::Greater, 0.1f } }, false, 1.f, 0.05f, true },
    { 1, 0, { { "speed",  engine::ConditionOp::Less,    0.1f } }, false, 1.f, 0.05f, true },
    { engine::kAnyState, 2, { { "attack", engine::ConditionOp::Trigger, 0.f } }, false, 1.f, 0.f, true },
    { 2, 0, {}, /*hasExitTime*/ true, 1.f, 0.f, true },
};
ctrl->defaultState = 0;

engine::AnimatorComponent ac{};
ac.applyTexture = true;     // 切 clip 时自动同步 Sprite.texture
ac.controller   = ctrl;     // 启用 FSM 路径；不赋则走 Phase 1 直接播放路径
api.addComponent(entity, ac);
```

实体还需挂 `Sprite` 组件，AnimatorSystem 每帧写回 `srcRect` (+ 可选 `texture`)。

---

## 3. Phase 1：play / queue / 优先级

适用于无 controller 的轻量场景（粒子、UI 反馈、未状态机化的怪物）。

### API
```cpp
struct PlayOptions {
    int      priority     = 0;       // 越大越高
    bool     forceRestart = false;   // 同 clip 也重置 time
    float    startTime    = 0.f;
    float    speed        = 1.f;
    PlayMode mode         = PlayMode::ClipDefault;
};

void AnimatorComponent::play(AnimationHandle anim, const PlayOptions& opts = {});
void AnimatorComponent::queue(AnimationHandle anim, const PlayOptions& opts = {});
void AnimatorComponent::lock();    // 锁定当前 clip 不可被打断
void AnimatorComponent::unlock();
void AnimatorComponent::stop();
```

### 决策规则（在 `AnimatorComponent::play` 内）
1. `playing && !interruptible && opts.priority < currentPriority` → 入队，等当前播完后再播
2. 同 clip 且 `playing && !forceRestart` → 仅刷新 speed/priority，不重置 time
3. 否则立即切换；新 clip 默认 `interruptible=true`，需要锁帧的玩法层调 `lock()`

### 配方：攻击启动锁定 + 完成回 idle
```cpp
anim.play(attackClip, { .priority=10, .forceRestart=true, .mode=PlayMode::Once });
anim.lock();                                  // 起手帧不可打断
anim.queue(idleClip, { .mode=PlayMode::Loop });// 攻击完成后自动续 idle
// 受击优先级更高时可立刻覆盖
anim.play(hurtClip, { .priority=20, .forceRestart=true });
```

> 当 `controller` 已设置时，FSM 决策接管"播什么"，`play()` 仍然可用但通常不再需要——改用参数/Trigger（§5）。

---

## 4. Phase 2：帧事件 (Animation Notify)

### 写在 clip 上
```cpp
clip.events = {
    { 0.10f, "hitbox_on" },
    { 0.18f, "hitbox_off" },
    { 0.05f, "sfx", 0, 0.f, "swing" },
    { 0.12f, "vfx", 0, 0.f, "slash_arc" },
};
```
**约束**：`events` 须按 `time` 升序。事件由 AnimatorSystem 在 `[prevTime, newTime)` 区间扫描派发，loop 跨 duration 边界自动分两段扫，保证不丢不重。

### 派发方式
事件写入实体的 `AnimEventQueue` 组件（按需 lazy-emplace）。**生命周期为单帧**：AnimatorSystem 每帧 update 起始清空。

### 消费侧
其他 system / gameplay 在同帧后续步骤里读队列：
```cpp
if (auto* q = api.tryGetComponent<engine::AnimEventQueue>(ent)) {
    for (const auto& e : q->events) {
        if (e.name == "hitbox_on")  hitbox.enable(ent);
        else if (e.name == "hitbox_off") hitbox.disable(ent);
        else if (e.name == "sfx")   audio.play(e.stringParam);
    }
}
```

> 速度为负、暂停 (speed=0)、跨帧大步进，扫描方向自适应；非循环到端被 clamp 时仍会发末帧事件。

---

## 5. Phase 3：参数 + FSM + Crossfade

### 5.1 参数黑板
```cpp
anim.setFloat  ("speed", 0.0f);
anim.setInt    ("combo", 1);
anim.setBool   ("grounded", true);
anim.setTrigger("attack");           // 一次性，被 transition 消费后自动复位
anim.resetTrigger("attack");         // 显式复位
float v = anim.getFloat("speed");
```

参数全局保存在 `AnimatorComponent.parameters`，所有层共享同一份黑板。

### 5.2 转移条件
```cpp
enum class ConditionOp {
    Greater, GreaterEq, Less, LessEq, Equal, NotEqual,
    IsTrue, IsFalse,
    Trigger,    // 仅 ParamType::Trigger 有效；命中时由 system 自动 reset
};
```
- 多个 condition 之间为 **AND**；任一失败本条 transition 不触发
- transitions 按声明顺序求值，**首条命中即停**——优先级用顺序表达
- `from = kAnyState` (-1) → 任意状态可进入；自动跳过 `to == currentState`

### 5.3 hasExitTime
- `hasExitTime=true`：仅当当前状态归一化进度 `time/duration ≥ exitTime` 时允许转移
- 配 `conditions=[]` 用于 "Once 状态播完自动回某状态"：
  ```cpp
  { 2, 0, {}, true, 1.f, 0.f, true }   // Attack 完整播完 → Idle
  ```

### 5.4 Crossfade
- `transition.duration > 0` 时，AnimatorSystem 记录 `fromState` + `transitionT` 进入过渡期
- **2D sprite 当前为硬切**：源状态停止推进，目标状态立即开始（不混合像素），过渡时长仅作为"打断窗口"语义保留
- 真混合留给后续骨骼/Spine 接入；接口和数据保持向前兼容

### 5.5 状态事件
状态机通过同一个 `AnimEventQueue` 派发以下事件（消费方式与帧事件相同）：
- `state_enter:<StateName>`
- `state_exit:<StateName>`
- `state_finished` （Once 状态 clamp 到末尾时一次）

---

## 6. Phase 4：动画分层 (Layers)

适用：上半身/下半身分离、受击红闪叠加、瞄准偏移等"叠在 base 上"的播放轨。

### 6.1 数据
```cpp
namespace LayerChannel {
    enum : uint32_t {
        SrcRect  = 1u << 0,
        Texture  = 1u << 1,
        Tint     = 1u << 2,
        Offset   = 1u << 3,
        Rotation = 1u << 4,
        Scale    = 1u << 5,
    };
}
enum class LayerBlendMode : uint8_t { Override, Additive };

struct AnimatorLayer {
    std::string     name;
    float           weight    = 1.f;
    LayerBlendMode  blendMode = LayerBlendMode::Override;
    uint32_t        mask      = LayerChannel::SrcRect | LayerChannel::Texture;
    std::vector<AnimState>      states;
    std::vector<AnimTransition> transitions;
    int             defaultState = 0;
};
```
`AnimatorController.layers` 是**额外层**集合（Base 层仍由扁平字段表达）。每个额外层：
- 拥有独立 FSM（自己的 states/transitions/defaultState）
- 共享 AnimatorComponent 的参数黑板（同一参数可同时驱动多层）
- 共享同一个 `AnimEventQueue`（事件按发生顺序追加）

### 6.2 合成规则
渲染写回顺序：**Base 层先写 → 各额外层按声明顺序覆盖对应通道**。

| 通道 | 数据来源 | Override | Additive |
|---|---|---|---|
| `SrcRect` | clip frame | 覆盖 `Sprite.srcRect` | （无帧增量语义，留给 Phase 5） |
| `Texture` | `clip.texture` | 覆盖 `Sprite.texture` | — |
| `Tint` / `Offset` / `Rotation` / `Scale` | 程序化层（Phase 5） | 待 Phase 5 实现 | 待 Phase 5 实现 |

`weight <= 0` 的层跳过合成（FSM 仍推进，事件仍发）。

### 6.3 配方：上半身挥剑覆盖（图集已拆）
```cpp
engine::AnimatorLayer upper;
upper.name      = "UpperBody";
upper.weight    = 1.0f;
upper.blendMode = engine::LayerBlendMode::Override;
upper.mask      = engine::LayerChannel::SrcRect;   // 不抢 base 的 texture
upper.states    = {
    { "Empty",  emptyClip,    1.f, engine::PlayMode::Loop }, // 空 srcRect，等价"不覆盖"
    { "Swing",  upperSwing,   1.f, engine::PlayMode::Once },
};
upper.transitions = {
    { engine::kAnyState, 1, { { "swing", engine::ConditionOp::Trigger, 0.f } } },
    { 1, 0, {}, true, 1.f, 0.f, true },     // 播完回 Empty
};
ctrl->layers.push_back(std::move(upper));
```

### 6.4 运行时
`AnimatorComponent.extraLayers` 由系统自动按 `controller.layers.size()` 调整。每个 `AnimatorLayerRuntime` 持有该层独立的 `currentState/time/speed/fromState/...`。

---

## 7. 系统执行顺序

`AnimatorSystem::update(dt)` 对每个 entity：

1. 清空当帧 `AnimEventQueue`
2. 若有 `controller`：基础层 FSM 评估 → 转移启动 → crossfade 计时
3. 各额外层 FSM 评估（共享黑板与队列）
4. 基础层时间推进 + 帧事件扫描
5. 基础层完成处理（`state_finished` 派发、`queuedAnim` 续播）
6. 基础层写回 `Sprite.srcRect` (+ texture)
7. 各额外层时间推进 + 事件扫描 + 合成写回（按 mask + blendMode）

> 消费帧事件的 system 必须排在 AnimatorSystem 之后、下一帧 update 开始之前，否则队列会被清空。

---

## 8. 事件命名约定

| 名称 | 含义 |
|---|---|
| `hitbox_on` / `hitbox_off` | 打开/关闭命中盒 |
| `damage_window_start` / `damage_window_end` | 伤害判定窗口 |
| `footstep` | 脚步声/尘土 |
| `sfx` | 通用音效（`stringParam` 给 sound id） |
| `vfx` | 通用特效（`stringParam` 给 effect id） |
| `state_enter:<Name>` | 状态机进入某状态 |
| `state_exit:<Name>` | 状态机离开某状态 |
| `state_finished` | Once 状态播完一次 |

自定义事件按需扩展，但建议小写 + 下划线，跨项目可读性更好。

---

## 9. 常见用法配方

### A. gameplay 只写参数，不写 play()
```cpp
auto& anim = api.getComponent<engine::AnimatorComponent>(player);
anim.setFloat("speed", glm::length(velocity));
if (input.pressed("attack")) anim.setTrigger("attack");
if (tookDamage)              anim.setTrigger("hurt");
// AnyState→Hurt、Idle⇄Walk、Attack→Idle 等转移由 controller 处理
```

### B. 受击红闪（占位，等 Phase 5 程序化层）
当前 Phase 4 帧动画无法直接写 tint。**临时方案**：gameplay 直接写 `Sprite.tint`，并用 `state_enter:Hurt` 事件触发 0.1s 计时器复位。Phase 5 落地后改为 `Tint` 通道的 Additive 程序化层。

### C. queued 续播（无 FSM 时）
```cpp
anim.play(spawnClip, { .mode=PlayMode::Once, .priority=5 });
anim.queue(idleClip, { .mode=PlayMode::Loop });
```
spawn 播完后自动接 idle。

### D. AnyState→Hurt 全局打断
```cpp
ctrl->transitions.push_back({
    engine::kAnyState, hurtStateIdx,
    { { "hurt", engine::ConditionOp::Trigger, 0.f } },
    /*hasExitTime*/ false, 0.f, /*crossfade*/ 0.05f, /*interruptible*/ true,
});
```
任意状态命中 `hurt` trigger 时立刻切到 Hurt 状态。

### E. 同 clip 重复触发不闪回起始帧
直接 `play(sameClip, { /* forceRestart=false 默认 */ })`：当前 clip 不变 → 仅刷新 speed/priority，时间继续推进。

---

## 10. 限制与注意事项

- **Crossfade 不做像素混合**：sprite 模式下 transition.duration 仅是逻辑过渡时长，画面是硬切。骨骼/Spine 接入后再启用真混合。
- **PingPong = Loop**：当前实现把 `PingPong` 视作 Loop；真正反向播放（speed<0）已支持，但 PingPong 自动反向需 Phase 5。
- **trigger 复位时机**：transition 命中时由系统消费一次。多个 transition 在同一帧都依赖同一 trigger 时，仅首条生效（顺序匹配）。
- **events 必须升序**：未排序时跨边界扫描可能漏发。资产导入端应做校验。
- **AnimEventQueue 单帧**：消费侧若跨帧延迟处理会丢事件，应在同帧用完。
- **Layer Tint/Offset/Rotation/Scale 通道暂为占位**：Phase 4 仅落地 SrcRect/Texture 合成，其余通道等 Phase 5 程序化层提供数据源。
- **额外层无 priority/queued**：层级是结构性叠加，不参与基础层的请求队列模型。
- **基础层兼容字段**：`AnimatorController.states/transitions/defaultState` 等价于"Base 层"。新代码可继续使用扁平字段，只在需要叠加时往 `layers` 里推。

---

## 附：组件/资产清单

| 类型 | 角色 |
|---|---|
| `AnimationClip`（资产） | 帧序列 + 帧事件 |
| `AnimatorController`（资产，shared_ptr） | Base 层 FSM + 额外层数组 |
| `AnimatorComponent`（组件） | 当前播放、参数黑板、各层运行时 |
| `AnimEventQueue`（组件，单帧） | 帧事件 + 状态事件输出口 |
| `Sprite`（组件） | 写回目标：srcRect / texture |
| `AnimatorSystem`（系统） | 每帧驱动以上全部 |
