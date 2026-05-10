#include "DebugOverlaySystem.h"
#include "../components/TextComponent.h"
#include "../../backend/renderer/IRenderDevice.h"
#include <cstdio>

namespace engine {

DebugOverlaySystem::DebugOverlaySystem(EngineContext& ctx) : ctx_(ctx) {}

UpdatePhaseMask DebugOverlaySystem::phaseMask() const {
    return updatePhaseBit(UpdatePhase::UI);
}

void DebugOverlaySystem::init() {
}

bool DebugOverlaySystem::runPhase(UpdatePhase phase, float dt) {
    if (phase != UpdatePhase::UI) return true;

    auto view = ctx_.world.view<DebugOverlayComponent, TextComponent>();
    
    // Check if we need to update (e.g. 4 times a second)
    for (auto [ent, overlay, label] : view.each()) {
        overlay.fpsAccumulator += (dt > 0.f ? (1.0f / dt) : 0.0f);
        overlay.fpsFrames++;
        overlay.updateTimer += dt;
        
        if (overlay.updateTimer >= 0.25f) {
            float avgFps = overlay.fpsAccumulator / static_cast<float>(overlay.fpsFrames);
            const auto& stats = ctx_.frameStats;
            
            char buf[512];
            std::snprintf(buf, sizeof(buf),
                "FPS: %.1f\n"
                "Render Path: %s\n"
                "Sprites (Total/Vis): %u / %u\n"
                "Cull Cells/Candidates: %u / %u\n"
                "CPU syncEntities: %u us\n"
                "CPU mixedGpuSync: %u us\n"
                "CPU cullCollect:  %u us\n"
                "CPU cullSort:     %u us\n"
                "CPU cullIndex:    %u us\n"
                "CPU total:        %u us\n"
                "Draw Calls: %u\n"
                "GPU Batches: %u",
                avgFps,
                backend::renderPathName(stats.path),
                stats.spriteCount, stats.visibleSpriteCount,
                stats.cullingCoveredCellCount, stats.cullingCandidateSpriteCount,
                stats.syncEntitiesUs,
                stats.mixedGpuSyncUs,
                stats.cullingCollectUs,
                stats.cullingSortUs,
                stats.cullingIndexUs,
                stats.totalCpuUs,
                stats.drawCallCount, stats.gpuDrawBatchCount);
                
            label.text = buf;
            
            overlay.updateTimer = 0.0f;
            overlay.fpsAccumulator = 0.0f;
            overlay.fpsFrames = 0;
        }
    }
    
    return true;
}

} // namespace engine
