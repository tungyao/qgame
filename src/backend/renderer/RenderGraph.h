#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace backend {

// RenderGraph in this engine is intentionally small for the first lighting
// migration. It is not a full frame scheduler yet: it records the pass/resource
// contract in one place so the backend executes a stable, inspectable order.
//
// The first consumer is SDL GPU 2D lighting:
//   WorldColorPass -> LightingCompute -> LightingCompositePass -> UIPass
//
// Later we can add resource aliasing, pass reordering, and automatic barriers
// without changing RenderSystem's high-level submission shape again.
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

class RenderGraph {
public:
    // Add a resource declaration and return its stable index. The index is only
    // valid for this graph instance; backend-owned texture handles remain in the
    // backend. This separation keeps the first graph backend-agnostic.
    uint32_t addResource(RenderGraphResource desc);

    // Add a pass declaration. Execution is still hard-coded by the backend for
    // now; the graph records the intended order and dependencies for stats and
    // debugging, which is the immediate need for the lighting migration.
    uint32_t addPass(RenderGraphPass pass);

    void clear();

    const std::vector<RenderGraphResource>& resources() const { return resources_; }
    const std::vector<RenderGraphPass>& passes() const { return passes_; }

private:
    std::vector<RenderGraphResource> resources_;
    std::vector<RenderGraphPass> passes_;
};

const char* renderGraphAccessName(RenderGraphAccess access);

} // namespace backend
