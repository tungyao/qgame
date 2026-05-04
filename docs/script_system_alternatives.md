# ScriptSystem 替代方案研究（移动端友好）

> 背景：StarEngine 需要兼容 iOS / Android。iOS 禁止 JIT、禁止 dlopen 用户 dll、禁止下载可执行逻辑。所以排除：LuaJIT、Wasmtime/Wasmer JIT、cr.h 用户态 dll 热重载、Mono JIT。
> 目标：不引入"脚本系统"也能实现等价的灵活性 + 热迭代体验，性能不退步。

---

## 引擎当前缺失的系统盘点

`src/engine/systems/` 现有：Input / Physics / Render / Audio / Animator / Tween / UI。
Month 1-8 已完成，引擎本身能跑。做星露谷类游戏还需要补：

**接近必需**
- ScriptSystem（或其替代，本文主题）
- ParticleSystem — 飘血、尘土、雨雪、收割粒子
- LightingSystem / 2D 光照 — 昼夜+室内灯笼，单独 pass
- PathfindingSystem (A* on tilemap) — NPC、动物 AI

**强烈推荐**
- TimeSystem — 游戏时间/日期/季节，独立于 dt
- EventSystem — `entt::dispatcher` 上加一层"延迟触发 / cron-like 日程"
- SaveSystem — SceneSerializer 只存 ECS，存档还需游戏进度版本化

**视项目而定**
- NetworkSystem / WeatherSystem

---

## ScriptSystem 传统方案（移动端约束下的可用性）

| 方案 | iOS 合规 | 性能 | 备注 |
|------|---------|------|------|
| Lua 5.4 解释器 | ✅ | 原生 1/30~1/50 | sol2 嵌入简单，纯解释器 |
| LuaJIT | ❌ | 接近原生 | iOS 禁 JIT，作者基本停更 |
| C# / Mono JIT | ❌ | 接近原生 | iOS 必须 AOT (IL2CPP)，运行时 10MB+ |
| Python | ✅ | 慢 50-100× | 不推荐游戏逻辑用 |
| AngelScript / Squirrel | ✅ | 中等 | 静态类型，生态小 |
| wasm3（解释器） | ✅ | 原生 1/10 | 语言无关，host call 开销大 |
| Wasmtime/Wasmer JIT | ❌ | 80-95% | iOS 禁 JIT |
| cr.h 用户态 dll 热重载 | ❌ | 100% | iOS 禁 dlopen |

iOS 实际可选：**Lua 5.4 / wasm3 / AngelScript**——都是纯解释器。

---

## 推荐方向：不上脚本系统也行

### 方案 A：纯数据驱动（主力）

**核心**：游戏逻辑 = 数据 + 通用 interpreter system，脚本不存在。

**三件套**

1. **行为树（Behavior Tree）**
   - 节点固定：Sequence / Selector / Action / Condition / Decorator
   - Action/Condition 是预注册的 C++ 函数表，节点引用函数 ID
   - JSON 描述结构，运行时 tick
   - 适合：NPC AI、动物、敌人

2. **Schedule / Timeline 表**
   - 星露谷 NPC 日程的本质：`{time:"09:00", action:"move_to", args:[x,y]}` 列表
   - `ScheduleSystem` 按游戏时间驱动表
   - 适合：NPC 日常、剧情触发、定时事件

3. **Event Graph / 状态机**
   - `{state:"idle", on:"player_near", goto:"talk"}` 边表
   - `StateMachineSystem` 通用解释
   - 适合：对话流、任务流、机关、UI 流程

**性能**：零 VM，cache-friendly 数组迭代，比脚本快得多。
**痛点**：表达力上限，过程式逻辑数据化会丑。
**适用范围**：星露谷类游戏 80% 的 NPC/AI/剧情触发器都能 cover。

---

### 方案 B：Lua 5.4 解释器（不带 JIT）

iOS 完全允许——纯解释器，无运行时代码生成。Roblox/Defold 在 iOS 就这么跑。

- 性能：原生 1/30~1/50，**只跑事件回调**完全够用
- 嵌入：sol2 + Lua 5.4 源码，无外部依赖
- 限制：脚本随包发布，不下载——App Store 接受
- **不要把 Lua 放进 update 循环**，只用于 `onInteract / onDialogue / onItemUse`
- 暴露极小 API（只允许调 GameAPI 的 20 个函数），脚本系统压缩到 ~500 行

