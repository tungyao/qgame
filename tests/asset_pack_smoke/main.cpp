#include <cstdio>
#include <string>
#include <unordered_map>

#include <backend/audio/IAudioDevice.h>
#include <backend/renderer/IRenderDevice.h>
#include <engine/assets/AssetManager.h>

#ifndef QGAME_PACK_TEST_MANIFEST
#define QGAME_PACK_TEST_MANIFEST "assets/manifest.json"
#endif

namespace {

int fail(const char* msg) {
    std::fprintf(stderr, "asset_pack_smoke: %s\n", msg);
    return 1;
}

class FakeRenderDevice final : public backend::IRenderDevice {
public:
    void init() override {}
    void beginFrame() override {}
    void endFrame() override {}
    void shutdown() override {}

    const backend::RendererCapabilities& capabilities() const override { return caps_; }
    const backend::RenderFrameStats& frameStats() const override { return stats_; }
    backend::RenderFrameStats& mutableFrameStats() override { return stats_; }
    void resetFrameStats() override { stats_ = {}; }

    TextureHandle createTexture(const backend::TextureDesc& desc) override {
        TextureHandle h{nextTexture_++, 1};
        textures_[h.index] = TextureInfo{desc.width, desc.height, desc.format};
        return h;
    }

    void destroyTexture(TextureHandle h) override { textures_.erase(h.index); }

    ShaderHandle createShader(const backend::ShaderDesc&) override { return ShaderHandle{nextShader_++, 1}; }
    void destroyShader(ShaderHandle) override {}

    engine::FontHandle createFont(const engine::FontData& fontData) override {
        engine::FontHandle h{nextFont_++, 1};
        fonts_[h.index] = fontData;
        return h;
    }

    void destroyFont(engine::FontHandle h) override { fonts_.erase(h.index); }

    const engine::FontData* getFont(engine::FontHandle h) const override {
        auto it = fonts_.find(h.index);
        return it != fonts_.end() ? &it->second : nullptr;
    }

    BufferHandle createBuffer(const backend::BufferDesc&) override { return BufferHandle{nextBuffer_++, 1}; }
    void destroyBuffer(BufferHandle) override {}
    void* mapBuffer(BufferHandle) override { return nullptr; }
    void unmapBuffer(BufferHandle) override {}
    void uploadToBuffer(BufferHandle, const void*, size_t, size_t = 0) override {}
    void downloadFromBuffer(BufferHandle, void*, size_t, size_t = 0) override {}

    ComputePipelineHandle createComputePipeline(const backend::ComputePipelineDesc&) override {
        return ComputePipelineHandle{nextCompute_++, 1};
    }
    void destroyComputePipeline(ComputePipelineHandle) override {}

    void submitCommandBuffer(const backend::CommandBuffer&) override {}
    void submitPass(const PassSubmitInfo&, const std::vector<const backend::RenderCmd*>&) override {}
    void present() override {}
    TextureHandle renderToTexture(const backend::CommandBuffer&, int, int) override { return {}; }
    TextureHandle renderToTextureOffscreen(const backend::CommandBuffer&, int, int) override { return {}; }
    void* getRawTexture(TextureHandle) const override { return nullptr; }

    bool getTextureDimensions(TextureHandle h, int& outW, int& outH) const override {
        auto it = textures_.find(h.index);
        if (it == textures_.end()) return false;
        outW = it->second.width;
        outH = it->second.height;
        return true;
    }

    void submitGPUDrivenPass(const PassSubmitInfo&, const GPURenderParams&) override {}

private:
    struct TextureInfo {
        int width = 0;
        int height = 0;
        backend::TextureFormat format = backend::TextureFormat::RGBA8;
    };

    backend::RendererCapabilities caps_{};
    backend::RenderFrameStats stats_{};
    std::unordered_map<uint32_t, TextureInfo> textures_;
    std::unordered_map<uint32_t, engine::FontData> fonts_;
    uint32_t nextTexture_ = 1;
    uint32_t nextShader_ = 1;
    uint32_t nextFont_ = 1;
    uint32_t nextBuffer_ = 1;
    uint32_t nextCompute_ = 1;
};

class FakeAudioDevice final : public backend::IAudioDevice {
public:
    void init() override {}
    void shutdown() override {}

    SoundHandle loadSound(const char*) override { return SoundHandle{nextSound_++, 1}; }
    SoundHandle loadSoundFromMemory(const void*, size_t size, const char*) override {
        return size > 0 ? SoundHandle{nextSound_++, 1} : SoundHandle{};
    }
    void unloadSound(SoundHandle) override {}
    void playSound(SoundHandle, float = 1.f) override {}
    void stopSound(SoundHandle) override {}
    void playStream(const char*, bool) override {}
    void stopStream() override {}
    void setSpatialPos(SoundHandle, float, float) override {}
    void setListener(float, float) override {}
    void update() override {}

private:
    uint32_t nextSound_ = 1;
};

} // namespace

int main() {
    FakeRenderDevice render;
    FakeAudioDevice audio;
    engine::AssetManager assets;
    assets.init(&render, &audio);

    if (!assets.loadManifest(QGAME_PACK_TEST_MANIFEST)) {
        return fail("failed to load baked manifest");
    }
    if (!assets.hasAsset("texture.demo.character")
        || !assets.hasAsset("font.demo.main")
        || !assets.hasAsset("animation.demo.test")) {
        return fail("expected demo asset ids missing");
    }

    TextureHandle character = assets.loadTextureById("texture.demo.character");
    if (!character.valid()) return fail("texture.demo.character did not load");
    if (assets.texturePath(character).rfind("pak://main/", 0) != 0) {
        return fail("texture path is not a pak:// path");
    }

    int tw = 0, th = 0;
    if (!render.getTextureDimensions(character, tw, th) || tw != 32 || th != 48) {
        return fail("character texture dimensions mismatch");
    }

    TextureHandle region = assets.regionIdTexture(character);
    if (!region.valid()) return fail("character region id texture did not load from pack");
    int rw = 0, rh = 0;
    if (!render.getTextureDimensions(region, rw, rh) || rw != tw || rh != th) {
        return fail("region id texture dimensions mismatch");
    }

    engine::FontHandle font = assets.loadFontById("font.demo.main");
    if (!font.valid()) return fail("font.demo.main did not load");
    const engine::FontData* fontData = render.getFont(font);
    if (!fontData || fontData->glyphs.empty() || !fontData->texture.valid()) {
        return fail("font data was not populated");
    }
    if (assets.fontPath(font).rfind("pak://main/", 0) != 0) {
        return fail("font path is not a pak:// path");
    }

    AnimationHandle anim = assets.loadAnimationById("animation.demo.test");
    if (!anim.valid()) return fail("animation.demo.test did not load");
    const engine::AnimationClip* clip = assets.getAnimationClip(anim);
    if (!clip || clip->frames.empty() || !clip->texture.valid()) {
        return fail("animation clip or spritesheet did not load");
    }
    if (assets.animationPath(anim).rfind("pak://main/", 0) != 0) {
        return fail("animation path is not a pak:// path");
    }

    assets.shutdown();
    std::puts("asset_pack_smoke: ok");
    return 0;
}
