# StarEngine — 架构设计文档

> 类星露谷物语 2D 游戏引擎  
> 语言：C++20 | ECS：EnTT | 渲染：SDL3 GPU API | 窗口/输入：SDL3

---

## 一、设计原则

1. **Backend 统一 Frame Contract** — 所有 backend 子系统实现同一接口，由 FrameScheduler 统一驱动
2. **Engine 唯一 Runtime Owner** — backend 完全私有，game/editor 无法直接访问
3. **Editor 不触达 Backend** — editor 只通过 EditorAPI 间接操作引擎
4. **Command-Based 渲染与音频** — engine 不直接调 GPU/DSP API，只提交命令
5. **Audio 异步** — audio 属于 async backend system，不强绑帧循环

---

## 二、层级总览

```
┌─────────────────────────────────────────────────────┐
│  editor/              (BUILD_EDITOR only)            │
│  game/                                               │
├─────────────────────────────────────────────────────┤
│  engine/                                             │
│    api/       GameAPI  │  EditorAPI                  │
│    runtime/   EngineContext │ SystemRegistry          │
│               FrameScheduler                         │
│    systems/   RenderSystem  │ AudioSystem             │
│               PhysicsSystem │ InputSystem             │
│               ScriptSystem                           │
├─────────────────────────────────────────────────────┤
│  backend/             [完全私有，仅 engine 可 include] │
│    renderer/  CommandBuffer │ RenderGraph             │
│               IRenderDevice                          │
│    audio/     AudioCommandQueue │ AudioThread         │
│               IAudioDevice                           │
│    shared/    ResourceHandle │ Fence │ Sync           │
├─────────────────────────────────────────────────────┤
│  platform/    Window │ InputRawEvent                 │
│               FileSystem │ ThreadPrimitive            │
├─────────────────────────────────────────────────────┤
│  core/        Math │ Handles │ Containers            │
│               Logger │ Memory Allocator              │
└─────────────────────────────────────────────────────┘
```

### 依赖方向（单向，禁止反向）

```
core → platform → backend → engine → game
                                   → editor
```

| 模块 | 可依赖 | 禁止依赖 |
|------|--------|---------|
| core | 无 | 所有 |
| platform | core | backend 以上 |
| backend | core, platform | engine, game, editor |
| engine | core, platform, backend | game, editor |
| game | engine 及以下 | editor |
| editor | 所有 | — |

---

## 三、core 层

```
core/
├── math/
│   ├── Vec2.h          # float x,y + 运算符重载
│   ├── Vec3.h
│   ├── Mat3.h          # 2D 变换矩阵
│   └── Rect.h          # AABB，用于碰撞和 UV
├── memory/
│   ├── Arena.h         # 帧内线性分配器，帧末一次性重置
│   └── PoolAllocator.h # 固定大小对象池（Component 用）
├── containers/
│   ├── HandleMap.h     # Handle→T 映射，避免裸指针
│   └── RingBuffer.h    # AudioCommandQueue 底层
├── Handle.h            # 通用 Handle<Tag,T>，含版本号防悬挂
├── Logger.h            # 分级日志，release 下 strip
└── Assert.h
```

### Handle 设计（防悬挂指针）

```cpp
template<typename Tag, typename T = uint32_t>
struct Handle {
    T index   : 20;  // 最多 1M 个对象
    T version : 12;  // 版本号，检测 use-after-free
    bool valid() const { return index != 0; }
};

using TextureHandle = Handle<struct TextureTag>;
using SoundHandle   = Handle<struct SoundTag>;
using EntityHandle  = Handle<struct EntityTag>;
```

---

## 四、platform 层

```
platform/
├── Window.h            # 创建窗口，获取 native handle 给 bgfx
├── InputRawEvent.h     # SDL 原始事件 → 平台无关结构
├── FileSystem.h        # 跨平台路径、异步读文件
└── Thread.h            # 封装 std::thread + 信号量
```

### 输入统一抽象（触摸/鼠标/键盘）

```cpp
struct InputRawEvent {
    enum Type {
        POINTER_DOWN, POINTER_MOVE, POINTER_UP,  // 统一触摸和鼠标
        KEY_DOWN, KEY_UP,
        GAMEPAD_BUTTON, GAMEPAD_AXIS
    };
    Type    type;
    int     pointerId;   // 多点触摸 ID，鼠标固定为 0
    float   x, y;        // 归一化坐标 [0,1]
    int     keyCode;
};
```

---

## 五、backend 层