---

### 方案 C：wasm3（解释器模式）

- 纯解释器，iOS 合规
- 性能约原生 1/10，比 Lua 5.4 快
- 优点：脚本可用 Rust/Zig/C 写，类型安全
- 痛点：每次 host call 几百 ns 开销，迭代 ECS 很伤
- 价值：未来想让 **mod 用 Rust 写** 才考虑，否则不值

---

### 方案 D：编译期注册的 C++ "脚本"（推荐）

**核心**：脚本就是 C++ 函数，通过反射/注册表让它们看起来像脚本——运行时按名字查找、按数据触发。

```cpp
GAME_ACTION("npc.move_to", [](Entity e, float x, float y) {
    auto& tr = ctx.registry.get<Transform>(e);
    // ...
});

GAME_CONDITION("player.has_item", [](Entity e, int itemId) -> bool {
    // ...
});
```

行为树/Schedule/JSON 引用 `"npc.move_to"` 字符串，运行时查表分发。

- **性能**：100% 原生 C++，函数指针调用，比 Lua 快 50×
- **热迭代**：桌面开发期用方案 E，移动端打包就是静态调用
- **移动端**：纯 C++ 编译进 app，不违反 iOS 规则
- 本质：Unreal BlueprintCallable 机制的简化版

---

### 方案 E：桌面热重载 + 移动端静态（开发流程）

**关键洞察**：开发期需要快速迭代，发布到 iOS 反正都要 AOT 编译。

- 桌面（Win/Mac/Linux）开发：cr.h 热重载 C++ dll，几秒看到效果
- 移动端打包：同一份 C++ 代码静态链接进 app
- `#ifdef DEV_HOT_RELOAD` 切换

**结果**：开发体验 ≈ 脚本，发布性能 = 原生，iOS 合规。

---

### 方案 F：C++20 协程 DSL（被低估）

C++20 coroutine 让 C++ 代码长得像脚本：

```cpp
Task npcDayRoutine(Entity npc) {
    co_await waitUntil(9, 0);
    co_await moveTo(npc, shop);
    co_await waitDuration(8h);
    co_await moveTo(npc, home);
    co_await playAnim(npc, "sleep");
}
```

- 写起来像脚本，跑起来是原生 C++
- 配合一个 `CoroutineSystem` 调度
- iOS 合规（C++ 协程是编译期变换）
- 工程已经是 C++20，加 200 行调度器就能用
- 适合：剧情演出、过程式 NPC 行为、复杂状态序列

---

## 推荐组合（移动端友好）

```
┌─ 行为树 + Schedule 表 + 状态机（方案 A）  ← NPC/AI/剧情/任务的 80%
├─ C++ Action 注册表（方案 D）              ← 行为树叶子节点全是这个
├─ C++20 协程 DSL（方案 F）                 ← 复杂过程式逻辑、剧情演出
└─ Lua 5.4（方案 B，可选）                  ← 留给未来 mod 支持，不上也能撑
```

**开发期**：方案 E 让 C++ 桌面端秒级热重载。
**移动端**：全部静态编译，零脚本运行时，过审无忧，性能拉满。

---

## 工作量估计

| 组合 | 引擎层代码量 | 工具链 | 开发期体验 |
|------|------|------|------|
| A+D | ~1500 行 | JSON 编辑器（可后做） | 改数据热加载 |
| A+D+F | ~2500 行 | 同上 | 改协程要重编 |
| A+D+F+E | ~3000 行 | + cr.h 集成 | 桌面秒级热重载 |

**最小可用 = A+D**：行为树 + 状态机 + Schedule + C++ Action 注册表。
星露谷类游戏的 NPC / 作物 / 动物 / 商店 / 剧情触发器全部能 cover。

---

## 待研究 / 未决

- A+D 的最小架构骨架：ISystem 怎么挂、JSON schema 长啥样、Action 注册宏怎么写
- BT 的 tick 频率与 FrameScheduler 集成位置（Step 3 或新增 Step）
- 协程调度器如何与 EnTT dispatcher 协作（co_await 事件）
- 数据热加载：JSON 改了如何不重启 reload BT/Schedule
- Lua 引入时机：MVP 不上，等真有 mod 需求或第三方剧情贡献者再上
