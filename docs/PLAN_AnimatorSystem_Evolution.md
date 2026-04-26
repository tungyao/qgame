# AnimatorSystem 演进计划

> 当前状态：`src/engine/systems/AnimatorSystem.cpp` 仅支持单 clip 顺序播放 + per-frame duration + 完成标记
> 本阶段目标：从"播帧器"演进为可支撑动作/RPG 玩法的动画运行时

---

## 一、当前能力 / 缺口

### 已有
- 单 AnimatorComponent 持有 currentAnim、time、speed、playing、finished
- 按 frame.duration 累加定位、loop / one-shot
- 写回 Sprite.srcRect（可选 texture 切换）

### 缺口
- 无打断 / 优先级，切 clip 全靠外部直接赋值
- 无状态机，玩法层手动管理 "什么时候该播哪个"
- 切 clip 是硬切，没有过渡 / 淡入淡出
- 无回调，命中帧、脚步、特效需要外部按时间猜
- 无参数系统，gameplay 与动画强耦合
- 单层播放，无法上下半身分离
- 无程序化动画（tween / spring / squash），UI 反馈和细节表现都得手写

---

## 二、阶段划分

按依赖与价值排序，分 5 个 Phase。每个 Phase 独立可发布、可回滚。

### Phase 1：打断与优先级（基础防御）
**目标**：让"播什么"成为有规则的请求，而不是赋值。

- AnimatorComponent 增加：`priority`、`interruptible`、`queuedAnim`
- 提供 API：`play(clip, opts)`、`crossfade(clip, opts)`、`queue(clip)`、`stop()`
- `opts`：priority、forceRestart、startTime、playMode（Once/Loop/PingPong）
- 当前 clip 不可打断 + 新请求优先级不够 → 入队或丢弃
- 单元测试：相同 clip 重复请求不重置、低优先级被高优先级覆盖、queued 在 finished 后接续

**交付**：`AnimatorComponent.h` 扩展、`AnimatorSystem` 增加请求处理段、`Animator` facade（玩法层调用）

---

### Phase 2：帧事件（Animation Notifies）
**目标**：把"第 N 帧触发什么"从 gameplay tick 里搬到 clip 数据里。

- `AnimationClip` 增加 `events: vector<AnimEvent>`：`{ time, name, intParam, floatParam, stringParam }`
- 常用 name 约定：`hitbox_on` / `hitbox_off` / `footstep` / `sfx` / `vfx` / `damage_window_start` / `damage_window_end`
- `AnimatorSystem::update` 推进 time 时，扫过区间 `[prevTime, time)` 触发事件，loop 跨越 duration 时分两段扫
- 派发方式：写入 `AnimEventQueue` 组件（per-entity），由其他 system 在同帧消费 —— 避免 system 间直接回调耦合
- 反向 / 速度 0 / 跳帧 / pause 都要正确（不重复、不漏）

**交付**：`AnimationClip` schema 扩展（asset loader 同步）、`AnimEventQueue` 组件、消费侧示例（HitboxSystem 监听 `hitbox_on/off`）

---

### Phase 3：参数与状态机（FSM）
**目标**：动画选择由参数驱动，玩法只写参数，不写"播哪个 clip"。

#### 3.1 参数黑板
- AnimatorComponent 持有 `parameters: map<string, Variant>`，类型：float / int / bool / trigger
- trigger：消费一次后自动复位
- 提供 setFloat / setBool / setTrigger API

#### 3.2 状态机
- `AnimatorController` 资产：`states[]`、`transitions[]`、`defaultState`
- State：name、clip、speed、loop、events 覆盖
- Transition：`from`、`to`、`conditions[]`（参数比较）、`hasExitTime`、`exitTime`、`duration`（过渡时长）、`interruptible`
- 支持 `Any State` 转移（任意状态可进入，例：受击）
- `AnimatorComponent` 持有 `controller` 引用 + `currentState` + `transitionState`

