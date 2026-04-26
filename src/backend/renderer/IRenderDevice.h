#pragma once

#include "IBackendSystem.h"
#include "CommandBuffer.h"
#include "../shared/ResourceHandle.h"
#include "../../engine/components/FontData.h"

namespace backend {

enum class TextureFilter { Nearest, Linear };

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
    
    struct GPURenderParams {
        BufferHandle spriteBuffer;
        BufferHandle visibleIndexBuffer;
        uint32_t spriteCount;
        uint32_t visibleCount;
        CameraData camera;
        bool clearEnabled = true;
        core::Color clearColor = core::Color::Black;
    };
    
    virtual void submitGPUDrivenPass(const PassSubmitInfo& info, const GPURenderParams& params) = 0;

    bool debug_ = false;
};

} // namespace backend
