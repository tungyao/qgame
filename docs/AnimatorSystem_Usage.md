# AnimatorSystem 使用文档

> 覆盖 Phase 1–5 已落地能力：优先级/打断/队列、帧事件、参数+FSM+Crossfade、动画分层、Tween/Spring/程序化层/时间缩放
> 源码：`src/engine/components/AnimatorComponent.h`、`src/engine/systems/AnimatorSystem.cpp`、`src/engine/systems/TweenSystem.cpp`、`src/engine/anim/{Easing,SpringValue}.h`

---

## 目录

1. [资产数据结构](#1-资产数据结构)
2. [组件挂载](#2-组件挂载)
3. [Phase 1：play / queue / 优先级](#3-phase-1playqueue优先级)
4. [Phase 2：帧事件 (Animation Notify)](#4-phase-2帧事件-animation-notify)
5. [Phase 3：参数 + FSM + Crossfade](#5-phase-3参数--fsm--crossfade)
6. [Phase 4：动画分层 (Layers)](#6-phase-4动画分层-layers)
7. [Phase 5：Tween / Spring / 程序化层 / 时间缩放](#7-phase-5tween--spring--程序化层--时间缩放)
8. [系统执行顺序](#8-系统执行顺序)
9. [事件命名约定](#9-事件命名约定)
10. [常见用法配方](#10-常见用法配方)
11. [限制与注意事项](#11-限制与注意事项)

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

## 7. Phase 5：Tween / Spring / 程序化层 / 时间缩放

### 7.1 时间缩放 (5.4)
- `EngineContext.timeScale`：全局时间缩放（hit-stop / 慢动作）
- `AnimatorComponent.localTimeScale`：单体缩放
- AnimatorSystem 内 `dt = rawDt * global * local`；TweenSystem 内 `dt = rawDt * global`
```cpp
ctx.timeScale = 0.05f;       // 全局顿帧
anim.localTimeScale = 2.0f;  // 该 entity 加倍速
```

### 7.2 TweenSystem (5.1)
轻量补间，独立于 Animator。挂 `TweenComponent`，每个实例为 `TweenInstance`。
```cpp
engine::TweenComponent& tc = api.getOrEmplace<engine::TweenComponent>(ent);
engine::TweenInstance t{};
t.channel  = engine::TweenChannel::ScaleX;
t.from     = 0.f;  t.to = 1.f;
t.duration = 0.4f;
t.easing   = engine::Easing::BackOut;     // 弹入
tc.add(t);
```
通道：`PositionX/Y、Rotation、ScaleX/Y、SpriteTintR/G/B/A、CameraZoom、Custom`。
完成后默认从容器移除 (`removeOnFinish=true`)；`loop`、`pingpong` 可选；`Custom` 通道由调用者读 `outValue`。

Easing：`Linear/Quad/Cubic/Quart/Sine/Expo/Back/Elastic/Bounce` × `In/Out/InOut`。

### 7.3 SpringValue (5.2)
header-only 工具，调用方按需 `update(dt)`。
```cpp
engine::SpringValue camFollow{};
camFollow.stiffness = 120.f;
camFollow.damping   = engine::SpringValue::criticalDamping(120.f);
// 每帧：
camFollow.target = player.x;
camera.x = camFollow.update(dt);
```
适用：相机跟随、UI 弹入、采集物吸附、命中位移回弹。

### 7.4 程序化层 (5.3)
`AnimatorLayer.kind != ProceduralKind::None` 即为程序化层，忽略 `states/transitions`，每帧由 AnimatorSystem 调用对应求值器，写入 `AnimatorOutput` 组件，RenderSystem 在 `updateGPUSlot` 时合成到 Sprite/Transform。

内置类型：

| Kind | 通道 | 触发方式 | 关键 config |
|---|---|---|---|
| `HitShake` | Offset (X+Y) | trigger | amplitude (像素)、frequency (Hz)、duration (秒) |
| `HurtFlash` | Tint 加色 (R+) | trigger | amplitude (0..1 红色强度)、duration |
| `BreatheBob` | Offset Y | strength 参数（持续） | amplitude、frequency |
| `SquashStretchOnLand` | Scale (XY 反向) | trigger | amplitude (比例 0..1)、duration |

`AnimatorOutput`（per-entity，AnimatorSystem 自动 emplace + 每帧重置）：
```cpp
struct AnimatorOutput {
    float       offsetX, offsetY;
    float       rotationOffset;
    float       scaleMulX, scaleMulY;
    core::Color tintMul;        // 仅 r 通道用作 HurtFlash 加色偏移
};
```

### 配方：受击红闪 + 抖动
```cpp
ctrl->layers.push_back({
    "Hurt", 1.f, engine::LayerBlendMode::Override, /*mask*/ 0,
    {}, {}, 0,
    engine::ProceduralKind::HurtFlash,
    { /*trigger*/"hurt_flash", "", /*amp*/1.0f, 0.f, /*duration*/0.15f }
});
ctrl->layers.push_back({
    "Shake", 1.f, engine::LayerBlendMode::Additive, 0, {}, {}, 0,
    engine::ProceduralKind::HitShake,
    { "hurt_flash", "", /*amp*/4.f, /*freq*/30.f, /*duration*/0.18f }
});
// gameplay 命中时：
anim.setTrigger("hurt_flash");
```

### 配方：呼吸抖动（持续，强度由参数控制）
```cpp
anim.setFloat("breath", 1.0f);
ctrl->layers.push_back({
    "Breathe", 1.f, engine::LayerBlendMode::Additive, 0, {}, {}, 0,
    engine::ProceduralKind::BreatheBob,
    { "", /*strength*/"breath", /*amp*/1.5f, /*freq*/0.6f, 0.f }
});
```

### 配方：hit-stop
```cpp
// 命中触发：
ctx.timeScale = 0.05f;
springTimer = 0.05f;
// 主循环：
springTimer -= rawDt;
if (springTimer <= 0) ctx.timeScale = 1.0f;
```

---

## 8. 系统执行顺序

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

## 9. 事件命名约定

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

## 10. 常见用法配方

### A. gameplay 只写参数，不写 play()
```cpp
auto& anim = api.getComponent<engine::AnimatorComponent>(player);
anim.setFloat("speed", glm::length(velocity));
if (input.pressed("attack")) anim.setTrigger("attack");
if (tookDamage)              anim.setTrigger("hurt");
// AnyState→Hurt、Idle⇄Walk、Attack→Idle 等转移由 controller 处理
```

### B. 受击红闪
见 §7.4 程序化层 `HurtFlash`：在 controller 加一层 + `setTrigger("hurt_flash")` 即可。

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

## 11. 限制与注意事项

- **Crossfade 不做像素混合**：sprite 模式下 transition.duration 仅是逻辑过渡时长，画面是硬切。骨骼/Spine 接入后再启用真混合。
- **PingPong = Loop**：当前实现把 Animator 的 `PingPong` 视作 Loop（真反向 speed<0 已支持）；TweenSystem 的 `pingpong` 标志已实现来回往返。
- **trigger 复位时机**：transition 命中时由系统消费一次。多个 transition 在同一帧都依赖同一 trigger 时，仅首条生效（顺序匹配）。
- **events 必须升序**：未排序时跨边界扫描可能漏发。资产导入端应做校验。
- **AnimEventQueue 单帧**：消费侧若跨帧延迟处理会丢事件，应在同帧用完。
- **clip 驱动层只用 SrcRect/Texture 通道**：Tint/Offset/Rotation/Scale 由 Phase 5 程序化层填充 `AnimatorOutput`，RenderSystem 在 GPU 上传时叠加。
- **HurtFlash 仅红色加色**：`AnimatorOutput.tintMul.r` 用作 R 通道加色偏移，未实现完整 RGBA 乘法/加法管线。需要更复杂染色请扩 `AnimatorOutput` 与 `RenderSystem::updateGPUSlot`。
- **TweenSystem 与 AnimatorSystem 各自缩放 dt**：均读 `EngineContext.timeScale`；`AnimatorComponent.localTimeScale` 仅作用于 Animator，不影响 Tween。
- **额外层无 priority/queued**：层级是结构性叠加，不参与基础层的请求队列模型。
- **基础层兼容字段**：`AnimatorController.states/transitions/defaultState` 等价于"Base 层"。新代码可继续使用扁平字段，只在需要叠加时往 `layers` 里推。

---

## 12. 当前不足 / 后续工作

按子系统列出实现折衷与未覆盖项；优先级 = 影响面 × 出现频率。

### 12.1 状态机 / 转移
- **Crossfade 无像素混合**：2D sprite 模式下 transition.duration 仅作为逻辑过渡时长（打断窗口），画面是硬切；骨骼/Spine 接入后才能真混合。
- **transition.interruptible 字段未严格生效**：当前过渡期内允许任意新 transition 抢占（计划简化版），后续应按 `interruptible=false` 阻止打断。源码 `tickFSM` 内有占位注释。
- **trigger 仅消费首条匹配**：多个 transition 同帧依赖同一 trigger 时，按声明顺序首条命中即消费，其余 transition 看不到 trigger。需要分支选择请用 bool/float 参数。
- **AnyState→自身**会被静默跳过，但 `from=具体状态, to=自身` 不会跳过——代码不对称，可能造成意外 self-loop。
- **hasExitTime 在 loop 状态下行为模糊**：当前用 `time/duration` 取整数倍归一，跨圈语义未严格定义；非循环状态用 `time/duration + finished` clamp 到 1。
- **defaultState 越界不报错**：直接 silently 不进入任何状态。资产加载端应做校验。

### 12.2 帧事件
- **events 必须按 time 升序**：未排序时扫描行为未定义；当前实现无排序兜底。资产导入端缺失校验。
- **AnimEventQueue 单帧生命周期**：消费 system 必须排在 AnimatorSystem 之后、下一帧 update 之前；跨帧延迟消费会丢事件。
- **状态机 `state_enter/exit/finished` 与帧 events 共用同一队列**：消费方需按 `name` 前缀区分（`state_*`），事件多时遍历开销线性。
- **暂停 (speed=0) 时不发事件**：单帧 `prevTime == newTime` 直接跳过扫描；如果需要"恢复时补发"目前没做。

### 12.3 分层 (Phase 4)
- **额外层无 priority/queued/lock 语义**：层级是结构性叠加，不参与请求队列模型；想"上半身打断重启"目前只能换 trigger。
- **Layer.weight 仅作 0/非 0 开关**：clip 驱动层不做权重插值（2D sprite 无法插值 srcRect），weight 实际只决定要不要写入。Procedural 层把 weight 用作幅度倍率。
- **Additive 模式对 SrcRect/Texture 通道无意义**：帧动画无"增量 srcRect"，当前直接跳过；只有 Procedural 层的 Offset/Scale/Tint 才能 Additive 叠加。
- **layers 不支持嵌套/分组**：扁平数组，写回顺序 = 声明顺序。

### 12.4 程序化层 (Phase 5.3)
- **HurtFlash 只用 R 通道**：`AnimatorOutput.tintMul.r` 用作 R 加色偏移，G/B 通道字段定义了但 RenderSystem 没消费；想做绿/蓝/降饱和闪烁需扩 `updateGPUSlot`。
- **AnimatorOutput 没有完整 RGBA 乘+加管线**：当前只做 `gpuColor.r += tintMul.r/255`，没有 mul（颜色滤镜、亮度衰减）和 alpha 通道叠加。
- **每帧强制 `gpuDirty = true`**：只要 entity 持有 AnimatorOutput 就每帧重传 GPU slot，不区分输出是否实际变化；高密度场景可加 dirty 比对。
- **内置 procedural kind 只有 4 种**：`HitShake / HurtFlash / BreatheBob / SquashStretchOnLand`。需要 AimOffset、Recoil、Tilt、ColorPulse 等需自己加 case 或开放回调式 layer。
- **procedural 层没有 onComplete 回调**：触发型层完成后只是 `procActive = false`，不发事件；想链式触发还得 gameplay 层自己计时。
- **多个 procedural 层共享 AnimatorOutput**：同通道 Override + Additive 混排时按声明顺序覆盖/累加，没有"先全部 Override 再 Additive"的两遍合成。
- **strengthParam 仅 BreatheBob 用**：其他 kind 的强度都得通过 trigger 重置 phase 表达，不支持运行时连续调节振幅。

### 12.5 Tween (Phase 5.1)
- **不支持 onComplete 回调**：`TweenInstance` 没有 `std::function` 字段（避免组件 trivially 不可拷贝），调用方需轮询 `finished` 或用 `removeOnFinish=false` + `userId` 查询。
- **不支持延迟启动 (delay)**：要延迟需自己计时或用 chained tween（也未提供）。
- **不支持序列 (sequence/parallel)**：每个 instance 独立推进，没有 group/yoyo/sequence 容器；复杂时序得手写状态机。
- **Custom 通道值需要调用方自己读**：没有"绑定到任意成员指针"的回调式赋值，C++ 反射缺位的妥协。
- **不影响 `localTimeScale`**：TweenSystem 只乘 `EngineContext.timeScale`；想给单实体做慢动作只能改 `duration`。
- **pingpong 单回合即结束**：`pingpong=true && loop=false` 走完一来一回就 finished；想无限往返需 `loop=true && pingpong=true`。
- **从容器中间删除是 O(n)**：`std::vector` + `remove_if`，每帧重排；活跃 tween > 几百时考虑改 stable list。

### 12.6 Spring (Phase 5.2)
- **裸工具，不参与 ECS**：调用方必须自己存 `SpringValue` 并 `update(dt)`；没有 `SpringComponent` + 系统驱动版本。
- **半隐式欧拉**：在 stiffness 极高 + 大 dt 时可能数值不稳；高频物理推荐 substep 或换 RK4。
- **标量版本**：Vec2/Vec3 需要自己用三个独立实例。

### 12.7 时间缩放 (Phase 5.4)
- **TweenSystem 不读 `localTimeScale`**：单体 hit-stop 只对 Animator 生效，Tween 仍按全局速率推进；想完全冻结需把 `EngineContext.timeScale = 0`。
- **PhysicsSystem / 其他 system 不缩放**：当前只有 Animator + Tween 读 timeScale；hit-stop 期间物理仍正常推进，可能与画面期望不符。
- **没有 hit-stop 工具/事件 API**：要实现"命中 0.05s 顿帧"得 gameplay 自己计时复位 timeScale；可后续封装 `requestHitStop(dt)` 工具。
- **timeScale 极端值不夹紧**：`<0` 或 `>很大` 不报错，行为未定义。

### 12.8 资产 / 序列化
- **AnimatorController / AnimatorLayer 还没有 JSON 加载器**：当前只能代码里手写。`PLAN_AnimatorSystem_Evolution.md` §三里的 JSON schema 尚未在 AssetManager 里实现。
- **procedural layer 配置无 JSON 表达**：`ProceduralKind` 枚举到字符串映射缺失。
- **运行时无可视化编辑器**：状态多时手写易错；编辑器扩展留给 Month 7+ Inspector。

### 12.9 性能
- **AnimatorSystem 内每实体多次 `try_get<Sprite>` / `try_get<AnimEventQueue>`**：未做 cached view + group；千实体规模可考虑改 `view<AnimatorComponent, Sprite>`。
- **每帧扫描所有 transitions**：无加速结构；状态/转移多时 O(N) 线性。
- **GPU sprite 每帧因 procedural 全量重传**：见 §12.4。
- **未做 LOD / 距相机剔除**：远处 entity 也照常推进 FSM、写 GPU slot。

### 12.10 不在本计划范围（保留给后续）
- Blend Tree (1D / 2D)
- Root Motion
- Skeletal / Spine / DragonBones（也是 crossfade 真混合的前提）
- AnimGraph 可视化编辑器
- LOD / 批量 / Job 化
- 网络回放 / 同步

---

## 附：组件/资产清单

| 类型 | 角色 |
|---|---|
| `AnimationClip`（资产） | 帧序列 + 帧事件 |
| `AnimatorController`（资产，shared_ptr） | Base 层 FSM + 额外层数组 |
| `AnimatorComponent`（组件） | 当前播放、参数黑板、各层运行时 |
| `AnimEventQueue`（组件，单帧） | 帧事件 + 状态事件输出口 |
| `Sprite`（组件） | 写回目标：srcRect / texture |
| `AnimatorSystem`（系统） | 每帧驱动 FSM/层/程序化输出 |
| `AnimatorOutput`（组件，单帧重置） | 程序化层输出 (offset/rotation/scaleMul/tintMul) |
| `TweenComponent` / `TweenSystem` | 独立补间，按通道写回 Transform/Sprite/Camera |
| `SpringValue`（工具） | 阻尼弹簧；调用方手动 `update(dt)` |
| `EngineContext.timeScale` | 全局时间缩放 (hit-stop) |