> **所有头文件 CMake PRIVATE，game/editor 无法 include**

```
backend/
├── shared/
│   ├── ResourceHandle.h    # GPU 资源句柄（Texture/Buffer/Shader）
│   ├── Fence.h             # CPU-GPU 同步
│   └── Sync.h              # 线程同步原语
├── renderer/
│   ├── IBackendSystem.h    # 统一生命周期接口
│   ├── IRenderDevice.h     # 渲染设备抽象
│   ├── CommandBuffer.h     # 渲染命令列表
│   ├── RenderGraph.h       # （可选）依赖图自动barrier
│   ├── opengl/
│   │   └── GLRenderDevice.cpp
│   ├── vulkan/
│   │   └── VkRenderDevice.cpp
│   └── metal/
│       └── MetalRenderDevice.cpp
└── audio/
    ├── IAudioDevice.h      # 音频设备抽象
    ├── AudioCommandQueue.h # 无锁/有锁环形队列
    ├── AudioThread.h       # 独立消费线程
    └── sdl_mixer/
        └── SDLAudioDevice.cpp
```

### IBackendSystem（Frame Contract 接口）

```cpp
class IBackendSystem {
public:
    virtual ~IBackendSystem() = default;
    virtual void init()       = 0;
    virtual void beginFrame() = 0;  // 帧开始，重置状态
    virtual void endFrame()   = 0;  // 帧结束，flush 命令
    virtual void shutdown()   = 0;
};
```

### IRenderDevice

```cpp
class IRenderDevice : public IBackendSystem {
public:
    // 资源管理
    virtual TextureHandle createTexture(const TextureDesc&)  = 0;
    virtual void          destroyTexture(TextureHandle)       = 0;
    virtual ShaderHandle  createShader(const ShaderDesc&)    = 0;

    // 提交命令
    virtual void submitCommandBuffer(const CommandBuffer&)   = 0;
    virtual void present()                                   = 0;

    // 编辑器用：渲染到离屏纹理
    virtual TextureHandle renderToTexture(const CommandBuffer&, int w, int h) = 0;
};
```

### CommandBuffer

```cpp
struct DrawSpriteCmd {
    TextureHandle texture;
    float x, y, rotation, scaleX, scaleY;
    Rect  srcRect;
    int   layer;
    Color tint;
};

struct DrawTileCmd {
    TextureHandle tileset;
    int tileId;
    int gridX, gridY;
    int layer;
};

class CommandBuffer {
public:
    void begin();
    void drawSprite(const DrawSpriteCmd&);
    void drawTile(const DrawTileCmd&);
    void setCamera(const CameraData&);
    void end();

    const std::vector<RenderCmd>& commands() const;
private:
    std::vector<RenderCmd> cmds_;
    bool recording_ = false;
};
```

### IAudioDevice + 异步模型

```cpp
class IAudioDevice {
public:
    virtual SoundHandle loadSound(const char* path)              = 0;
    virtual void        unloadSound(SoundHandle)                 = 0;
    virtual void        playSound(SoundHandle, float vol = 1.f)  = 0;
    virtual void        stopSound(SoundHandle)                   = 0;
    virtual void        playStream(const char* path, bool loop)  = 0;  // BGM
    virtual void        setSpatialPos(SoundHandle, float x, float y) = 0;
    virtual void        setListener(float x, float y)           = 0;
    virtual void        consumeCommands()                        = 0;  // AudioThread 调用
};

// 音频命令（主线程 push，AudioThread 消费）
struct AudioCmd {
    enum Type { PLAY, STOP, SET_SPATIAL, SET_LISTENER, LOAD, UNLOAD };
    Type        type;
    SoundHandle handle;
    float       x, y, vol;
    char        path[256];
};

class AudioCommandQueue {
    RingBuffer<AudioCmd, 1024> ring_;  // 无锁环形队列
public:
    void push(const AudioCmd&);        // 主线程调用，非阻塞
    bool pop(AudioCmd&);               // AudioThread 调用
};
```

---

## 六、engine 层

```
engine/
├── runtime/
│   ├── EngineContext.h     # 持有所有子系统，单一真相源
│   ├── SystemRegistry.h    # 注册/查询 ISystem
│   └── FrameScheduler.h    # 驱动 tick 分层
├── systems/
│   ├── ISystem.h           # 系统基类
│   ├── RenderSystem.h      # 遍历 ECS → 填充 CommandBuffer
│   ├── AudioSystem.h       # 遍历 ECS → 提交 AudioCommandQueue
│   ├── PhysicsSystem.h     # AABB 碰撞
│   ├── InputSystem.h       # 原始事件 → 语义事件
│   └── ScriptSystem.h      # 驱动用户逻辑
└── api/
    ├── GameAPI.h            # game 对外接口（语义层）
    └── EditorAPI.h          # editor 对外接口（BUILD_EDITOR）
```

