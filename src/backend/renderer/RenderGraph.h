#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace backend {

// RenderGraph in this engine is intentionally small for the first lighting
// migration. It now compiles the declared pass/resource contract into a stable
// execution schedule with per-resource access transitions. Backends can consume
// those transitions as real API barriers where supported, or as validation and
// pass-boundary documentation on APIs such as SDL GPU.
//
// The first consumer is SDL GPU 2D lighting:
//   WorldColorPass -> LightingCompute -> LightingCompositePass -> UIPass
//
// Later we can add resource aliasing and pass reordering without changing
// RenderSystem's high-level submission shape again.
enum class RenderGraphResourceType : uint8_t {
    Texture,
    Buffer,
    Swapchain,
};

enum class RenderGraphAccess : uint8_t {
    ReadSampled,
    ReadStorage,
    WriteColor,
    WriteStorage,
    Present,
};

struct RenderGraphResource {
    RenderGraphResourceType type = RenderGraphResourceType::Texture;
    std::string name;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct RenderGraphResourceUse {
    uint32_t resource = 0;
    RenderGraphAccess access = RenderGraphAccess::ReadSampled;
};

struct RenderGraphPass {
    std::string name;
    std::vector<RenderGraphResourceUse> reads;
    std::vector<RenderGraphResourceUse> writes;
};

struct RenderGraphBarrier {
    uint32_t resource = 0;
    RenderGraphAccess before = RenderGraphAccess::ReadSampled;
    RenderGraphAccess after = RenderGraphAccess::ReadSampled;
    uint32_t beforePass = UINT32_MAX;
    uint32_t afterPass = UINT32_MAX;
};

struct RenderGraphCompiledPass {
    uint32_t passIndex = 0;
    std::vector<RenderGraphBarrier> barriersBefore;
};

struct RenderGraphResourceState {
    bool initialized = false;
    RenderGraphAccess lastAccess = RenderGraphAccess::ReadSampled;
    uint32_t firstPass = UINT32_MAX;
    uint32_t lastPass = UINT32_MAX;
};

struct CompiledRenderGraph {
    bool valid = true;
    std::string error;
    std::vector<RenderGraphCompiledPass> passes;
    std::vector<RenderGraphResourceState> finalStates;
};

class RenderGraph {
public:
    // Add a resource declaration and return its stable index. The index is only
    // valid for this graph instance; backend-owned texture handles remain in the
    // backend. This separation keeps the first graph backend-agnostic.
    uint32_t addResource(RenderGraphResource desc);

    // Add a pass declaration. Execution order is the declaration order for now;
    // compile() validates resource indices and builds the access-transition list
    // that a backend can map to barriers before each pass.
    uint32_t addPass(RenderGraphPass pass);

    CompiledRenderGraph compile() const;

    void clear();

    const std::vector<RenderGraphResource>& resources() const { return resources_; }
    const std::vector<RenderGraphPass>& passes() const { return passes_; }

private:
    std::vector<RenderGraphResource> resources_;
    std::vector<RenderGraphPass> passes_;
};

const char* renderGraphAccessName(RenderGraphAccess access);
const char* renderGraphResourceTypeName(RenderGraphResourceType type);
bool renderGraphAccessIsWrite(RenderGraphAccess access);

} // namespace backend
