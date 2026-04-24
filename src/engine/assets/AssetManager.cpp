#include "AssetManager.h"
#include "../../backend/renderer/IRenderDevice.h"
#include "../../backend/audio/IAudioDevice.h"
#include "../../core/Logger.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>

namespace engine {

const std::string AssetManager::kEmpty_;
uint32_t AssetManager::nextAnimIndex_ = 1;

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
    animByPath_.clear();
    animPathById_.clear();
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

// 解析 Aseprite JSON (Array 格式)。path 形如 "anim/player.json" 或 "anim/player.json#walk"
AnimationHandle AssetManager::loadAnimation(const std::string& path) {
    auto it = animByPath_.find(path);
    if (it != animByPath_.end()) {
        it->second.refCount++;
        return it->second.handle;
    }

    // 拆分 path 和 tag
    std::string filePath = path;
    std::string tagName;
    if (auto hash = path.find('#'); hash != std::string::npos) {
        filePath = path.substr(0, hash);
        tagName  = path.substr(hash + 1);
    }

    AnimationClip clip;
    clip.name = tagName.empty() ? filePath : tagName;
    clip.loop = true;

    std::ifstream ifs(filePath);
    if (!ifs.is_open()) {
        core::logError("[AssetManager] failed to open animation file: %s", filePath.c_str());
        return {};
    }

    nlohmann::json j;
    try {
        ifs >> j;
    } catch (const std::exception& e) {
        core::logError("[AssetManager] json parse error: %s", e.what());
        return {};
    }

    // 1) 收集所有 frame (Array 格式: j["frames"] 是数组；Hash 格式是对象)
    struct RawFrame { core::Rect rect; float duration; };
    std::vector<RawFrame> rawFrames;

    auto pushFrame = [&](const nlohmann::json& f) {
        auto& fr = f["frame"];
        RawFrame rf;
        rf.rect = { fr.value("x", 0.f), fr.value("y", 0.f),
                    fr.value("w", 0.f), fr.value("h", 0.f) };
        rf.duration = f.value("duration", 100) / 1000.f; // ms → s
        rawFrames.push_back(rf);
    };
    if (j.contains("frames")) {
        if (j["frames"].is_array()) {
            for (auto& f : j["frames"]) pushFrame(f);
        } else if (j["frames"].is_object()) {
            for (auto& [k, f] : j["frames"].items()) pushFrame(f);
        }
    }

    // 2) 选择 tag 范围
    int from = 0, to = (int)rawFrames.size() - 1;
    bool loop = true;
    if (!tagName.empty() && j.contains("meta") && j["meta"].contains("frameTags")) {
        bool found = false;
        for (auto& t : j["meta"]["frameTags"]) {
            if (t.value("name", "") == tagName) {
                from = t.value("from", 0);
                to   = t.value("to", from);
                std::string dir = t.value("direction", "forward");
                loop = (dir != "none"); // Aseprite 没有显式 loop 字段，约定 forward/pingpong = 循环
                found = true;
                break;
            }
        }
        if (!found) {
            core::logWarn("[AssetManager] tag not found: %s in %s", tagName.c_str(), filePath.c_str());
        }
    }

    // 3) 填充 clip.frames
    from = std::max(0, from);
    to   = std::min(to, (int)rawFrames.size() - 1);
    for (int i = from; i <= to; ++i) {
        AnimationFrame af;
        af.srcRect = rawFrames[i].rect;
        af.duration = rawFrames[i].duration;
        clip.frames.push_back(af);
        clip.duration += af.duration;
    }
    clip.loop = loop;

    // 4) 加载 spritesheet 贴图 (meta.image 相对 JSON 文件目录)
    if (j.contains("meta") && j["meta"].contains("image")) {
        std::filesystem::path dir = std::filesystem::path(filePath).parent_path();
        std::filesystem::path img = dir / j["meta"]["image"].get<std::string>();
        clip.texture = loadTexture(img.string());
    }

    AnimationHandle h;
    h.index = nextAnimIndex_++;
    h.version = 1;
    uint32_t id = (uint32_t(h.index) << 12) | h.version;
    animByPath_[path] = {h, 1, std::move(clip)};
    animPathById_[id] = path;
    return h;
}

void AssetManager::releaseAnimation(AnimationHandle h) {
    if (!h.valid()) return;
    uint32_t id = (uint32_t(h.index) << 12) | h.version;
    auto pit = animPathById_.find(id);
    if (pit == animPathById_.end()) return;

    auto& entry = animByPath_[pit->second];
    if (--entry.refCount <= 0) {
        if (entry.clip.texture.valid()) releaseTexture(entry.clip.texture);
        animByPath_.erase(pit->second);
        animPathById_.erase(id);
    }
}

const std::string& AssetManager::animationPath(AnimationHandle h) const {
    if (!h.valid()) return kEmpty_;
    uint32_t id = (uint32_t(h.index) << 12) | h.version;
    auto it = animPathById_.find(id);
    return (it != animPathById_.end()) ? it->second : kEmpty_;
}

const AnimationClip* AssetManager::getAnimationClip(AnimationHandle h) const {
    if (!h.valid()) return nullptr;
    uint32_t id = (uint32_t(h.index) << 12) | h.version;
    auto pit = animPathById_.find(id);
    if (pit == animPathById_.end()) return nullptr;
    auto it = animByPath_.find(pit->second);
    if (it == animByPath_.end()) return nullptr;
    return &it->second.clip;
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
