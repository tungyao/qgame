#pragma once
#include "IBackendSystem.h"
#include "CommandBuffer.h"
#include "../shared/ResourceHandle.h"

namespace backend {

struct TextureDesc {
    int         width  = 0;
    int         height = 0;
    int         channels = 4;  // RGBA
    const void* data   = nullptr;
    bool        mips   = false;
};

struct ShaderDesc {
    const void* vsData  = nullptr;  // vertex shader 字节码
    size_t      vsSize  = 0;
    const void* fsData  = nullptr;  // fragment shader 字节码
    size_t      fsSize  = 0;
};

class IRenderDevice : public IBackendSystem {
public:
    // 资源管理
    virtual TextureHandle createTexture(const TextureDesc&)  = 0;
    virtual void          destroyTexture(TextureHandle)       = 0;
    virtual ShaderHandle  createShader(const ShaderDesc&)    = 0;
    virtual void          destroyShader(ShaderHandle)         = 0;

    // 提交命令
    virtual void submitCommandBuffer(const CommandBuffer&)   = 0;
    virtual void present()                                   = 0;

    // 编辑器专用：渲染到离屏纹理（返回可给 ImGui 用的句柄）
    virtual TextureHandle renderToTexture(const CommandBuffer&, int w, int h) = 0;
};

} // namespace backend
