#pragma once

#include "IBackendSystem.h"
#include "CommandBuffer.h"
#include "../shared/ResourceHandle.h"
#include "../../engine/components/FontData.h"
#include <cstdint>

namespace backend {

enum class TextureFilter { Nearest, Linear };
enum class TextureFormat { RGBA8, R8 };

// 后端能力是渲染路径选择的唯一事实来源。上层不要通过 dynamic_cast
// 猜测设备类型，而是根据这些标志决定是否启用 GPU-driven、compute、
// indirect draw 等高级路径。
struct RendererCapabilities {
    // Compute/storage are the foundation for GPU culling, GPU sorting, and
    // compute lighting. They do not imply a specific high-level renderer.
    bool supportsCompute = false;
    bool supportsStorageBuffer = false;
    bool supportsStorageTexture = false;

    // High-level path switches. A backend may support compute but still fail
    // the specialized sprite pipeline, in which case RenderSystem must fall
    // back to CPU-batch rendering.
    bool supportsGPUDrivenSprite = false;
    bool supportsIndirectDraw = false;

    // Binding/diagnostic capabilities that unlock later stages without
    // changing the public rendering model.
    bool supportsTextureArray = false;
    bool supportsTimestampQuery = false;

    // 2D lighting is intentionally separate from generic compute. A backend may
    // run compute shaders but still lack the storage-texture / render-target
    // sampling path required by the planned lighting buffer and composite pass.
    bool supportsWorldOffscreenColor = false;
    bool supportsSampledRenderTarget = false;
    bool supportsLighting2D = false;

    // Diagnostic backend name for debug overlays and demo scenes. Keep it a
    // static string so per-frame stats never allocate.
    const char* backendName = "Unknown";
};

enum class RenderPath : uint8_t {
    Unknown = 0,
    SDLGPU_GPUDriven,
    SDLGPU_CPUBatch,
    OpenGL_CPUBatch
};

inline const char* renderPathName(RenderPath path) {
    switch (path) {
        case RenderPath::SDLGPU_GPUDriven: return "SDL_GPU_GPUDriven";
        case RenderPath::SDLGPU_CPUBatch:  return "SDL_GPU_CPUBatch";
        case RenderPath::OpenGL_CPUBatch:  return "OpenGL_CPUBatch";
        case RenderPath::Unknown:
        default:                           return "Unknown";
    }
}

// 每帧统计只记录“路径与成本信号”，不承载策略逻辑。它是后续
// upload queue、GPU culling、indirect draw 的回归仪表盘。
struct RenderFrameStats {
    RenderPath path = RenderPath::Unknown;
    const char* fallbackReason = nullptr; // 指向静态字符串，避免每帧分配。

    // Scene scale observed by RenderSystem.
    uint32_t spriteCount = 0;
    uint32_t visibleSpriteCount = 0;

    // Backend submission shape. GPU batches count GPU-driven instance batches;
    // draw calls includes both CPU-batch and GPU-driven draw submissions.
    uint32_t gpuDrawBatchCount = 0;
    uint32_t drawCallCount = 0;
    uint32_t computeDispatchCount = 0;
    uint32_t textureBindCount = 0;

    // Phase L0/L1 lighting counters. These do not imply that lighting is
    // rendered yet; they prove the ECS data path is visible to RenderSystem and
    // give the future compute pass a simple regression signal.
    uint32_t light2DCount = 0;
    uint32_t occluder2DCount = 0;
    uint32_t reflector2DCount = 0;
    uint32_t environment2DCount = 0;