### FrameScheduler — Tick 分层

```
每帧执行顺序（严格）：

1. inputSystem->preUpdate()          采集 SDL 原始事件，转语义事件
2. physicsSystem->update(dt)         固定步长物理，更新 Transform
3. scriptSystem->update(dt)          游戏逻辑，修改 ECS Component
4. audioSystem->submitCommands()     非阻塞，把音频命令推入 queue
                                     AudioThread 异步消费，不阻塞主线程
5. renderSystem->update(dt)          遍历 ECS，填充 CommandBuffer
6. renderDevice->submitCommandBuffer()  flush 到 GPU
7. renderDevice->present()           交换缓冲
8. inputSystem->postUpdate()         清除单帧状态（KeyJustPressed 等）
```

### EngineContext

```cpp
class EngineContext {
public:
    // ECS
    entt::registry world;

    // Backend（私有，外部不可访问）
    std::unique_ptr<IRenderDevice>   renderDevice;
    std::unique_ptr<IAudioDevice>    audioDevice;
    std::unique_ptr<AudioThread>     audioThread;

    // Command buffers
    CommandBuffer        renderCommandBuffer;
    AudioCommandQueue    audioCommandQueue;

    // Runtime
    SystemRegistry  systems;
    FrameScheduler  scheduler;
    AssetManager    assets;

    uint64_t frameCounter = 0;
    float    deltaTime    = 0.f;

    void init(const EngineConfig&);
    void run();
    void shutdown();
};
```

### SystemRegistry

```cpp
class SystemRegistry {
    std::vector<std::unique_ptr<ISystem>>   systems_;
    std::unordered_map<size_t, ISystem*>    lookup_;

public:
    template<typename T, typename... Args>
    T& registerSystem(Args&&... args);

    template<typename T>
    T& get();

    void updateAll(float dt);
};
```

### GameAPI（语义层，game 只看这个）

```cpp
class GameAPI {
public:
    // Entity
    EntityHandle spawnEntity();
    void         destroyEntity(EntityHandle);

    template<typename T>
    T& addComponent(EntityHandle, T component);

    template<typename T>
    T& getComponent(EntityHandle);

    // Audio（语义，不暴露 AudioCommandQueue）
    SoundHandle  loadSound(const char* assetPath);
    void         playSound(SoundHandle, float vol = 1.f);
    void         setSpatialListener(float x, float y);

    // Scene
    void         loadScene(const char* path);
    void         unloadScene();

    // Asset
    TextureHandle loadTexture(const char* assetPath);

private:
    EngineContext& ctx_;
};
```

### EditorAPI（BUILD_EDITOR 隔离）

```cpp
#ifdef BUILD_EDITOR
class EditorAPI {
public:
    // 场景预览（渲染到离屏纹理）
    TextureHandle renderSceneToTexture(int w, int h);

    // 编辑器摄像机
    void setEditorCamera(float x, float y, float zoom);

    // 临时实体（不进存档）
    EntityHandle createTransientEntity();

    // 调试
    SystemRegistry& getSystemRegistry();
    entt::registry& getWorld();

private:
    EngineContext& ctx_;
};
#endif
```

---

## 七、ECS Component 设计

```cpp
// 基础 Component
struct Transform {
    float x, y;
    float rotation;   // 弧度
    float scaleX = 1.f, scaleY = 1.f;
};

struct Velocity    { float x, y; };
struct Health      { int hp, maxHp; };

struct Sprite {
    TextureHandle texture;
    Rect          srcRect;   // spritesheet 中的区域
    int           layer;     // 渲染层级
    Color         tint = Color::White;
};

struct Animator {
    struct Frame { Rect srcRect; float duration; };
    std::vector<Frame> frames;
    float elapsedTime = 0.f;
    int   currentFrame = 0;
    bool  loop = true;
};

struct AudioEmitter {
    SoundHandle handle;
    float       volume = 1.f;
    bool        spatial = false;
    bool        dirty = false;   // 位置变化时置 true，AudioSystem 提交命令
};

struct TileMap {
    int           width, height;   // tile 数量
    int           tileSize;        // px
    TextureHandle tileset;
    std::vector<int> layers[3];    // ground / object / overhead
};

// 游戏逻辑 Component（game 层定义）
struct Harvestable { std::string itemId; bool ready; };
struct NPC         { enum State { IDLE, WALK, TALK } state; };
struct Inventory   { std::vector<std::string> items; };
```

