#pragma once
#include <vector>
#include <unordered_map>
#include <optional>
#include "CommandBuffer.h"
#include "IRenderDevice.h"
#include "../../core/math/Rect.h"
#include "../../core/math/Color.h"

namespace backend {

struct Viewport {
    int x = 0, y = 0;
    int width = 0, height = 0;
};

struct PassState {
    engine::RenderPass pass        = engine::RenderPass::World;
    Viewport           viewport;
    CameraData         camera;
    core::Color        clearColor  = core::Color::Black;
    bool               clearEnabled = true;
    bool               scissorEnabled = false;
    core::Rect         scissorRect;
};

struct PassDependency {
    engine::RenderPass from;
    engine::RenderPass to;
};

// Pass 执行顺序：若无依赖，按 addPass 的插入顺序执行；有依赖时走拓扑排序。
class RenderPipeline {
public:
    RenderPipeline() = default;

    void addPass(engine::RenderPass pass);
    void removePass(engine::RenderPass pass);
    bool hasPass(engine::RenderPass pass) const;

    void addDependency(engine::RenderPass from, engine::RenderPass to);
    void removeDependency(engine::RenderPass from, engine::RenderPass to);
    bool hasDependency(engine::RenderPass from, engine::RenderPass to) const;

    const std::vector<engine::RenderPass>& getPassExecutionOrder() const;

    void setPassViewport(engine::RenderPass pass, const Viewport& vp);
    const Viewport* getPassViewport(engine::RenderPass pass) const;

    void setPassCamera(engine::RenderPass pass, const CameraData& cam);
    const CameraData* getPassCamera(engine::RenderPass pass) const;

    void setPassClear(engine::RenderPass pass, bool enabled, core::Color color = core::Color::Black);

    // 执行所有 pass：按拓扑顺序为每个 pass 构建独立 CommandBuffer 并 submit
    void execute(CommandBuffer& cb, IRenderDevice& device);

    const std::vector<engine::RenderPass>& passes() const { return passes_; }
    std::vector<PassDependency> getDependencies() const;

private:
    bool topologicalSort(std::vector<engine::RenderPass>& outOrder) const;

    std::vector<engine::RenderPass>                    passes_;
    std::unordered_map<engine::RenderPass, PassState>  passStates_;
    std::vector<PassDependency>                        dependencies_;

    mutable std::optional<std::vector<engine::RenderPass>> cachedOrder_;
    mutable bool orderDirty_ = true;
};

} // namespace backend
