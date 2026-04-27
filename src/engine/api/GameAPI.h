#pragma once
#include <entt/entt.hpp>
#include "../../backend/shared/ResourceHandle.h"
#include "../runtime/EngineContext.h"
#include "../assets/AssetManager.h"
#include "../components/PhysicsComponents.h"
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
