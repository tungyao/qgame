#pragma once
#include "CommandBuffer.h"
#include "../../engine/light/LightCollector.h"
#include <vector>

namespace backend {

struct FrameContext {
    int viewportW = 0, viewportH = 0;
    // 将来可以添加：GBuffer纹理、LightRT等
};

class LightingPass {
public:
    void execute(FrameContext& ctx, CommandBuffer& cb, const engine::light::LightFrameData& lights);
};

class CompositePass {
public:
    void execute(FrameContext& ctx, CommandBuffer& cb);
};

} // namespace backend
