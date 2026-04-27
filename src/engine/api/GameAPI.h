#pragma once
#include <entt/entt.hpp>
#include "../../backend/shared/ResourceHandle.h"
#include "../runtime/EngineContext.h"
#include "../assets/AssetManager.h"
#include "../components/PhysicsComponents.h"
#include "../components/UIComponents.h"
#include "../systems/PhysicsSystem.h"
#include "../Export.h"

namespace engine {

class QGAME_ENGINE_API GameAPI {
public:
    explicit GameAPI(EngineContext& ctx) : ctx_(ctx) {}

    // ── Entity ────────────────────────────────────────────────────────────
    entt::entity spawnEntity();
    void         destroyEntity(entt::entity e);

    // 组件操作：extern template 声明，实例化在 DLL 中
    template<typename T>
    T& addComponent(entt::entity e, T component);

    template<typename T>
    T& getComponent(entt::entity e);

    template<typename T>
    bool hasComponent(entt::entity e) const;

    // 通过函数对象修改组件，结束后触发 EnTT on_update<T> 信号。
    // 与 getComponent 的引用赋值不同，这条路径会让 RenderSystem 等监听者得到通知，
    // 用来改 Transform/Sprite 等"被订阅"的组件时必须走这里。
    template<typename T, typename Fn>
    void patchComponent(entt::entity e, Fn&& fn);

    // ── Audio（Month 4 实现）──────────────────────────────────────────────
    SoundHandle loadSound(const char* assetPath);
    void        playSound(SoundHandle h, float vol = 1.f);
    void        stopSound(SoundHandle h);
    void        playMusic(const char* assetPath, bool loop = true);
    void        stopMusic();
    void        setSpatialListener(float x, float y);

    // ── Input（Month 5 实现）──────────────────────────────────────────────
    bool isKeyDown(int keyCode)         const;
    bool isKeyJustPressed(int keyCode)  const;
    bool isKeyJustReleased(int keyCode) const;
    bool pointerDown(int id = 0)        const;
    float pointerX(int id = 0)          const;
    float pointerY(int id = 0)          const;

    // ── Physics（Month 5 实现）────────────────────────────────────────────
    
    /**
     * 设置全局重力
     * @param x X 方向重力加速度（像素/秒²）
     * @param y Y 方向重力加速度（像素/秒²）
     */
    void setGravity(float x, float y);
    
    /**
     * 设置物理更新的固定时间步
     * @param step 时间步长（秒），默认 1/60
     */
    void setFixedTimestep(float step);

    /**
     * 射线检测 - 从起点发射射线，返回最近的碰撞体
     * @param startX, startY 射线起点
     * @param dirX, dirY 射线方向（无需归一化）
     * @param maxDist 最大检测距离
     * @param layerMask 只检测指定层（默认所有层）
     * @return RaycastHit 结果（.hit 表示是否命中）
     */
    RaycastHit raycast(float startX, float startY, float dirX, float dirY, float maxDist,
                       CollisionLayer layerMask = COLLISION_LAYER_ALL);
    
    /**
     * 盒形区域查询 - 检测矩形区域内所有碰撞体
     * @param centerX, centerY 查询区域中心
     * @param halfW, halfH 查询区域半宽/半高
     * @param layerMask 只查询指定层（默认所有层）
     * @return 重叠的碰撞体列表
     */
    std::vector<OverlapResult> overlapBox(float centerX, float centerY,
                                          float halfW, float halfH,
                                          CollisionLayer layerMask = COLLISION_LAYER_ALL);
    
    /**
     * 圆形区域查询 - 检测圆形区域内所有碰撞体
     * @param centerX, centerY 圆心
     * @param radius 半径
     * @param layerMask 只查询指定层（默认所有层）
     * @return 重叠的碰撞体列表
     */
    std::vector<entt::entity> overlapCircle(float centerX, float centerY, float radius,
                                            CollisionLayer layerMask = COLLISION_LAYER_ALL);

    /**
     * 注册碰撞事件回调
     * @param listener 监听器对象引用
     * @param fn 成员函数指针，签名为 void(const CollisionInfo&)
     * 
     * 用例：
     * class MyGame {
     *     void onCollision(const CollisionInfo& info) {
     *         if (info.overlapY < 0) { // landed on top }
     *     }
     * };
     * api.onCollision(myGame, &MyGame::onCollision);
     */
    template<typename Listener>
    void onCollision(Listener& listener, void(Listener::*fn)(const CollisionInfo&));

    // ── Scene（Month 8 实现）─────────────────────────────────────────────
    bool loadScene(const char* path);
    bool saveScene(const char* path);
    void unloadScene();

    // ── Asset ─────────────────────────────────────────────────────────────
    TextureHandle loadTexture(const char* assetPath);
    void          releaseTexture(TextureHandle h);
    FontHandle    loadFont(const char* assetPath);
    void          releaseFont(FontHandle h);
    AssetManager& assetManager();

