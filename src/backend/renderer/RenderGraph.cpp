#include "RenderGraph.h"

#include <cstdio>
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

CompiledRenderGraph RenderGraph::compile() const {
    CompiledRenderGraph compiled;
    compiled.passes.reserve(passes_.size());
    compiled.finalStates.resize(resources_.size());

    auto fail = [&](const char* passName, uint32_t passIndex,
                    uint32_t resourceIndex, const char* reason) {
        compiled.valid = false;
        char buffer[256];
        std::snprintf(buffer, sizeof(buffer),
                      "RenderGraph compile failed at pass %u (%s), resource %u: %s",
                      passIndex,
                      passName ? passName : "<unnamed>",
                      resourceIndex,
                      reason ? reason : "unknown error");
        compiled.error = buffer;
    };

    auto visitUse = [&](RenderGraphCompiledPass& compiledPass,
                        const RenderGraphPass& pass,
                        uint32_t passIndex,
                        const RenderGraphResourceUse& use) {
        if (!compiled.valid) return;
        if (use.resource >= resources_.size()) {
            fail(pass.name.c_str(), passIndex, use.resource, "resource index out of range");
            return;
        }

        RenderGraphResourceState& state = compiled.finalStates[use.resource];
        const bool isWrite = renderGraphAccessIsWrite(use.access);
        if (!state.initialized) {
            // Reading a swapchain is treated as reading the previous color/load
            // value. Other resources must be written before their first read.
            if (!isWrite && resources_[use.resource].type != RenderGraphResourceType::Swapchain) {
                fail(pass.name.c_str(), passIndex, use.resource, "resource read before first write");
                return;
            }
            state.initialized = true;
            state.lastAccess = use.access;
            state.firstPass = passIndex;
            state.lastPass = passIndex;
            return;
        }

        const bool previousWrite = renderGraphAccessIsWrite(state.lastAccess);
        const bool needsBarrier =
            state.lastAccess != use.access || previousWrite || isWrite;
        if (needsBarrier) {
            compiledPass.barriersBefore.push_back(RenderGraphBarrier{
                use.resource,
                state.lastAccess,
                use.access,
                state.lastPass,
                passIndex
            });
        }

        state.lastAccess = use.access;
        state.lastPass = passIndex;
    };

    for (uint32_t passIndex = 0; passIndex < passes_.size(); ++passIndex) {
        const RenderGraphPass& pass = passes_[passIndex];
        RenderGraphCompiledPass compiledPass;
        compiledPass.passIndex = passIndex;

        // Reads are visited before writes to model a pass that loads/samples
        // previous contents and then writes a new value, such as UI over the
        // already-composited swapchain.
        for (const RenderGraphResourceUse& read : pass.reads) {
            visitUse(compiledPass, pass, passIndex, read);
        }
        for (const RenderGraphResourceUse& write : pass.writes) {
            visitUse(compiledPass, pass, passIndex, write);
        }

        if (!compiled.valid) return compiled;
        compiled.passes.push_back(std::move(compiledPass));
    }

    return compiled;
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

const char* renderGraphResourceTypeName(RenderGraphResourceType type) {
    switch (type) {
        case RenderGraphResourceType::Texture:   return "Texture";
        case RenderGraphResourceType::Buffer:    return "Buffer";
        case RenderGraphResourceType::Swapchain: return "Swapchain";
        default:                                 return "Unknown";
    }
}

bool renderGraphAccessIsWrite(RenderGraphAccess access) {
    switch (access) {
        case RenderGraphAccess::WriteColor:
        case RenderGraphAccess::WriteStorage:
            return true;
        case RenderGraphAccess::ReadSampled:
        case RenderGraphAccess::ReadStorage:
        case RenderGraphAccess::Present:
        default:
            return false;
    }
}

} // namespace backend
