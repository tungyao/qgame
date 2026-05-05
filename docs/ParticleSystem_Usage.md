# ParticleSystem 使用文档

> 覆盖当前 GPU 粒子实现：CPU 发射初始粒子，GPU compute 更新生命周期/位置，GPU compact alive list，GPU sort，SDL_GPU indirect draw。
> 源码：`src/engine/components/ParticleComponent.h`、`src/engine/resources/GPUParticleRenderer.*`、`src/backend/renderer/sdl_gpu/SDLGPURenderDevice.*`、`assets/shaders/particle_*.hlsl`。

---

## 目录

1. [核心流程](#1-核心流程)
2. [组件挂载](#2-组件挂载)
3. [完整示例](#3-完整示例)
4. [运行内置 demo](#4-运行内置-demo)
5. [字段说明](#5-字段说明)
6. [渲染后端行为](#6-渲染后端行为)
7. [Shader 与构建约定](#7-shader-与构建约定)
8. [限制与注意事项](#8-限制与注意事项)

---

## 1. 核心流程

粒子系统走 SDL_GPU 主路径，OpenGL 不做粒子优化：

```text
ParticleComponent + Transform
    ↓
RenderSystem::syncParticleEmitters(dt)
    ↓
GPUParticleRenderer::emit()
    ↓
CPU 上传新生粒子的初始状态到 GPUParticle storage buffer
    ↓
SDLGPURenderDevice::submitGPUParticlePass()
    ↓
particle_update.comp.hlsl
    - 推进 age / position
    - 写 AliveIndices
    - 写 indirect draw args
    ↓
particle_sort.comp.hlsl
    - 对 alive list 做 GPU 排序
    ↓
particle_gpu.vert.hlsl / particle_gpu.frag.hlsl
    - 通过 AliveIndices 读取粒子
    - indirect draw 绘制 soft sprite quad
```

CPU 负责“发射新粒子”的初始状态，比如位置、速度、生命周期、颜色、UV。GPU 负责每帧模拟、筛出活跃粒子、排序和绘制。

---

## 2. 组件挂载

粒子实体至少需要：

- `Transform`：发射器位置和方向
- `ParticleComponent`：发射参数和纹理

头文件：

```cpp
#include <engine/components/RenderComponents.h>
#include <engine/components/ParticleComponent.h>
```

最小挂载：

```cpp
auto emitter = api.spawnEntity();

engine::Transform tf{};
tf.x = 520.f;
tf.y = 260.f;
tf.rotation = -1.35f;
api.addComponent(emitter, tf);

engine::ParticleComponent particles{};
particles.texture = particleTex;
particles.srcRect = { 0.f, 0.f, 32.f, 32.f };
particles.maxParticles = 256;
particles.emissionRate = 180.f;
particles.lifetime = 1.25f;
particles.speedMin = 35.f;
particles.speedMax = 160.f;
particles.sizeStart = 18.f;
particles.sizeEnd = 2.f;
particles.spread = 1.15f;
particles.colorStart = { 255, 245, 140, 230 };
particles.colorEnd = { 255, 80, 40, 0 };
particles.layer = 8;
particles.ySort = true;
particles.pass = engine::RenderPass::World;
api.addComponent(emitter, particles);
```

---

## 3. 完整示例

下面示例创建一张程序化 soft sprite 纹理，然后创建一个会摆动方向的粒子发射器。

```cpp
static std::vector<uint8_t> makeParticleTexture(int size) {
    std::vector<uint8_t> pixels(size * size * 4, 0);
    const float center = (size - 1) * 0.5f;
    const float radius = center > 0.f ? center : 1.f;

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float dx = (x - center) / radius;
            const float dy = (y - center) / radius;
            const float d = std::sqrt(dx * dx + dy * dy);
            const float coreGlow = std::max(0.f, 1.f - d);
            const float softEdge = coreGlow * coreGlow;
            const int i = (y * size + x) * 4;

            pixels[i + 0] = 255;
            pixels[i + 1] = 220;
            pixels[i + 2] = static_cast<uint8_t>(80 + 120 * softEdge);
            pixels[i + 3] = static_cast<uint8_t>(255 * softEdge);
        }
    }
    return pixels;
}

auto particlePx = makeParticleTexture(32);
TextureHandle particleTex = api.createTextureFromMemory(particlePx.data(), 32, 32);

entt::entity emitter = api.spawnEntity();
api.addComponent(emitter, engine::Transform{ 520.f, 260.f, -1.35f });

engine::ParticleComponent pc{};
pc.texture = particleTex;
pc.srcRect = { 0.f, 0.f, 32.f, 32.f };
pc.maxParticles = 256;
pc.emissionRate = 180.f;
pc.lifetime = 1.25f;
pc.speedMin = 35.f;
pc.speedMax = 160.f;
pc.sizeStart = 18.f;
pc.sizeEnd = 2.f;
pc.spread = 1.15f;
pc.colorStart = { 255, 245, 140, 230 };
pc.colorEnd = { 255, 80, 40, 0 };
pc.layer = 8;
pc.ySort = true;
pc.pass = engine::RenderPass::World;
api.addComponent(emitter, pc);

// 在游戏 update 中摆动发射方向。
particleDemoT += dt;
api.patchComponent<engine::Transform>(emitter, [&](engine::Transform& tf) {
    tf.rotation = -1.35f + std::sin(particleDemoT * 1.8f) * 0.65f;
    tf.x = 520.f + std::cos(particleDemoT * 0.9f) * 42.f;
    tf.y = 260.f + std::sin(particleDemoT * 1.1f) * 24.f;
});
```

---

## 4. 运行内置 demo

当前 `game/main.cpp` 已包含一个 GPU 精灵粒子 demo。构建并运行：

```powershell
cmake --build build --config Debug -j 4
.\build\game\Debug\game.exe
```

启动后主相机附近会看到一束持续喷射的发光 soft sprite 粒子。该 demo 使用程序化纹理，不依赖额外图片资产。

---

## 5. 字段说明

### 纹理与生命周期

```cpp
TextureHandle texture;
core::Rect    srcRect;
uint32_t      maxParticles = 256;
float         emissionRate = 64.f;
float         lifetime     = 1.f;
```

- `texture`：粒子使用的 sprite 纹理。
- `srcRect`：纹理内像素区域。
- `maxParticles`：该 emitter 的 GPU 槽位数量。
- `emissionRate`：每秒发射粒子数。
- `lifetime`：单个粒子存活秒数。

### 速度、大小和扩散

```cpp
float speedMin  = 20.f;
float speedMax  = 80.f;
float sizeStart = 8.f;
float sizeEnd   = 0.f;
float spread    = 6.28318530718f;
```

- 粒子初始方向以 `Transform::rotation` 为中心。
- `spread` 是随机散布角度，单位为弧度。
- `sizeStart` / `sizeEnd` 由 shader 按生命周期线性插值。

### 颜色

```cpp
core::Color colorStart = core::Color::White;
core::Color colorEnd   = core::Color{255, 255, 255, 0};
```

颜色在 GPU vertex shader 中按 `age / lifetime` 插值。常见淡出写法是 `colorEnd.a = 0`。

### 渲染排序

```cpp
int        layer = 0;
int        sortOrder = 0;
bool       ySort = false;
RenderPass pass = RenderPass::World;
```

GPU sort 当前按以下规则排序：

1. `layer`
2. `ySort` 开关
3. `pos.y`
4. `sortKey`
5. 粒子索引

---

## 6. 渲染后端行为

### SDL_GPU

SDL_GPU 是粒子系统的主实现路径：

- 创建 `GPUParticle` storage buffer
- 创建 alive index storage buffer
- 创建 indirect args buffer
- compute 更新粒子状态
- compute 生成 alive list
- compute 排序 alive list
- indirect draw 绘制活跃粒子

### OpenGL

OpenGL 后端目前只是兼容 fallback：

- 不执行 GPU 粒子 pass
- 不做粒子优化
- 不保证能看到 `ParticleComponent`

粒子效果应以 SDL_GPU 后端测试。

---

## 7. Shader 与构建约定

粒子 shader 只维护 HLSL 源文件：

```text
assets/shaders/particle_update.comp.hlsl
assets/shaders/particle_sort.comp.hlsl
assets/shaders/particle_gpu.vert.hlsl
assets/shaders/particle_gpu.frag.hlsl
```

CMake 会自动生成：

- SPIR-V：Vulkan / SDL_GPU 跨平台路径
- DXIL：D3D12 路径
- C++ 嵌入头：`build/generated/shaders/*.h`

不要为粒子系统手写 `.glsl` 源；需要改 shader 时改 HLSL。

---

## 8. 限制与注意事项

- 当前 emitter 使用单纹理绘制；多纹理粒子建议先拆多个 emitter。
- CPU 仍负责发射新粒子的初始数据；完全 GPU emitter / GPU free-list 尚未实现。
- GPU sort 当前使用正确性优先的 odd-even sort，适合验证闭环；超大粒子数建议后续替换为 bitonic 或 radix sort。
- `maxParticles` 会占用全局粒子池槽位，当前池大小为 `GPUParticleRenderer::MAX_PARTICLES`。
- `ParticleComponent` 内的 `gpuOffset`、`gpuCount`、`accumulator`、`seed` 是运行时状态，场景序列化时应作为内部状态处理。
- 多相机渲染时，RenderSystem 只在第一个相机推进 `dt`，后续相机只绘制，避免同一帧重复模拟。