    // 从内存像素数据上传纹理（RGBA8，测试/程序化纹理用）
    TextureHandle createTextureFromMemory(const void* rgbaPixels, int w, int h);

    // ── Animation ──────────────────────────────────────────────────────────
    // 创建并返回一个动画句柄（程序化创建，不依赖文件）
    AnimationHandle createAnimation(const char* name, const engine::AnimationClip& clip);

    // ── Time ─────────────────────────────────────────────────────────────────
    
    /**
     * 获取上一帧的增量时间（秒），已乘以 timeScale
     */
    float getDeltaTime() const;
    
    /**
     * 获取全局时间缩放因子
     */
    float getTimeScale() const;
    
    /**
     * 设置全局时间缩放（慢动作/暂停）
     * @param scale 缩放因子 (0=暂停, 0.5=半速, 1=正常)
     */
    void setTimeScale(float scale);

    // ── UI System ─────────────────────────────────────────────────────────────
    
    // ═══════════════════════════════════════════════════════════════════════════
    // Canvas - UI 根容器
    // ═══════════════════════════════════════════════════════════════════════════
    
    /**
     * 创建 Canvas (UI 根容器)
     * @param referenceW 设计分辨率宽度 (默认 1920)
     * @param referenceH 设计分辨率高度 (默认 1080)
     * @return Canvas 实体
     */
    entt::entity createCanvas(int referenceW = 1920, int referenceH = 1080);
    
    /**
     * 设置 Canvas 缩放模式
     * @param canvas Canvas 实体
     * @param scaleWithScreen true=随屏幕缩放, false=固定像素
     */
    void setCanvasScaleMode(entt::entity canvas, bool scaleWithScreen);
    
    /**
     * 设置 Canvas 安全区域 (刘海屏适配)
     */
    void setCanvasSafeArea(entt::entity canvas, float left, float top, float right, float bottom);
    
    // ═══════════════════════════════════════════════════════════════════════════
    // UI Element 创建
    // ═══════════════════════════════════════════════════════════════════════════
    
    /**
     * 创建 UI 元素
     * @param parent 父节点 (entt::null 表示根节点)
     * @return UI 元素实体
     */
    entt::entity createUIElement(entt::entity parent = entt::null);
    
    /**
     * 设置 UI 元素尺寸
     */
    void setUISize(entt::entity e, float width, float height);
    
    /**
     * 设置 UI 元素锚点
     * @param minX, minY 左上角锚点 (0-1)
     * @param maxX, maxY 右下角锚点 (0-1)
     */
    void setUIAnchor(entt::entity e, float minX, float minY, float maxX, float maxY);
    
    /**
     * 设置 UI 元素锚点偏移 (像素)
     */
    void setUIOffset(entt::entity e, float left, float top, float right, float bottom);
    
    /**
     * 设置 UI 元素中心点
     */
    void setUIPivot(entt::entity e, float x, float y);
    
    /**
     * 设置 UI 元素可交互性
     */
    void setUIInteractable(entt::entity e, bool interactable);
    
    /**
     * 设置 UI 元素排序
     */
    void setUISortOrder(entt::entity e, int order);
    
    // ═══════════════════════════════════════════════════════════════════════════
    // Button - 按钮
    // ═══════════════════════════════════════════════════════════════════════════
    
    /**
     * 创建按钮
     * @param width, height 按钮尺寸
     * @param onClick 点击回调
     * @return 按钮实体
     */
    entt::entity createButton(float width, float height, std::function<void()> onClick = nullptr);
    
    /**
     * 设置按钮点击回调
     */
    void setButtonCallback(entt::entity e, std::function<void()> onClick);
    
    /**
     * 设置按钮颜色状态
     */
    void setButtonColors(entt::entity e, 
                         const core::Color& normal,
                         const core::Color& hover,
                         const core::Color& pressed);
    
    /**
     * 设置按钮禁用状态
     */
    void setButtonEnabled(entt::entity e, bool enabled);
    
    // ═══════════════════════════════════════════════════════════════════════════
    // Toggle - 开关
    // ═══════════════════════════════════════════════════════════════════════════
    
    /**
     * 创建开关
     */
    entt::entity createToggle(float width, float height, 
                              std::function<void(bool)> onValueChanged = nullptr);
    
    /**
     * 设置开关状态
     */
    void setToggleValue(entt::entity e, bool isOn);
    
    /**
     * 获取开关状态
     */
    bool getToggleValue(entt::entity e) const;
    
    /**
     * 设置开关回调
     */
    void setToggleCallback(entt::entity e, std::function<void(bool)> onValueChanged);
    
    // ═══════════════════════════════════════════════════════════════════════════
    // Slider - 滑动条
    // ═══════════════════════════════════════════════════════════════════════════
    
