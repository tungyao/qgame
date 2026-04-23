#pragma once

namespace backend {

// 所有 backend 子系统必须实现的统一生命周期接口
// FrameScheduler 按固定顺序驱动所有 IBackendSystem
class IBackendSystem {
public:
    virtual ~IBackendSystem() = default;
    virtual void init()       = 0;
    virtual void beginFrame() = 0;  // 帧开始，重置状态
    virtual void endFrame()   = 0;  // 帧结束，flush 命令
    virtual void shutdown()   = 0;
};

} // namespace backend
