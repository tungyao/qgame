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
    explicit GLRenderDevice(SDL_Window* window, bool debug = false);
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
    
    engine::FontHandle createFont(const engine::FontData& fontData) override;
    void               destroyFont(engine::FontHandle)               override;
    const engine::FontData* getFont(engine::FontHandle) const       override;
    
    BufferHandle createBuffer(const BufferDesc&) override;
    void         destroyBuffer(BufferHandle)     override;
    void*        mapBuffer(BufferHandle)         override;
    void         unmapBuffer(BufferHandle)       override;
    void         uploadToBuffer(BufferHandle, const void* data, size_t size, size_t offset = 0) override;
    void         downloadFromBuffer(BufferHandle, void* data, size_t size, size_t offset = 0) override;
    
    ComputePipelineHandle createComputePipeline(const ComputePipelineDesc&) override;
    void                  destroyComputePipeline(ComputePipelineHandle)     override;

    // IRenderDevice — 提交 & 帧控制
    void submitCommandBuffer(const CommandBuffer&) override;
    void submitPass(const PassSubmitInfo&, const std::vector<const RenderCmd*>&) override;
    void present()                                 override;

    // IRenderDevice — 离屏渲染
    TextureHandle renderToTexture(const CommandBuffer&, int w, int h)          override;
    TextureHandle renderToTextureOffscreen(const CommandBuffer&, int w, int h) override;
    void*         getRawTexture(TextureHandle) const                            override;
    bool          getTextureDimensions(TextureHandle, int& outW, int& outH) const override;
    
    void submitGPUDrivenPass(const PassSubmitInfo& info, const GPURenderParams& params) override;

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

    struct BufferEntry {
        unsigned int glBuffer = 0;
        size_t       size     = 0;
        BufferUsage  usage    = BufferUsage::Vertex;
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
        bool          isFont = false;
        float         pxRange = 4.0f;
    };

    static constexpr int MAX_SPRITES_PER_BATCH = 2048;

    // 初始化辅助
    void createShaderProgram();
    void createBuffers();

    // 渲染辅助 — 与 SDLGPURenderDevice 共享相同逻辑
    void renderCommandBufferToTarget(const CommandBuffer& cb,
                                     unsigned int fbo,
                                     int width, int height);
    void renderCmdsToTarget(const std::vector<const RenderCmd*>& cmds,
                            const CameraData& camera,
                            bool clearEnabled,
                            core::Color clearColor,
                            unsigned int fbo,
                            int width, int height);
    void buildOrthoProjectionMatrix(float w, float h, float out[16]);
    void buildViewMatrix(float camX, float camY, float zoom, float rotation, float out[16]);

    // FBO 管理
    bool ensureFbo(FboEntry& fbo, TextureHandle& colorHandle, int w, int h);
    void destroyFbo(FboEntry& fbo, TextureHandle& colorHandle);

    SDL_Window*     window_    = nullptr;
	bool		   debug_ = false;
    SDL_GLContext   glContext_ = nullptr;

    // Shader program
    unsigned int shaderProgram_ = 0;
    int          uProjLoc_      = -1;
    int          uTexLoc_       = -1;
    
    // MSDF font shader
    unsigned int msdfShaderProgram_ = 0;
    int          msdfProjLoc_       = -1;
    int          msdfTexLoc_        = -1;
    int          msdfPxRangeLoc_    = -1;

    // 顶点/索引缓冲 (streaming, orphan each frame)
    unsigned int vao_ = 0;
    unsigned int vbo_ = 0;
    unsigned int ibo_ = 0;

    core::HandleMap<TextureHandle, TextureEntry> textures_;
    core::HandleMap<engine::FontHandle, engine::FontData> fonts_;
    core::HandleMap<BufferHandle, BufferEntry> buffers_;
    

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
