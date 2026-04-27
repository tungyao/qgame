# UI System v2

> 重写时间：2026-04-27
> 涉及文件：
> - `src/engine/components/UIComponents.h`
> - `src/engine/systems/UISystem.{h,cpp}`
> - `src/engine/systems/RenderSystem.cpp`（注入钩子）
> - `src/engine/runtime/EngineContext.cpp`（注册顺序）
> - `src/engine/api/GameAPI.{h,cpp}`
> - `game/main.cpp`（demo）

## 背景：v1 的问题

旧版 UISystem 借用 `Sprite/Transform/TextComponent` 来渲染 UI，导致：

1. **尺寸丢失**：`createButton` 等只 `emplace<Sprite>` 但不写 `srcRect.w/h`，渲染管线又用 `srcRect` 当 quad 大小，结果按钮显示为 0×0 → 不可见。
2. **Slider / ProgressBar 没渲染组件**：根本没创建任何 sprite，自然永远画不出来。
3. **layout 与 render 互相穿透**：`UISystem::computeWorldPosition` 写 `Transform.x/y`，又依赖 `Sprite.pivot` 做对齐——两个独立系统的字段被同步着用，pivot 默认 0.5/0.5 还和 UI 的"左上角原点"约定不一致。
4. **hierarchy 不彻底**：根节点判定靠 `UIParent` 缺失，多 Canvas 时容易混；sortOrder 比较退化成 entity id 大小比较。
5. **API 颗粒度别扭**：要么 5–6 个 `setUI*` 调用堆出一个按钮，要么标签/按钮分两个 entity 并行管理。

## v2 总体思路

1. **单一布局节点 `UINode`**：把布局参数、计算结果、交互状态全收拢在一个组件里。
2. **视觉/功能组件按需附加**：`UIBackground / UIButton / UIToggle / UISlider / UIProgressBar / UIImage / UILabel / UIDraggable`，UI 实体 = `UINode` + 0..N 个特性组件。
3. **独立渲染路径**：UISystem 自己产出 `DrawSpriteCmd / DrawTextCmd`（`pass = Screen`）缓存到本帧命令队列，由 RenderSystem 在 CPU 与 GPU-driven 两条路径里注入。**不再借用 Sprite/Transform/TextComponent**。
4. **世界空间锚点 `UIWorldAnchor`**：UI 跟随某个含 `Transform` 的世界实体，每帧投影到屏幕像素，但仍以**屏幕像素尺寸**渲染——典型应用是头顶血条 / 名字。
5. **1×1 白纹理 + tint** 实现纯色矩形，未来要换成带纹理 / 九宫格只需在 `emitRect` 里加分支，不破坏现有 API。

## 组件模型

```
UICanvas (root)               // 屏幕坐标空间
  └─ UINode (parent=canvas)   // 子节点 parent 字段指向父节点（可以是另一个 UINode 或 UICanvas）
       ├─ UIBackground / UIButton / UIToggle / UISlider /
       │  UIProgressBar / UIImage / UILabel  (按需附加)
       ├─ UIDraggable        (可选，让节点可拖拽)
       └─ UIWorldAnchor      (可选，让节点跟随世界实体)
```

### UINode 字段
| 字段 | 含义 |
| ---- | ---- |
| `parent` | 父节点（`UINode` 或 `UICanvas` 实体；`entt::null` 表示游离） |
| `width / height` | 期望尺寸（像素） |
| `pivotX / pivotY` | 中心点（0..1） |
| `anchor` | `UIAnchor{minX,minY,maxX,maxY}`，min==max 固定锚点，min!=max 拉伸 |
| `offsetX / offsetY` | 锚点中心相对偏移（像素，y-down） |
| `visible / interactable / sortOrder` | 状态 |
| `screenX/Y/W/H` | 由 UISystem 每帧计算（屏幕像素，矩形左上角） |
| `hovered / pressed` | 交互状态（UISystem 维护） |

### 锚点预设（`UIAnchor` 静态工厂）
`topLeft / topCenter / topRight / centerLeft / center / centerRight / bottomLeft / bottomCenter / bottomRight / stretch`

## UISystem 工作流

