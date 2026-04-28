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

    // ── UI System v2 ──────────────────────────────────────────────────────────
    //
    // 节点模型：每个 UI 实体含一个 UINode（布局/状态）+ 任意视觉/功能组件
    // (UIBackground/UIButton/UIToggle/UISlider/UIProgressBar/UIImage/UILabel/
    //  UIDraggable)。父子关系通过 UINode.parent 表示，根节点的 parent 指向
    // UICanvas 实体。
    //
    // 渲染：UISystem 每帧产出 DrawSpriteCmd/DrawTextCmd（pass=Screen），
    // RenderSystem 自动注入到 CPU 与 GPU-driven 两条路径。
    //
    // 世界空间 UI：调用 attachToWorld(node, targetEntity, ...) 让 UI 跟随某个
    // 含 Transform 的世界实体投影到屏幕，仍以屏幕像素尺寸渲染。

    // ── Canvas ───────────────────────────────────────────────────────────────
    entt::entity createCanvas(int referenceW = 1920, int referenceH = 1080);
    void setCanvasScaleMode(entt::entity canvas, bool scaleWithScreen);
    void setCanvasSafeArea(entt::entity canvas, float left, float top,
                           float right, float bottom);

    // ── 通用节点 ─────────────────────────────────────────────────────────────
    entt::entity createUIElement(entt::entity parent = entt::null);
    void setUIParent(entt::entity e, entt::entity parent);
    void setUISize(entt::entity e, float width, float height);
    // (minX,minY) = 左上锚点；(maxX,maxY) = 右下锚点；min==max 表示固定锚点
    void setUIAnchor(entt::entity e, float minX, float minY, float maxX, float maxY);
    // 锚点中心相对偏移（像素，y-down）
    void setUIOffset(entt::entity e, float x, float y);
    void setUIPivot(entt::entity e, float x, float y);
    void setUIInteractable(entt::entity e, bool interactable);
    void setUIVisible(entt::entity e, bool visible);
    void setUISortOrder(entt::entity e, int order);
    // 让 UI 跟随世界实体（需要 target 含 Transform）。offset 为世界空间偏移。
    void attachToWorld(entt::entity e, entt::entity target,
                       float offsetX = 0.f, float offsetY = 0.f);
    void detachFromWorld(entt::entity e);
    // 直接给节点叠加纯色/纹理背景
    void setUIBackground(entt::entity e, const core::Color& color,
                         TextureHandle texture = {});

    // ── Tooltip ──────────────────────────────────────────────────────────────
    // 鼠标在节点上停留 delay 秒后，紧贴鼠标弹出文字面板；移开/抬起立即消失。
    // 仅 text 必填；不填 font 时使用全局默认字体（fontSize 仍生效）。
    void setUITooltip(entt::entity e, const char* text,
                      FontHandle font = {}, float fontSize = 14.f,
                      float delay = 0.4f);
    void setUITooltipColors(entt::entity e,
                            const core::Color& bg, const core::Color& text);
    void clearUITooltip(entt::entity e);

    // ── TextInput ─────────────────────────────────────────────────────────────
    entt::entity createTextInput(float width, float height);
    void setTextInputText(entt::entity e, const char* text);
    const char* getTextInputText(entt::entity e) const;
    void setTextInputFont(entt::entity e, FontHandle font, float fontSize);
    void setTextInputPlaceholder(entt::entity e, const char* text);
    void setTextInputMaxLength(entt::entity e, uint32_t maxLen);
    void setTextInputPasswordMode(entt::entity e, bool enabled);
    void setTextInputReadOnly(entt::entity e, bool readOnly);
    void setTextInputCallback(entt::entity e, std::function<void(const std::string&)> onChanged);
    void setTextInputSubmitCallback(entt::entity e, std::function<void(const std::string&)> onSubmitted);
    void setTextInputFocusCallbacks(entt::entity e, std::function<void()> onFocus, std::function<void()> onBlur);
    void focusTextInput(entt::entity e);
    void blurTextInput(entt::entity e);
    bool isTextInputFocused(entt::entity e) const;

    // ── NineSlice ────────────────────────────────────────────────────────────
    // 给一个 UINode 挂上九宫格背景：纹理被切成 3×3，节点缩放时只有边/中心拉伸，
    // 四角保持原像素尺寸。常用于对话框/面板/按钮底图。
    // border 单位是源纹理像素。srcRect 留空表示用整张纹理。
    void setUINineSlice(entt::entity e, TextureHandle texture,
                        float borderL, float borderT,
                        float borderR, float borderB,
                        const core::Color& tint = core::Color::White);
    void setUINineSliceSrcRect(entt::entity e, float x, float y, float w, float h);
    void setUINineSliceFillCenter(entt::entity e, bool fillCenter);

    // ── Button ───────────────────────────────────────────────────────────────
    entt::entity createButton(float width, float height,
                              std::function<void()> onClick = nullptr);
    void setButtonCallback(entt::entity e, std::function<void()> onClick);
    void setButtonColors(entt::entity e,
                         const core::Color& normal,
                         const core::Color& hover,
                         const core::Color& pressed);
    void setButtonEnabled(entt::entity e, bool enabled);

    // ── Toggle ───────────────────────────────────────────────────────────────
    entt::entity createToggle(float width, float height,
                              std::function<void(bool)> onChanged = nullptr);
    void setToggleValue(entt::entity e, bool isOn);
    bool getToggleValue(entt::entity e) const;
    void setToggleCallback(entt::entity e, std::function<void(bool)> onChanged);

    // ── Slider ───────────────────────────────────────────────────────────────
    entt::entity createSlider(float width, float height,
                              float min = 0.f, float max = 1.f,
                              std::function<void(float)> onChanged = nullptr);
    void setSliderValue(entt::entity e, float value);
    float getSliderValue(entt::entity e) const;
    void setSliderRange(entt::entity e, float min, float max);
    void setSliderCallback(entt::entity e, std::function<void(float)> onChanged);

    // ── Progress Bar ─────────────────────────────────────────────────────────
    entt::entity createProgressBar(float width, float height);
    void setProgressValue(entt::entity e, float value);   // 0..1
    void setProgressColors(entt::entity e,
                           const core::Color& background,
                           const core::Color& fill);

    // ── Image ────────────────────────────────────────────────────────────────
    entt::entity createUIImage(float width, float height, TextureHandle texture = {});
    void setUIImageTexture(entt::entity e, TextureHandle texture);
    void setUIImageColor(entt::entity e, const core::Color& color);

    // ── Label / Text ─────────────────────────────────────────────────────────
    entt::entity createUIText(float width, float height, const char* text = "");
    void setUIText(entt::entity e, const char* text);
    void setUITextFont(entt::entity e, FontHandle font, float fontSize);
    void setUITextColor(entt::entity e, const core::Color& color);
    void setUITextAlignment(entt::entity e, int halign);  // 0=Left, 1=Center, 2=Right

    // ── ScrollView ───────────────────────────────────────────────────────────
    // 创建一个滚动容器；其子节点（通过 setUIParent 挂在它下面）会按内部 scroll
    // 偏移整体平移，并被裁剪到视口矩形。dir: 0=Vertical, 1=Horizontal, 2=Both。
    entt::entity createScrollView(float width, float height, int dir = 0);
    void setScrollContentSize(entt::entity e, float w, float h);
    void setScrollOffset(entt::entity e, float x, float y);
    void setScrollWheelSpeed(entt::entity e, float speed);
    void setScrollOnChanged(entt::entity e, std::function<void(float, float)> cb);
    void getScrollOffset(entt::entity e, float* outX, float* outY) const;

    // ── LayoutGroup ──────────────────────────────────────────────────────────
    // 创建一个自动布局容器：其直接子节点会按 type 排成行/列/网格，子节点的
    // anchor/pivot/offset 由 UISystem 每帧改写（Stretch 模式还会改写交叉轴尺寸）。
    // type: 0=Horizontal, 1=Vertical, 2=Grid。
    entt::entity createLayoutGroup(float width, float height, int type = 1);
    void setLayoutPadding(entt::entity e, float left, float top,
                          float right, float bottom);
    void setLayoutSpacing(entt::entity e, float spacingX, float spacingY);
    void setLayoutGridColumns(entt::entity e, int columns);     // 仅 Grid
    void setLayoutCrossAlign(entt::entity e, int align);        // 0=Start 1=Center 2=End 3=Stretch
    void setLayoutReverse(entt::entity e, bool reverse);

    // ── Draggable ────────────────────────────────────────────────────────────
    // 调用后强制 anchor=topLeft, pivot=(0,0)，offset 直接当作屏幕坐标。
    void makeDraggable(entt::entity e,
                       std::function<void(float x, float y)> onDrag = nullptr);
    void setDragBounds(entt::entity e, float minX, float minY,
                       float maxX, float maxY);
    // 设置拖拽元素的 payload 标签（与 UIDropTarget::acceptedPayload 配对过滤）
    void setDragPayload(entt::entity e, std::string payload);
    // 未命中任何 DropTarget 时是否自动回弹到拖拽起点
    void setDragSnapBack(entt::entity e, bool snapBack);

    // ── DropTarget ───────────────────────────────────────────────────────────
    // 把任意 UI 节点标记为可投放区域。onDrop 接收正在被拖的 UIDraggable 实体。
    void makeDropTarget(entt::entity e,
                        std::function<void(entt::entity)> onDrop = nullptr);
    // 仅当 UIDraggable.payload == acceptedPayload 时接受（空字符串 = 不过滤）
    void setDropAcceptedPayload(entt::entity e, std::string payload);
    void setDropHoverHighlight(entt::entity e, bool enabled, core::Color color);
    void setDropAcceptCallback(entt::entity e,
                               std::function<bool(entt::entity)> canAccept);
    void setDropEnterCallback(entt::entity e,
                              std::function<void(entt::entity)> onEnter);
    void setDropLeaveCallback(entt::entity e,
                              std::function<void(entt::entity)> onLeave);

    // ── 状态查询 ─────────────────────────────────────────────────────────────
    bool isPointerOverUI(entt::entity e) const;
    entt::entity getHoveredUI() const;
    entt::entity getPressedUI() const;
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