---

## 八、CMake 结构

```cmake
cmake_minimum_required(VERSION 3.20)
project(StarEngine CXX)
set(CMAKE_CXX_STANDARD 20)

# 第三方
add_subdirectory(third_party/SDL2)
add_subdirectory(third_party/bgfx)
add_subdirectory(third_party/entt)
add_subdirectory(third_party/nlohmann_json)

# 引擎模块（按依赖顺序）
add_subdirectory(src/core)
add_subdirectory(src/platform)
add_subdirectory(src/backend)   # headers PRIVATE
add_subdirectory(src/engine)

# 可选目标
option(BUILD_EDITOR "Build editor" ON)
option(BUILD_GAME   "Build game"   ON)

if(BUILD_EDITOR) add_subdirectory(editor) endif()
if(BUILD_GAME)   add_subdirectory(game)   endif()
```

```cmake
# src/backend/CMakeLists.txt
add_library(backend STATIC
    renderer/CommandBuffer.cpp
    audio/AudioCommandQueue.cpp
    audio/AudioThread.cpp
)
# PRIVATE → 头文件不向上层传播
target_include_directories(backend PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(backend PRIVATE core platform bgfx SDL2_mixer)

# src/engine/CMakeLists.txt
add_library(engine STATIC ...)
target_link_libraries(engine
    PUBLIC  core          # 传播给 game（game 能用 core 类型）
    PRIVATE backend       # 不传播，game 看不到 backend
    PRIVATE platform entt
)

# game/CMakeLists.txt
add_executable(game ...)
target_link_libraries(game PRIVATE engine)
# game 只链接 engine，backend 头文件完全不可见
```

---

## 九、目录结构

```
StarEngine/
├── src/
│   ├── core/
│   │   ├── math/         Vec2, Vec3, Mat3, Rect
│   │   ├── memory/       Arena, PoolAllocator
│   │   ├── containers/   HandleMap, RingBuffer
│   │   ├── Handle.h
│   │   ├── Logger.h
│   │   └── Assert.h
│   ├── platform/
│   │   ├── Window.h/cpp
│   │   ├── InputRawEvent.h
│   │   ├── FileSystem.h/cpp
│   │   └── Thread.h/cpp
│   ├── backend/
│   │   ├── shared/       ResourceHandle, Fence, Sync
│   │   ├── renderer/     IRenderDevice, CommandBuffer, RenderGraph
│   │   │   ├── opengl/
│   │   │   ├── vulkan/
│   │   │   └── metal/
│   │   └── audio/        IAudioDevice, AudioCommandQueue, AudioThread
│   │       └── sdl_mixer/
│   └── engine/
│       ├── runtime/      EngineContext, SystemRegistry, FrameScheduler
│       ├── systems/      RenderSystem, AudioSystem, PhysicsSystem,
│       │                 InputSystem, ScriptSystem
│       └── api/          GameAPI.h, EditorAPI.h
├── editor/
├── game/
├── third_party/
│   ├── SDL2/
│   ├── bgfx/
│   ├── entt/
│   └── nlohmann_json/
├── assets/
│   ├── textures/
│   ├── audio/
│   ├── maps/
│   └── data/             items.json, crops.json, npcs.json
├── CMakeLists.txt
└── ARCHITECTURE.md       ← 本文件
```

---

## 十、开发里程碑

| 阶段 | 目标 |
|------|------|
| Month 1 | core + platform + backend 接口定义 + CommandBuffer 骨架 |
| Month 2 | EngineContext + SystemRegistry + FrameScheduler |
| Month 3 | RenderSystem + bgfx 实现 + Tilemap 渲染 |
| Month 4 | AudioSystem + AudioThread + SDL2_mixer 实现 |
| Month 5 | InputSystem + PhysicsSystem(AABB) + EnTT ECS 集成 |
| Month 6 | GameAPI + EditorAPI + 编辑器框架 |
| Month 7 | 场景保存/加载 + AssetManager |
| Month 8 | iOS / Android 打包 + 平台适配 |
| Month 9+ | 游戏逻辑（农场、NPC、背包、时间系统） |