#### 3.3 过渡 (Crossfade)
- 过渡期间同时推进 from / to clip 的 time，按 t/duration 做权重
- 渲染：先帧到 srcRect 切换 + alpha 混合（2D sprite 通常做不了真混合，过渡多用快速淡入或直接帧切；保留接口给后续 skeletal）
- 默认策略：sprite 模式过渡时间内做"加速 from 到末尾 + 立即开始 to"或简单硬切，配置可选

#### 3.4 回调（与 Phase 2 帧事件互补）
- 状态级：`onEnter` / `onExit` / `onUpdate` 通过 AnimEventQueue 派发同样的 name 约定（`state_enter:Run` 等）
- finished：仍由 AnimatorComponent.finished 标记，状态机额外发 `state_finished`

**交付**：`AnimatorController` 资产 + JSON 定义、`AnimatorSystem` 转移评估段、参数 API、最小示例（idle ⇄ walk ⇄ attack）

---

### Phase 4：动画分层 (Layers)
**目标**：上半身/下半身、受击抖动、瞄准偏移可独立播放。

- AnimatorComponent.controller 改为 `layers[]`：每层一套独立 FSM + 当前 state + time
- Layer 属性：`name`、`weight`（0..1）、`blendMode`（Override / Additive）、`mask`（2D 下用 channel 集合，例如 `{srcRect_only}`、`{transformOffset_only}`、`{tint_only}`）
- 写回阶段按 layer 顺序合成：base layer 写完，上层根据 mask 覆盖对应通道
- 2D 实用 mask 通道：`Sprite.srcRect`、`Sprite.tintColor`、`Transform.localOffset`、`Transform.rotationOffset`、`Transform.scaleOffset`
- Additive 用法：受击红闪叠 tint、命中位移叠 offset

**交付**：Layer 数据结构、合成顺序、示例（base 走路 + 上层挥剑 srcRect 覆盖；或 base idle + additive 受击红闪）

> 注：纯帧动画做"上半身换 srcRect"需要美术拆图；若不拆图，分层主要价值在 Tint/Offset/Rotation/Scale 通道（程序化层）。这与 Phase 5 天然衔接。

---

### Phase 5：程序化动画 (Procedural)
**目标**：补充帧动画做不了的连续反馈：缓动、弹性、抖动、squash & stretch。

#### 5.1 Tween 组件 / 模块
- `Tween<T>`：from、to、duration、easing、loop、pingpong、onComplete
- 目标通道：Transform.position/scale/rotation、Sprite.tintColor/alpha、Camera.zoom
- API：`tween(entity).to(...).easing(EaseOutCubic).start()`，链式
- Easing 表：Linear / Quad / Cubic / Quart / Sine / Expo / Back / Elastic / Bounce × In/Out/InOut

#### 5.2 Spring / Damper
- `SpringValue`：当前值、速度、目标、stiffness、damping
- 用途：相机跟随、UI 弹入、采集物吸附、命中位移回弹
- 每帧 `update(dt, target)`，输出当前值

#### 5.3 Procedural Modifier 作为 Animator Layer
- 提供内置 layer 类型：`SquashStretchOnLand`、`HitShake`、`BreatheBob`、`HurtFlash`
- 这些 layer 不需要 clip，直接对 Transform/Sprite 通道写偏移
- 可通过参数（速度、blend weight）控制强度

#### 5.4 时间控制 (附带)
- 全局 `timeScale` + 单体 `timeScale`（hit-stop 顿帧）
- AnimatorComponent.localTimeScale，update 用 `dt * global * local`
- 反向播放（speed < 0）、ping-pong 模式

**交付**：`TweenSystem`、`SpringValue` 工具、4 个内置程序化 layer、hit-stop 示例

---

## 三、数据格式与资产改动

### AnimationClip JSON 扩展
```json
{
  "name": "attack_01",
  "texture": "char/hero.png",
  "loop": false,
  "frames": [{ "srcRect": [...], "duration": 0.05 }],
  "events": [
    { "time": 0.10, "name": "hitbox_on" },
    { "time": 0.18, "name": "hitbox_off" },
    { "time": 0.05, "name": "sfx", "string": "swing" }
  ]
}
```

