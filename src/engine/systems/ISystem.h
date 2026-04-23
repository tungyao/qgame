#pragma once

namespace engine {

class ISystem {
public:
    virtual ~ISystem() = default;

    virtual void init()              {}
    virtual void preUpdate()         {}
    virtual void update(float dt)    { (void)dt; }
    virtual void postUpdate()        {}
    virtual void shutdown()          {}

    // 返回 true 表示此 System 由 FrameScheduler 在特定步骤显式调用，
    // 不参与 Step 3 的通用 update 循环
    virtual bool isManuallyScheduled() const { return false; }
};

} // namespace engine