    /**
     * 创建滑动条
     */
    entt::entity createSlider(float width, float height,
                              float min = 0.f, float max = 1.f,
                              std::function<void(float)> onValueChanged = nullptr);
    
    /**
     * 设置滑动条值
     */
    void setSliderValue(entt::entity e, float value);
    
    /**
     * 获取滑动条值
     */
    float getSliderValue(entt::entity e) const;
    
    /**
     * 设置滑动条范围
     */
    void setSliderRange(entt::entity e, float min, float max);
    
    /**
     * 设置滑动条回调
     */
    void setSliderCallback(entt::entity e, std::function<void(float)> onValueChanged);
    
    // ═══════════════════════════════════════════════════════════════════════════
    // ProgressBar - 进度条
    // ═══════════════════════════════════════════════════════════════════════════
    
    /**
     * 创建进度条
     */
    entt::entity createProgressBar(float width, float height);
    
    /**
     * 设置进度条值 (0-1)
     */
    void setProgressValue(entt::entity e, float value);
    
    /**
     * 设置进度条颜色
     */
    void setProgressColors(entt::entity e, 
                           const core::Color& background,
                           const core::Color& fill);
    
    // ═══════════════════════════════════════════════════════════════════════════
    // Image - UI 图像
    // ═══════════════════════════════════════════════════════════════════════════
    
    /**
     * 创建 UI 图像
     */
    entt::entity createUIImage(float width, float height, TextureHandle texture = {});
    
    /**
     * 设置 UI 图像纹理
     */
    void setUIImageTexture(entt::entity e, TextureHandle texture);
    
    /**
     * 设置 UI 图像颜色
     */
    void setUIImageColor(entt::entity e, const core::Color& color);
    
    // ═══════════════════════════════════════════════════════════════════════════
    // Text - UI 文本
    // ═══════════════════════════════════════════════════════════════════════════
    
    /**
     * 创建 UI 文本
     */
    entt::entity createUIText(float width, float height, const char* text = "");
    
    /**
     * 设置 UI 文本内容
     */
    void setUIText(entt::entity e, const char* text);
    
    /**
     * 设置 UI 文本字体
     */
    void setUITextFont(entt::entity e, FontHandle font, float fontSize);
    
    /**
     * 设置 UI 文本颜色
     */
    void setUITextColor(entt::entity e, const core::Color& color);
    
    /**
     * 设置 UI 文本对齐方式
     */
    void setUITextAlignment(entt::entity e, int alignment);  // 0=Left, 1=Center, 2=Right
    
    // ═══════════════════════════════════════════════════════════════════════════
    // Drag - 拖拽
    // ═══════════════════════════════════════════════════════════════════════════
    
    /**
     * 使元素可拖拽
     */
    void makeDraggable(entt::entity e, 
                       std::function<void(float x, float y)> onDrag = nullptr);
    
    /**
     * 设置拖拽范围限制
     */
    void setDragBounds(entt::entity e, float minX, float minY, float maxX, float maxY);
    
    // ═══════════════════════════════════════════════════════════════════════════
    // UI State Query
    // ═══════════════════════════════════════════════════════════════════════════
    
    /**
     * 检测指针是否在元素上
     */
    bool isPointerOverUI(entt::entity e) const;
    
    /**
     * 获取当前悬停的 UI 元素
     */
    entt::entity getHoveredUI() const;
    
    /**
     * 获取当前按下的 UI 元素
     */
    entt::entity getPressedUI() const;
    
    /**
     * 获取 UI 元素的计算位置 (屏幕坐标)
     */
    void getUIComputedRect(entt::entity e, float* outX, float* outY, 
                           float* outW, float* outH) const;

    // ── Engine control ────────────────────────────────────────────────────
    void quit();

private:
    EngineContext& ctx_;
};

} // namespace engine

// ── 模板实现（内联，但需要配合 extern template 实例化）───────────────────────
namespace engine {

template<typename T>
T& GameAPI::addComponent(entt::entity e, T component) {
    return ctx_.world.template emplace_or_replace<T>(e, std::move(component));
}

template<typename T>
T& GameAPI::getComponent(entt::entity e) {
    return ctx_.world.template get<T>(e);
}

template<typename T>
bool GameAPI::hasComponent(entt::entity e) const {
    return ctx_.world.template all_of<T>(e);
}

template<typename T, typename Fn>
void GameAPI::patchComponent(entt::entity e, Fn&& fn) {
    ctx_.world.template patch<T>(e, std::forward<Fn>(fn));
}

template<typename Listener>
void GameAPI::onCollision(Listener& listener, void(Listener::*fn)(const CollisionInfo&)) {
    ctx_.dispatcher.template sink<CollisionInfo>().template connect<fn>(listener);
}

} // namespace engine

// ── 包含 extern template 声明（防止客户端代码实例化）─────────────────────────
#include "GameAPI_ExternTemplates.h"
