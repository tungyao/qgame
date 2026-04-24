#pragma once
#include "../IRenderDevice.h"
#include "../../../core/containers/HandleMap.h"
#include <SDL3/SDL.h>
#include <vector>

namespace backend {

// OpenGL 3.3 Core Profile 渲染设备
// 兼容不支持 Vulkan 的老显卡（Intel HD 2000+, GeForce 400+, Radeon HD 5000+）
class GLRenderDevice final : public IRenderDevice {
public:
    explicit GLRenderDevice(SDL_Window* window);
    ~GLRenderDevice() override;

    // IBackendSystem
    void init()       override;
    void beginFrame() override;
    void endFrame()   override;
    void shutdown()   override;

    // IRenderDevice — 资源
    TextureHandle createTexture(const TextureDesc&) override;
    void          destroyTexture(TextureHandle)     override;
    ShaderHandle  createShader(const ShaderDesc&)  override;
    void          destroyShader(ShaderHandle)       override;

    // IRenderDevice — 提交 & 帧控制
    void submitCommandBuffer(const CommandBuffer&) override;
    void submitImGuiDrawData(const ImDrawData*)    override;
    void present()                                 override;
    void initImGui()                               override;
    void shutdownImGui()                           override;

    // IRenderDevice — 离屏渲染
    TextureHandle renderToTexture(const CommandBuffer&, int w, int h)          override;
    TextureHandle renderToTextureOffscreen(const CommandBuffer&, int w, int h) override;
    void*         getRawTexture(TextureHandle) const                            override;

private:
    struct TextureEntry {
        unsigned int glTex   = 0;
        int          width   = 0;
        int          height  = 0;
    };

    struct FboEntry {
        unsigned int fbo = 0;
        TextureHandle colorTex{};
        int width  = 0;
        int height = 0;
    };

    struct SpriteVertex {
        float   x, y;
        float   u, v;
        uint8_t r, g, b, a;
    };

    struct BatchSegment {
        TextureHandle tex;
        uint32_t      idxOffset;
        uint32_t      idxCount;
        int32_t       vertOffset;
    };

    static constexpr int MAX_SPRITES_PER_BATCH = 2048;

    // 初始化辅助
    void createShaderProgram();
    void createBuffers();

    // 渲染辅助 — 与 SDLGPURenderDevice 共享相同逻辑
    void renderCommandBufferToTarget(const CommandBuffer& cb,
                                     unsigned int fbo,
                                     int width, int height);
    void buildSpriteGeometry(const std::vector<DrawSpriteCmd>& cmds,
                             std::vector<BatchSegment>& batches);
    void buildTileGeometry(const std::vector<DrawTileCmd>& cmds,
                           std::vector<BatchSegment>& batches);
    void buildOrthoMatrix(float w, float h, float out[16]);
    void buildOrthoProjectionMatrix(float w, float h, float out[16]);
    void buildViewMatrix(float camX, float camY, float zoom, float rotation, float out[16]);
    void buildOrthoMatrixCamera(float w, float h, float camX, float camY, float zoom, float rotation, float out[16]);

    // FBO 管理
    bool ensureFbo(FboEntry& fbo, TextureHandle& colorHandle, int w, int h);
    void destroyFbo(FboEntry& fbo, TextureHandle& colorHandle);

    SDL_Window*     window_    = nullptr;
    SDL_GLContext   glContext_ = nullptr;

    // Shader program
    unsigned int shaderProgram_ = 0;
    int          uProjLoc_      = -1;
    int          uTexLoc_       = -1;

    // 顶点/索引缓冲 (streaming, orphan each frame)
    unsigned int vao_ = 0;
    unsigned int vbo_ = 0;
    unsigned int ibo_ = 0;

    core::HandleMap<TextureHandle, TextureEntry> textures_;

    // renderToTexture (swapchain-paired, 复用 FBO)
    FboEntry      screenFbo_{};
    TextureHandle screenTarget_{};

    // renderToTextureOffscreen (独立 FBO)
    FboEntry      offscreenFbo_{};
    TextureHandle offscreenTarget_{};

    std::vector<SpriteVertex> batchVerts_;
    std::vector<uint16_t>     batchIdx_;
};

} // namespace backend
