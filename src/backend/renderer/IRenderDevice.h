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
    uint32_t lighting2DSubmitCount = 0;
    uint32_t renderGraphPassCount = 0;
    uint32_t renderGraphBarrierCount = 0;
    uint32_t worldColorPassCount = 0;
    uint32_t lightingCompositeCount = 0;
    uint32_t uiPassCount = 0;

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
        // Optional per-camera visible index payload for frame graph execution.
        // The legacy immediate path may upload the shared visibleIndexBuffer
        // before submit, but a deferred frame graph needs the upload tied to
        // the camera node so later cameras do not overwrite the same buffer.
        std::vector<uint32_t> ownedVisibleIndices;
        CameraData camera;
        bool clearEnabled = true;
        core::Color clearColor = core::Color::Black;
    };
    
    virtual void submitGPUDrivenPass(const PassSubmitInfo& info, const GPURenderParams& params) = 0;

    struct GPUParticleParams {
        ComputePipelineHandle updatePipeline;
        ComputePipelineHandle sortPipeline;          // odd-even fallback (>256)
        ComputePipelineHandle bitonicSortPipeline;   // single-pass bitonic (≤256)
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
        bool sortEnabled = true;  // skip sort when emitter has ySort==false
    };

    virtual void submitGPUParticlePass(const PassSubmitInfo& info, const GPUParticleParams& params) = 0;

    // L3 2D lighting prototype. The CPU only uploads scene-space light and
    // occluder data; the SDL_GPU/Vulkan path builds per-screen-tile light lists
    // and shades the half-resolution dynamic-light + hard-shadow overlay
    // entirely on the GPU. The public contract is intentionally still small so
    // later RenderGraph work can replace the alpha-blended overlay composite
    // without changing ECS collection code.
    struct Light2DPoint {
        float x = 0.f, y = 0.f;
        float radius = 1.f, intensity = 1.f;
        float colorR = 1.f, colorG = 1.f, colorB = 1.f, colorA = 1.f;
        float softness = 16.f;
        uint32_t layerMask = 0xFFFFFFFFu;
        uint32_t castsShadow = 1u;
        uint32_t pad0 = 0u;
    };

    struct Light2DSegment {
        float ax = 0.f, ay = 0.f, bx = 0.f, by = 0.f;
        float opacity = 1.f, pad0 = 0.f, pad1 = 0.f, pad2 = 0.f;
    };

    struct Reflector2DRegion {
        float ax = 0.f, ay = 0.f, bx = 0.f, by = 0.f;
        float width = 0.f, height = 0.f;
        float reflectivity = 0.5f, roughness = 0.35f;
        float tintR = 1.f, tintG = 1.f, tintB = 1.f, tintA = 1.f;
        uint32_t shape = 0u;
        uint32_t visible = 1u;
        uint32_t pad0 = 0u;
        uint32_t pad1 = 0u;
    };

    struct Lighting2DParams {
        std::vector<Light2DPoint> lights;
        std::vector<Light2DSegment> segments;
        std::vector<Reflector2DRegion> reflectors;
        CameraData camera;
        uint32_t viewportW = 0;
        uint32_t viewportH = 0;
        float ambientR = 24.f / 255.f;
        float ambientG = 32.f / 255.f;
        float ambientB = 52.f / 255.f;
        float ambientA = 1.f;
        float ambientIntensity = 0.18f;
        float exposure = 0.9f;
        float wetness = 0.f;
        float time = 0.f;
        uint32_t frameIndex = 0;
        bool enabled = true;
    };

    virtual void submitLighting2DPass(const PassSubmitInfo& info, const Lighting2DParams& params) = 0;

    // First RenderGraph-facing submission used by the SDL GPU lighting upgrade.
    // It deliberately passes command pointers instead of owning command storage:
    // RenderSystem already owns stable per-frame command vectors, and avoiding
    // copies keeps this bridge small while the graph abstraction is still young.
    struct WorldLightingSubmitInfo {
        PassSubmitInfo worldPass;
        bool hasGPUWorld = false;
        GPURenderParams gpuWorld;
        std::vector<const RenderCmd*> worldCommands;
        std::vector<GPUParticleParams> particles;
        std::vector<const RenderCmd*> uiCommands;
        Lighting2DParams lighting;
    };

    virtual void submitWorldLightingGraph(const WorldLightingSubmitInfo& info) = 0;

    enum class FrameGraphPassKind : uint8_t {
        Raster,
        GPUDriven,
        WorldLighting,
    };

    // Frame-level graph bridge: RenderSystem collects all active cameras for a
    // frame, sorts them by Camera::depth, and submits this ordered list once.
    // The backend still executes each node explicitly today, but the camera
    // boundary is now represented as data instead of hidden in RenderSystem's
    // loop. This is the stepping stone toward a real scheduler with resource
    // lifetime/barrier ownership.
    struct FrameGraphCameraPass {
        FrameGraphPassKind kind = FrameGraphPassKind::Raster;
        const char* debugName = "CameraPass";
        PassSubmitInfo pass;
        std::vector<const RenderCmd*> commands;
        std::vector<GPUParticleParams> particles;
        GPURenderParams gpu;
        WorldLightingSubmitInfo worldLighting;

        // Optional storage for commands generated while building this frame
        // graph node. Pointer vectors above may reference these elements; the
        // frame graph is submitted immediately, so this avoids leaking
        // RenderSystem-local temporaries into backend lifetime.
        std::vector<RenderCmd> ownedCommands;
    };

    struct FrameGraphSubmitInfo {
        std::vector<FrameGraphCameraPass> cameraPasses;
    };

    virtual void submitFrameGraph(const FrameGraphSubmitInfo& info) {
        auto uploadOwnedVisibleIndices = [this](const GPURenderParams& gpu) {
            if (!gpu.visibleIndexBuffer.valid() || gpu.ownedVisibleIndices.empty()) return;
            uploadToBuffer(gpu.visibleIndexBuffer,
                           gpu.ownedVisibleIndices.data(),
                           gpu.ownedVisibleIndices.size() * sizeof(uint32_t),
                           0);
        };

        for (const FrameGraphCameraPass& pass : info.cameraPasses) {
            switch (pass.kind) {
                case FrameGraphPassKind::WorldLighting:
                    if (pass.worldLighting.hasGPUWorld) {
                        uploadOwnedVisibleIndices(pass.worldLighting.gpuWorld);
                    }
                    submitWorldLightingGraph(pass.worldLighting);
                    break;
                case FrameGraphPassKind::GPUDriven:
                    uploadOwnedVisibleIndices(pass.gpu);
                    submitGPUDrivenPass(pass.pass, pass.gpu);
                    for (const GPUParticleParams& particle : pass.particles) {
                        submitGPUParticlePass({ pass.pass.camera, false, pass.pass.clearColor }, particle);
                    }
                    if (!pass.commands.empty()) {
                        PassSubmitInfo overlay = pass.pass;
                        overlay.clearEnabled = false;
                        submitPass(overlay, pass.commands);
                    }
                    break;
                case FrameGraphPassKind::Raster:
                default:
                    submitPass(pass.pass, pass.commands);
                    for (const GPUParticleParams& particle : pass.particles) {
                        submitGPUParticlePass({ pass.pass.camera, false, pass.pass.clearColor }, particle);
                    }
                    break;
            }
        }
    }

    bool debug_ = false;
};

} // namespace backend
