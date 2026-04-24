#pragma once
#include <string>
#include <unordered_map>
#include "../../backend/shared/ResourceHandle.h"
#include "../components/AnimatorComponent.h"
#include "../components/FontData.h"

namespace backend {
class IRenderDevice;
class IAudioDevice;
}

namespace engine {

// 按路径缓存 Texture / Sound，引用计数，统一生命周期管理
class AssetManager {
public:
    void init(backend::IRenderDevice* render, backend::IAudioDevice* audio);
    void shutdown();

    // 返回已缓存的 handle，不存在则加载；引用计数 +1
    TextureHandle loadTexture(const std::string& path);
    SoundHandle   loadSound(const std::string& path);
    AnimationHandle loadAnimation(const std::string& path);
    // path 是 ttf 路径；实际加载 sibling "<path>.font"（编译阶段预烘焙，本地阶段手工生成）。
    FontHandle    loadFont(const std::string& path);

    // 引用计数 -1，归零时销毁 GPU 资源
    void releaseTexture(TextureHandle h);
    void releaseSound(SoundHandle h);
    void releaseAnimation(AnimationHandle h);
    void releaseFont(FontHandle h);

    // 通过 handle 反查路径（SceneSerializer 序列化时用）
    const std::string& texturePath(TextureHandle h) const;
    const std::string& soundPath(SoundHandle h)    const;
    const std::string& animationPath(AnimationHandle h) const;
    const std::string& fontPath(FontHandle h) const;

    // 获取动画剪辑数据
    const AnimationClip* getAnimationClip(AnimationHandle h) const;

    // 直接注册动画剪辑（用于程序化创建，不依赖文件）
    AnimationHandle registerAnimation(const std::string& name, const AnimationClip& clip);

private:
    backend::IRenderDevice* render_ = nullptr;
    backend::IAudioDevice*  audio_  = nullptr;

    struct TexEntry { TextureHandle handle; int refCount = 0; };
    struct SndEntry { SoundHandle   handle; int refCount = 0; };
    struct AnimEntry { AnimationHandle handle; int refCount = 0; AnimationClip clip; };
    struct FontEntry { FontHandle handle; TextureHandle atlas; int refCount = 0; };

    std::unordered_map<std::string, TexEntry> texByPath_;
    std::unordered_map<std::string, SndEntry> sndByPath_;
    std::unordered_map<std::string, AnimEntry> animByPath_;
    std::unordered_map<std::string, FontEntry> fontByPath_;

    // 反查表
    std::unordered_map<uint32_t, std::string> texPathById_;
    std::unordered_map<uint32_t, std::string> sndPathById_;
    std::unordered_map<uint32_t, std::string> animPathById_;
    std::unordered_map<uint32_t, std::string> fontPathById_;

    static const std::string kEmpty_;
    static uint32_t nextAnimIndex_;
};

} // namespace engine
