#pragma once
#include "../IRenderDevice.h"
#include "../../shared/ResourceHandle.h"
#include "../../../core/containers/HandleMap.h"
#include <SDL3/SDL_gpu.h>
#include <vector>

struct SDL_Window;

namespace backend {

// SDL3 GPU API 实现的渲染设备
// 支持 Vulkan（Linux/Windows）/ Metal（macOS/iOS）/ D3D12（Windows）
class SDLGPURenderDevice final : public IRenderDevice {
public:
    explicit SDLGPURenderDevice(SDL_Window* window, bool debug = false);
    ~SDLGPURenderDevice() override;

    // IBackendSystem
    void init()       override;
    void beginFrame() override;
    void endFrame()   override;
    void shutdown()   override;

    // IRenderDevice
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

    void submitCommandBuffer(const CommandBuffer&) override;
    void submitPass(const PassSubmitInfo&, const std::vector<const RenderCmd*>&) override;
    void present()                                 override;

    TextureHandle renderToTexture(const CommandBuffer&, int w, int h) override;
    TextureHandle renderToTextureOffscreen(const CommandBuffer&, int w, int h) override;
    void*         getRawTexture(TextureHandle handle) const override;
    bool          getTextureDimensions(TextureHandle, int& outW, int& outH) const override;
    SDL_GPUTexture* getSDLTexture(TextureHandle handle) const;
    
    void submitGPUDrivenPass(const PassSubmitInfo& info, const GPURenderParams& params) override;

    // 供 RenderSystem 查询设备，上传纹理用
    SDL_GPUDevice* gpuDevice() const { return device_; }

private:
    // ── 内部类型 ─────────────────────────────────────────────────────────────
    struct TextureEntry {
        SDL_GPUTexture*  gpuTex   = nullptr;
        SDL_GPUSampler*  sampler  = nullptr;
        int              width    = 0;
        int              height   = 0;
    };

    struct BufferEntry {
        SDL_GPUBuffer*         gpuBuffer = nullptr;
        SDL_GPUTransferBuffer* transfer   = nullptr;
        size_t                 size       = 0;
        BufferUsage            usage      = BufferUsage::Vertex;
    };

    struct ComputePipelineEntry {
        SDL_GPUComputePipeline* pipeline = nullptr;
    };

    struct SpriteVertex {
        float    x, y;        // position
        float    u, v;        // UV
        uint8_t  r, g, b, a; // color (normalized)
    };

    static constexpr int MAX_SPRITES_PER_BATCH = 2048;

    // ── 初始化辅助 ────────────────────────────────────────────────────────────
    void createPipeline();
    SDL_GPUGraphicsPipeline* createPipelineForFormat(SDL_GPUTextureFormat format);
    SDL_GPUGraphicsPipeline* createMSDFPipelineForFormat(SDL_GPUTextureFormat format);
    SDL_GPUGraphicsPipeline* createGPUDrivenPipelineForFormat(SDL_GPUTextureFormat format);
    void                     createGPUDrivenIndexBuffer();
    SDL_GPUShader* loadShader(const uint8_t* code, size_t size,
                              SDL_GPUShaderStage stage,
                              int numSamplers, int numUniformBuffers,
                              SDL_GPUShaderFormat fmt);

    // ── 绘制辅助 ──────────────────────────────────────────────────────────────
    struct BatchSegment {
        TextureHandle tex;
        uint32_t      idxOffset;
        uint32_t      idxCount;
        int32_t       vertOffset;
        bool          isFont = false;
        float         pxRange = 4.0f;
    };
    // 将像素空间 (0,0)-(w,h) 映射到 NDC 的正交投影矩阵（列主序）
    void buildOrthoMatrix(float w, float h, float out[16]);
    void buildOrthoProjectionMatrix(float w, float h, float out[16]);
    void buildViewMatrix(float camX, float camY, float zoom, float rotation, float out[16]);
    void buildOrthoMatrixCamera(float w, float h, float camX, float camY, float zoom, float rotation, float out[16]);

    // ── 成员 ──────────────────────────────────────────────────────────────────
    SDL_Window*              window_             = nullptr;
    bool                     debug_              = false;
    SDL_GPUDevice*           device_             = nullptr;
    SDL_GPUGraphicsPipeline* pipeline_           = nullptr;
    SDL_GPUGraphicsPipeline* offscreenPipeline_  = nullptr;
    SDL_GPUGraphicsPipeline* msdfPipeline_       = nullptr;
    SDL_GPUGraphicsPipeline* msdfOffscreenPipeline_ = nullptr;
    SDL_GPUGraphicsPipeline* gpuDrivenPipeline_  = nullptr;
    SDL_GPUBuffer*           gpuDrivenQuadIndexBuf_ = nullptr;
    SDL_GPUShaderFormat      shaderFormat_       = SDL_GPU_SHADERFORMAT_DXIL;

    // 每帧动态顶点/索引缓冲（CPU→GPU transfer）
    SDL_GPUBuffer*           vertexBuf_ = nullptr;
    SDL_GPUBuffer*           indexBuf_  = nullptr;
    SDL_GPUTransferBuffer*   transferBuf_ = nullptr;

    // UBO (projection matrix) — 通过 PushUniformData 传递（更简单）
    // SDL3 GPU API 支持 SDL_PushGPUVertexUniformData

    core::HandleMap<TextureHandle, TextureEntry> textures_;
    core::HandleMap<engine::FontHandle, engine::FontData> fonts_;
    core::HandleMap<BufferHandle, BufferEntry> buffers_;
    core::HandleMap<ComputePipelineHandle, ComputePipelineEntry> computePipelines_;
    TextureHandle             editorRenderTarget_{};
    int                       editorRenderTargetWidth_ = 0;
    int                       editorRenderTargetHeight_ = 0;

    // Offscreen rendering (independent of swapchain)
    TextureHandle             offscreenRenderTarget_{};
    int                       offscreenRenderTargetWidth_ = 0;
    int                       offscreenRenderTargetHeight_ = 0;

    // 当前帧 command buffer（SDL3 GPU 概念，不是我们的 backend::CommandBuffer）
    SDL_GPUCommandBuffer* gpuCmdBuf_ = nullptr;

    // 当前帧 swapchain 纹理
    SDL_GPUTexture* swapchainTex_ = nullptr;
    uint32_t        swapW_ = 0, swapH_ = 0;

    // batch 状态
    std::vector<SpriteVertex> batchVerts_;
    std::vector<uint16_t>     batchIdx_;
    TextureHandle             batchTex_;
    int                       batchLayer_ = 0;
    void renderCommandBufferToTarget(SDL_GPUCommandBuffer* cmdBuf, SDL_GPUGraphicsPipeline* pipeline, const CommandBuffer& cb, SDL_GPUTexture* target, uint32_t width, uint32_t height, bool clearTarget);
    void renderCmdsToTarget(SDL_GPUCommandBuffer* cmdBuf, SDL_GPUGraphicsPipeline* pipeline,
                            const std::vector<const RenderCmd*>& cmds,
                            const CameraData& camera,
                            bool clearEnabled,
                            core::Color clearColor,
                            SDL_GPUTexture* target,
                            uint32_t width, uint32_t height);
    TextureHandle createRenderTargetTexture(int width, int height);
};

} // namespace backend