### AnimatorController JSON
```json
{
  "parameters": [
    { "name": "speed", "type": "float", "default": 0 },
    { "name": "attack", "type": "trigger" }
  ],
  "layers": [
    {
      "name": "Base",
      "weight": 1.0,
      "blendMode": "Override",
      "mask": ["srcRect", "texture"],
      "defaultState": "Idle",
      "states": [
        { "name": "Idle", "clip": "hero_idle", "loop": true },
        { "name": "Walk", "clip": "hero_walk", "loop": true },
        { "name": "Attack", "clip": "hero_attack_01", "loop": false }
      ],
      "transitions": [
        { "from": "Idle", "to": "Walk", "conditions": [["speed", ">", 0.1]], "duration": 0.05 },
        { "from": "Walk", "to": "Idle", "conditions": [["speed", "<", 0.1]], "duration": 0.05 },
        { "from": "AnyState", "to": "Attack", "conditions": [["attack", "trigger"]], "duration": 0.0 },
        { "from": "Attack", "to": "Idle", "hasExitTime": true, "exitTime": 1.0 }
      ]
    },
    {
      "name": "Hurt",
      "weight": 0.0,
      "blendMode": "Additive",
      "mask": ["tint"],
      "procedural": "HurtFlash"
    }
  ]
}
```

---

## 四、组件 / 系统改动概览

| 模块 | 改动 |
| --- | --- |
| `AnimatorComponent` | 增加 controller、参数表、每层运行时状态、queuedAnim、priority、localTimeScale |
| `AnimationClip` | 增加 events |
| `AnimatorController`（新） | 资产，layers + states + transitions + parameters |
| `AnimEventQueue`（新组件） | 当帧事件队列，供其他 system 消费 |
| `AnimatorSystem` | 拆为：参数评估 → 转移评估 → 时间推进 → 事件扫描 → 多层合成写回 |
| `TweenSystem`（新） | 独立系统，不依赖 Animator |
| `AssetManager` | 注册 AnimatorController 加载 |
| `Animator` facade（新） | 玩法层 API：setFloat/setBool/setTrigger/play/crossfade/stop |

---

## 五、里程碑与验收

| Phase | 验收用例 |
| --- | --- |
| 1 | 攻击中再次按攻击不打断；受击高优先级覆盖攻击；queued idle 在 attack 完成后自动播放 |
| 2 | 攻击 clip 在第 5 帧生成 hitbox、第 9 帧关闭，HitboxSystem 接到事件；loop clip 跨 duration 边界事件不丢不重 |
| 3 | gameplay 只写 `setFloat("speed", v)` 与 `setTrigger("attack")`，无任何 play() 调用；AnyState→Hurt 可从任意状态进入 |
| 4 | 受击红闪 layer 启用时角色仍正常走路；关掉 layer 走路无残留 |
| 5 | 拾取物 spawn 用 EaseOutBack 缩放弹入；命中触发 0.05s timeScale=0.05 顿帧；落地 squash 0.1s 回弹 |

---

## 六、风险与取舍

- **Sprite 真过渡受限**：2D 帧动画无法逐像素混合，crossfade 在 sprite 模式下退化为快速切换 + tint/alpha 过渡；真混合留给后续骨骼/Spine 接入。
- **状态机数据规模**：手写 JSON 在状态多时易错，编辑器可视化留给 Month 7+ 的 Inspector 扩展。
- **事件派发顺序**：跨 layer / 跨 system 的事件顺序需要明确（建议：所有 layer 推进完再统一消费 AnimEventQueue）。
- **回放 / 网络同步**：当前不考虑，但 controller + 参数 + 时间的设计天然适合后续做 replication。

---

## 七、不在本计划范围（保留给后续）

- Blend Tree（1D / 2D）
- Root Motion
- Skeletal / Spine / DragonBones
- AnimGraph 可视化编辑器
- LOD / 批量 / Job 化
- 网络同步