每帧 `update()`：

1. **首帧**：通过 `renderDevice.createTexture` 上传 1×1 白纹理。
2. **Canvas 缩放**：根据 `referenceWidth/Height` 与 `screenW/H` 计算 `scaleFactor`（暂未驱动布局，预留接入）。
3. **`runLayout()`**：自每个 Canvas 出发，DFS 遍历以 Canvas 为 parent 的根节点，再递归子节点；
   - 普通节点：`screenX/Y = anchorCenter + offset - pivot * size`
   - 含 `UIWorldAnchor` 的节点：通过世界相机参数 `(camX, camY, zoom)` 把目标实体的 Transform 投影到屏幕，作为本节点的 anchor 中心（**仍按 pivot/offset 决定最终位置**）。
4. **`runInteraction(input)`**：
   - 屏幕命中测试：取 sortOrder 最大的可见可交互节点为 hovered。
   - 按下：抓取 pressed 节点，触发 `UIButton::onDown`，初始化 `UIDraggable.grabOffset`，标记 `UISlider::dragging`。
   - 持续按下：滑条按指针 X 实时计算 `value` + 触发 `onChanged`；拖拽元素直接更新 `offsetX/Y`（`makeDraggable` 已强制 anchor=topLeft + pivot=(0,0)）。
   - 抬起：触发 `UIButton::onClick / onUp`、翻转 `UIToggle::isOn`、`UISlider::onReleased`、`UIDraggable::onDragEnd`。
5. **`buildCommands()`**：按 sortOrder 升序遍历可见节点，每个节点的视觉/功能组件各自产出若干 `DrawSpriteCmd / DrawTextCmd`：
   - **Slider**：track 全宽 + fill 按 value 截断 + handle 居中
   - **ProgressBar**：bg + fill（支持四个方向）
   - **Toggle**：track + knob（左/右两态）
   - **Button**：根据 `pressed/hovered/disabled` 选色，单矩形
   - **Background / Image**：单矩形（带 / 不带纹理）
   - **Label**：`DrawTextCmd`，pass=Screen

## RenderSystem 注入点

UI 命令用 `RenderPass::Screen`，由 UI 相机的 `layerMask` 过滤。两条路径的注入位置：

```cpp
// CPU 路径：buildSceneCommands 末尾
if (ctx.systems.has<UISystem>()) {
    ctx.systems.get<UISystem>().emitDrawCommands(cb);
}

// GPU-driven 路径：per-camera Step 2 (textCommands 收集后)
if (ctx_.systems.has<UISystem>()) {
    std::vector<const backend::RenderCmd*> uiPtrs;
    ctx_.systems.get<UISystem>().appendDrawCommandPtrs(uiPtrs);
    for (const backend::RenderCmd* p : uiPtrs) {
        const RenderPass pp = cmdPass(*p);
        if ((cam.layerMask & renderPassBit(pp)) == 0) continue;
        textCommands.push_back(*p);
    }
}
```

注册顺序在 `EngineContext.cpp` 调整为 `UISystem` 先于 `RenderSystem` 注册，保证 UI 本帧布局/命令在 RenderSystem 读取前已就绪。

## 坐标约定

- UISystem 内部 / 公开接口：**屏幕像素，y-down，矩形左上角**。`UINode::screenX/Y/W/H` 与 `getUIComputedRect` 都按这个语义。
- emit 阶段把屏幕坐标 `(sx, sy)` 转成 Screen 相机的 world 坐标 `(sx - vpW/2, sy - vpH/2)`。这是因为 backend 的相机投影写死了 `(world - cam.xy) * zoom + viewport/2`，UI 相机位于 `(0,0)` 且 zoom=1，反向抵消即可让屏幕坐标"原样"显示。

## API 速查（`GameAPI`）