    // Upload pressure is the first target for the next stage: frame upload
    // queue + staging/ring buffers should drive these numbers down.
    uint64_t uploadBytes = 0;
    uint32_t uploadCallCount = 0;
};

enum class BufferUsage : uint32_t {
    Vertex   = 1 << 0,
    Index    = 1 << 1,
    Storage  = 1 << 2,
    Indirect = 1 << 3,
    Uniform  = 1 << 4,
};

inline BufferUsage operator|(BufferUsage a, BufferUsage b) {
    return static_cast<BufferUsage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline uint32_t operator&(BufferUsage a, BufferUsage b) {
    return static_cast<uint32_t>(a) & static_cast<uint32_t>(b);
}

struct TextureDesc {
    int width = 0;
    int height = 0;
    int channels = 4;
    const void* data = nullptr;
    bool mips = false;
    TextureFilter filter = TextureFilter::Nearest;
    TextureFormat format = TextureFormat::RGBA8;
};

struct ShaderDesc {
    const void* vsData = nullptr;
    size_t vsSize = 0;
    const void* fsData = nullptr;
    size_t fsSize = 0;
};

struct BufferDesc {
    size_t size = 0;
    BufferUsage usage = BufferUsage::Vertex;
    const void* initialData = nullptr;
};

struct ComputePipelineDesc {
       // OpenGL backend: GLSL source via code/codeSize.
       // SDL3 GPU backend: prefers spirvCode/dxilCode based on its shaderFormat,
       // falls back to code/codeSize if those are not set.
    const void* code = nullptr;
    size_t codeSize = 0;
    const void* spirvCode = nullptr;
    size_t      spirvSize = 0;
    const void* dxilCode = nullptr;
    size_t      dxilSize = 0;
    const char* entryPoint = "main";
    uint32_t threadCountX = 64;
    uint32_t threadCountY = 1;
    uint32_t threadCountZ = 1;
    uint32_t numSamplers = 0;
    uint32_t numReadonlyStorageTextures = 0;
    uint32_t numReadonlyStorageBuffers = 0;
    uint32_t numReadwriteStorageTextures = 0;
    uint32_t numReadwriteStorageBuffers = 0;
    uint32_t numUniformBuffers = 0;
};

class IRenderDevice : public IBackendSystem {
public:
    virtual const RendererCapabilities& capabilities() const = 0;
    virtual const RenderFrameStats& frameStats() const = 0;
    virtual RenderFrameStats& mutableFrameStats() = 0;
    virtual void resetFrameStats() = 0;

    virtual TextureHandle createTexture(const TextureDesc&) = 0;
    virtual void destroyTexture(TextureHandle) = 0;
    virtual ShaderHandle createShader(const ShaderDesc&) = 0;
    virtual void destroyShader(ShaderHandle) = 0;
    
    virtual engine::FontHandle createFont(const engine::FontData& fontData) = 0;
    virtual void               destroyFont(engine::FontHandle) = 0;
    virtual const engine::FontData* getFont(engine::FontHandle) const = 0;
    
    virtual BufferHandle createBuffer(const BufferDesc&) = 0;
    virtual void destroyBuffer(BufferHandle) = 0;
    virtual void* mapBuffer(BufferHandle) = 0;
    virtual void unmapBuffer(BufferHandle) = 0;
    virtual void uploadToBuffer(BufferHandle, const void* data, size_t size, size_t offset = 0) = 0;
    virtual void downloadFromBuffer(BufferHandle, void* data, size_t size, size_t offset = 0) = 0;
    
    virtual ComputePipelineHandle createComputePipeline(const ComputePipelineDesc&) = 0;
    virtual void destroyComputePipeline(ComputePipelineHandle) = 0;

    virtual void submitCommandBuffer(const CommandBuffer&) = 0;

    // Pipeline-driven path：pass 的 camera/clear 由调用方显式传入，cmd 列表以指针形式给出，
    // 避免 pipeline 为每个 pass 再录制一个 CommandBuffer。目标固定为 swapchain。
    struct PassSubmitInfo {
        CameraData  camera;
        bool        clearEnabled = true;
        core::Color clearColor   = core::Color::Black;
    };
    virtual void submitPass(const PassSubmitInfo& info,
                            const std::vector<const RenderCmd*>& cmds) = 0;

    virtual void present() = 0;

    virtual TextureHandle renderToTexture(const CommandBuffer&, int w, int h) = 0;
    virtual TextureHandle renderToTextureOffscreen(const CommandBuffer&, int w, int h) = 0;

    virtual void* getRawTexture(TextureHandle) const = 0;
    virtual bool getTextureDimensions(TextureHandle, int& outW, int& outH) const = 0;
    
    // 一段连续的可见 sprite，绑定同一张纹理。firstInstance 是 visibleIndexBuffer 中的偏移。
    struct GPUDrawBatch {
        TextureHandle texture;
        uint32_t      firstInstance = 0;
        uint32_t      instanceCount = 0;
    };

    struct GPURenderParams {
        BufferHandle spriteBuffer;
        BufferHandle visibleIndexBuffer;
        uint32_t     spriteCount  = 0;
        uint32_t     visibleCount = 0;
        std::vector<GPUDrawBatch> batches;
        CameraData camera;
        bool clearEnabled = true;
        core::Color clearColor = core::Color::Black;
    };
    
    virtual void submitGPUDrivenPass(const PassSubmitInfo& info, const GPURenderParams& params) = 0;

    struct GPUParticleParams {
        ComputePipelineHandle updatePipeline;
        ComputePipelineHandle sortPipeline;
        BufferHandle particleBuffer;
        BufferHandle aliveIndexBuffer;
        BufferHandle indirectArgsBuffer;
        TextureHandle texture;
        uint32_t firstParticle = 0;
        uint32_t particleCount = 0;
        float dt = 0.f;
        CameraData camera;
        bool clearEnabled = false;
        core::Color clearColor = core::Color::Black;
    };

    virtual void submitGPUParticlePass(const PassSubmitInfo& info, const GPUParticleParams& params) = 0;

    bool debug_ = false;
};

} // namespace backend
