#include "RenderPipeline.h"
#include "../../core/Logger.h"
#include <algorithm>
#include <queue>

namespace backend {

void RenderPipeline::addPass(engine::RenderPass pass) {
    if (!hasPass(pass)) {
        passes_.push_back(pass);
        passStates_[pass] = PassState{};
        passStates_[pass].pass = pass;
        orderDirty_ = true;
    }
}

void RenderPipeline::removePass(engine::RenderPass pass) {
    for (auto it = passes_.begin(); it != passes_.end(); ++it) {
        if (*it == pass) {
            passes_.erase(it);
            passStates_.erase(pass);
            for (auto depIt = dependencies_.begin(); depIt != dependencies_.end(); ) {
                if (depIt->from == pass || depIt->to == pass) {
                    depIt = dependencies_.erase(depIt);
                } else {
                    ++depIt;
                }
            }
            orderDirty_ = true;
            break;
        }
    }
}

bool RenderPipeline::hasPass(engine::RenderPass pass) const {
    for (auto p : passes_) {
        if (p == pass) return true;
    }
    return false;
}

void RenderPipeline::addDependency(engine::RenderPass from, engine::RenderPass to) {
    if (!hasPass(from) || !hasPass(to)) {
        core::logError("RenderPipeline::addDependency: pass not found");
        return;
    }
    if (from == to) {
        core::logError("RenderPipeline::addDependency: self dependency");
        return;
    }
    for (const auto& dep : dependencies_) {
        if (dep.from == from && dep.to == to) return;
    }
    dependencies_.push_back({from, to});
    orderDirty_ = true;
}

void RenderPipeline::removeDependency(engine::RenderPass from, engine::RenderPass to) {
    for (auto it = dependencies_.begin(); it != dependencies_.end(); ++it) {
        if (it->from == from && it->to == to) {
            dependencies_.erase(it);
            orderDirty_ = true;
            break;
        }
    }
}

bool RenderPipeline::hasDependency(engine::RenderPass from, engine::RenderPass to) const {
    for (const auto& dep : dependencies_) {
        if (dep.from == from && dep.to == to) return true;
    }
    return false;
}

std::vector<PassDependency> RenderPipeline::getDependencies() const {
    return dependencies_;
}

const std::vector<engine::RenderPass>& RenderPipeline::getPassExecutionOrder() const {
    if (!orderDirty_ && cachedOrder_.has_value()) {
        return *cachedOrder_;
    }

    std::vector<engine::RenderPass> order;
    if (!topologicalSort(order)) {
        core::logError("RenderPipeline: cycle detected in dependencies");
        order = passes_;
    }

    cachedOrder_ = std::move(order);
    orderDirty_ = false;
    return *cachedOrder_;
}

bool RenderPipeline::topologicalSort(std::vector<engine::RenderPass>& outOrder) const {
    const int n = static_cast<int>(passes_.size());
    if (n == 0) return true;

    // passes_ 规模很小（一般 <10），线性查找比 hash 容器更快。
    auto indexOf = [&](engine::RenderPass p) {
        for (int i = 0; i < n; ++i) if (passes_[i] == p) return i;
        return -1;
    };

    std::vector<std::vector<int>> adj(n);
    std::vector<int> inDegree(n, 0);

    for (const auto& dep : dependencies_) {
        int fromIdx = indexOf(dep.from);
        int toIdx   = indexOf(dep.to);
        if (fromIdx < 0 || toIdx < 0) continue;
        adj[fromIdx].push_back(toIdx);
        ++inDegree[toIdx];
    }

    std::queue<int> q;
    for (int i = 0; i < n; ++i) {
        if (inDegree[i] == 0) q.push(i);
    }

    int visited = 0;
    while (!q.empty()) {
        int u = q.front(); q.pop();
        ++visited;
        outOrder.push_back(passes_[u]);
        for (int v : adj[u]) {
            if (--inDegree[v] == 0) q.push(v);
        }
    }

    return visited == n;
}

void RenderPipeline::setPassViewport(engine::RenderPass pass, const Viewport& vp) {
    passStates_[pass].viewport = vp;
}

const Viewport* RenderPipeline::getPassViewport(engine::RenderPass pass) const {
    auto it = passStates_.find(pass);
    if (it != passStates_.end()) return &it->second.viewport;
    return nullptr;
}

void RenderPipeline::setPassCamera(engine::RenderPass pass, const CameraData& cam) {
    passStates_[pass].camera = cam;
}

const CameraData* RenderPipeline::getPassCamera(engine::RenderPass pass) const {
    auto it = passStates_.find(pass);
    if (it != passStates_.end()) return &it->second.camera;
    return nullptr;
}

void RenderPipeline::setPassClear(engine::RenderPass pass, bool enabled, core::Color color) {
    passStates_[pass].clearEnabled = enabled;
    passStates_[pass].clearColor   = color;
}

void RenderPipeline::execute(CommandBuffer& cb, IRenderDevice& device) {
    const auto& order = getPassExecutionOrder();
    if (order.empty()) return;

    // 按 pass 分桶成指针列表：一次扫描，无变体拷贝、无二次 CommandBuffer 录制。
    std::unordered_map<engine::RenderPass, std::vector<const RenderCmd*>> buckets;
    buckets.reserve(order.size());
    for (engine::RenderPass pass : order) buckets[pass];

    for (const auto& cmd : cb.commands()) {
        engine::RenderPass pass;
        if (auto* s = std::get_if<DrawSpriteCmd>(&cmd))      pass = s->pass;
        else if (auto* t = std::get_if<DrawTileCmd>(&cmd))   pass = t->pass;
        else if (auto* tx = std::get_if<DrawTextCmd>(&cmd))  pass = tx->pass;
        else continue; // ClearCmd/SetCameraCmd：swapchain 路径由 PassState 覆盖
        auto it = buckets.find(pass);
        if (it != buckets.end()) it->second.push_back(&cmd);
    }

    for (engine::RenderPass pass : order) {
        const PassState& state = passStates_[pass];
        IRenderDevice::PassSubmitInfo info;
        info.camera       = state.camera;
        info.clearEnabled = state.clearEnabled;
        info.clearColor   = state.clearColor;
        device.submitPass(info, buckets[pass]);
    }
}

} // namespace backend
