#pragma once
#include <string>
#include <unordered_map>
#include "../../backend/shared/ResourceHandle.h"

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

    // 引用计数 -1，归零时销毁 GPU 资源
    void releaseTexture(TextureHandle h);
    void releaseSound(SoundHandle h);

    // 通过 handle 反查路径（SceneSerializer 序列化时用）
    const std::string& texturePath(TextureHandle h) const;
    const std::string& soundPath(SoundHandle h)    const;

private:
    backend::IRenderDevice* render_ = nullptr;
    backend::IAudioDevice*  audio_  = nullptr;

    struct TexEntry { TextureHandle handle; int refCount = 0; };
    struct SndEntry { SoundHandle   handle; int refCount = 0; };

    std::unordered_map<std::string, TexEntry> texByPath_;
    std::unordered_map<std::string, SndEntry> sndByPath_;

    // 反查表
    std::unordered_map<uint32_t, std::string> texPathById_;
    std::unordered_map<uint32_t, std::string> sndPathById_;

    static const std::string kEmpty_;
};

} // namespace engine
