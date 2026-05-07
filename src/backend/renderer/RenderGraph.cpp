#include "RenderGraph.h"

#include <utility>

namespace backend {

uint32_t RenderGraph::addResource(RenderGraphResource desc) {
    resources_.push_back(std::move(desc));
    return static_cast<uint32_t>(resources_.size() - 1u);
}

uint32_t RenderGraph::addPass(RenderGraphPass pass) {
    passes_.push_back(std::move(pass));
    return static_cast<uint32_t>(passes_.size() - 1u);
}

void RenderGraph::clear() {
    resources_.clear();
    passes_.clear();
}

const char* renderGraphAccessName(RenderGraphAccess access) {
    switch (access) {
        case RenderGraphAccess::ReadSampled: return "ReadSampled";
        case RenderGraphAccess::ReadStorage: return "ReadStorage";
        case RenderGraphAccess::WriteColor:  return "WriteColor";
        case RenderGraphAccess::WriteStorage:return "WriteStorage";
        case RenderGraphAccess::Present:     return "Present";
        default:                             return "Unknown";
    }
}

} // namespace backend
