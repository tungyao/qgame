#pragma once

#include "IBackendSystem.h"
#include "CommandBuffer.h"
#include "../shared/ResourceHandle.h"

namespace backend {

struct TextureDesc {
    int width = 0;
    int height = 0;
    int channels = 4;
    const void* data = nullptr;
    bool mips = false;
};

struct ShaderDesc {
    const void* vsData = nullptr;
    size_t vsSize = 0;
    const void* fsData = nullptr;
    size_t fsSize = 0;
};

class IRenderDevice : public IBackendSystem {
public:
    virtual TextureHandle createTexture(const TextureDesc&) = 0;
    virtual void destroyTexture(TextureHandle) = 0;
    virtual ShaderHandle createShader(const ShaderDesc&) = 0;
    virtual void destroyShader(ShaderHandle) = 0;

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
};

} // namespace backend
