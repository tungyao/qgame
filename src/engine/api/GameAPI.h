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
    void setGravity(float x, float y);

    // 注册碰撞事件回调（sink 绑定，生命周期由调用方管理）
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
