#include "AssetManager.h"
#include "../../backend/renderer/IRenderDevice.h"
#include "../../backend/audio/IAudioDevice.h"
#include "../../core/Logger.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace engine {

const std::string AssetManager::kEmpty_;

void AssetManager::init(backend::IRenderDevice* render, backend::IAudioDevice* audio) {
    render_ = render;
    audio_  = audio;
}

void AssetManager::shutdown() {
    // 强制释放所有残留资源（不依赖 release 引用计数）
    if (render_) {
        for (auto& [path, e] : texByPath_)
            render_->destroyTexture(e.handle);
    }
    if (audio_) {
        for (auto& [path, e] : sndByPath_)
            audio_->unloadSound(e.handle);
    }
    texByPath_.clear();
    sndByPath_.clear();
    texPathById_.clear();
    sndPathById_.clear();
}

TextureHandle AssetManager::loadTexture(const std::string& path) {
    auto it = texByPath_.find(path);
    if (it != texByPath_.end()) {
        it->second.refCount++;
        return it->second.handle;
    }

    if (!render_) return {};

    int w, h, ch;
    unsigned char* pixels = stbi_load(path.c_str(), &w, &h, &ch, 4);
    if (!pixels) {
        core::logError("[AssetManager] failed to load texture: %s", path.c_str());
        return {};
    }

    backend::TextureDesc desc{};
    desc.data    = pixels;
    desc.width   = w;
    desc.height  = h;
    desc.channels = 4;
    TextureHandle h_tex = render_->createTexture(desc);
    stbi_image_free(pixels);

    if (!h_tex.valid()) return {};

    uint32_t id = (uint32_t(h_tex.index) << 12) | h_tex.version;
    texByPath_[path]  = {h_tex, 1};
    texPathById_[id]  = path;
    return h_tex;
}

SoundHandle AssetManager::loadSound(const std::string& path) {
    auto it = sndByPath_.find(path);
    if (it != sndByPath_.end()) {
        it->second.refCount++;
        return it->second.handle;
    }

    if (!audio_) return {};

    SoundHandle h_snd = audio_->loadSound(path.c_str());
    if (!h_snd.valid()) {
        core::logError("[AssetManager] failed to load sound: %s", path.c_str());
        return {};
    }

    uint32_t id = (uint32_t(h_snd.index) << 12) | h_snd.version;
    sndByPath_[path]  = {h_snd, 1};
    sndPathById_[id]  = path;
    return h_snd;
}

void AssetManager::releaseTexture(TextureHandle h) {
    if (!h.valid() || !render_) return;
    uint32_t id = (uint32_t(h.index) << 12) | h.version;
    auto pit = texPathById_.find(id);
    if (pit == texPathById_.end()) return;

    auto& entry = texByPath_[pit->second];
    if (--entry.refCount <= 0) {
        render_->destroyTexture(h);
        texByPath_.erase(pit->second);
        texPathById_.erase(id);
    }
}

void AssetManager::releaseSound(SoundHandle h) {
    if (!h.valid() || !audio_) return;
    uint32_t id = (uint32_t(h.index) << 12) | h.version;
    auto pit = sndPathById_.find(id);
    if (pit == sndPathById_.end()) return;

    auto& entry = sndByPath_[pit->second];
    if (--entry.refCount <= 0) {
        audio_->unloadSound(h);
        sndByPath_.erase(pit->second);
        sndPathById_.erase(id);
    }
}

const std::string& AssetManager::texturePath(TextureHandle h) const {
    if (!h.valid()) return kEmpty_;
    uint32_t id = (uint32_t(h.index) << 12) | h.version;
    auto it = texPathById_.find(id);
    return (it != texPathById_.end()) ? it->second : kEmpty_;
}

const std::string& AssetManager::soundPath(SoundHandle h) const {
    if (!h.valid()) return kEmpty_;
    uint32_t id = (uint32_t(h.index) << 12) | h.version;
    auto it = sndPathById_.find(id);
    return (it != sndPathById_.end()) ? it->second : kEmpty_;
}

} // namespace engine
