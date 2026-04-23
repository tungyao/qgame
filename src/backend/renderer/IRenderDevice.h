#pragma once

#include "IBackendSystem.h"
#include "CommandBuffer.h"
#include "../shared/ResourceHandle.h"

struct ImDrawData;

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
    virtual void submitImGuiDrawData(const ImDrawData* drawData) = 0;
    virtual void present() = 0;

    virtual TextureHandle renderToTexture(const CommandBuffer&, int w, int h) = 0;
};

} // namespace backend