```cpp
// Canvas
entt::entity canvas = api.createCanvas(1280, 720);
api.setCanvasScaleMode(canvas, true /*scaleWithScreen*/);
api.setCanvasSafeArea(canvas, l, t, r, b);

// 通用节点
entt::entity n = api.createUIElement(/*parent*/ canvas);
api.setUISize(n, w, h);
api.setUIAnchor(n, minX, minY, maxX, maxY);
api.setUIOffset(n, x, y);                  // 注意 v2 改为 2 参数
api.setUIPivot(n, px, py);
api.setUIInteractable(n, true);
api.setUIVisible(n, true);
api.setUISortOrder(n, 10);
api.setUIBackground(n, color, texture);    // 给任意节点叠纯色/纹理背景
api.attachToWorld(n, target, offX, offY);  // 跟随世界实体
api.detachFromWorld(n);

// 控件工厂（自动创建 UINode + 对应组件）
auto btn   = api.createButton(w, h, [](){...});
auto tog   = api.createToggle(w, h, [](bool on){...});
auto sld   = api.createSlider(w, h, min, max, [](float v){...});
auto bar   = api.createProgressBar(w, h);
auto img   = api.createUIImage(w, h, texture);
auto label = api.createUIText(w, h, "Hello");

api.setButtonColors(btn, normal, hover, pressed);
api.setSliderValue(sld, 50.f); api.setSliderRange(sld, 0, 100);
api.setProgressValue(bar, 0.5f);
api.setUITextFont(label, font, 18.f);
api.setUITextAlignment(label, /*0=L,1=C,2=R*/ 1);

// 拖拽（自动设 anchor=topLeft + pivot=(0,0)，offset 直接当屏幕坐标）
api.makeDraggable(img, [](float x, float y){...});
api.setDragBounds(img, 0, 0, 1280, 720);

// 状态查询
entt::entity hovered = api.getHoveredUI();
entt::entity pressed = api.getPressedUI();
float x,y,w,h; api.getUIComputedRect(node, &x, &y, &w, &h);
```

## 世界空间 UI 范例（demo 中的玩家血条）

```cpp
auto playerHealthBar = api.createProgressBar(60.f, 8.f);
api.setUIParent(playerHealthBar, canvas);
api.setUIPivot(playerHealthBar, 0.5f, 1.f);              // pivot 在底边中心
api.attachToWorld(playerHealthBar, player, 0.f, -32.f);  // 在 player 头顶 32px
api.setProgressColors(playerHealthBar,
    {30,30,30,220}, {220,60,60,255});

// 每帧
api.setProgressValue(playerHealthBar, currentHp / maxHp);
```

UISystem 会在 `runLayout()` 里：
1. 取 `player.Transform.x/y` + `(0, -32)` 当作世界坐标。
2. 用第一个 `layerMask & renderPassBit(World)` 的 primary 相机做投影，得到屏幕像素位置。
3. 按 `pivot=(0.5, 1)` + `offset` 决定矩形左上角，再走正常的 emit 流程渲染。

→ UI 在屏幕上以 60×8 像素呈现（不会被世界相机的 zoom 拉伸），但跟着角色移动。

## 从 v1 迁移备忘

| v1 | v2 |
| -- | -- |
| `engine::UIElement` | `engine::UINode` |
| `engine::Canvas` | `engine::UICanvas` |
| `engine::Button / Slider / ...` | `engine::UIButton / UISlider / ...`（同名前缀加 `UI`） |
| `setUIOffset(e, l, t, r, b)`（4 参数） | `setUIOffset(e, x, y)`（2 参数） |
| 通过 `Sprite` 间接渲染 | 由 UISystem 直接 emit 命令 |
| Slider/ProgressBar 不渲染 | 完整渲染 track/fill/handle 等 |

## 后续可扩展点

- 九宫格切片背景：在 `emitRect` 里增加 NineSlice 分支。
- 文本对齐：当前 `halign=Center/Right` 用 `text.size() * fontSize * 0.55` 估算文本宽，等 FontData 暴露字宽度后切到精确版本。
- Canvas `scaleFactor` 真正驱动布局缩放（目前已计算但未应用）。
- UI 命令排序：跨节点的 `sortOrder` 已稳定，但 ySort/层级穿插仍走 backend 默认顺序，必要时在 `buildCommands` 里加二次排序。
- 焦点 / 键盘导航：`UISystem` 已预留 `pressed_/hovered_`，可加 `focused_` + Tab 序遍历。
