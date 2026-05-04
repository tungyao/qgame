#include "FrameScheduler.h"

#include <SDL3/SDL.h>

#include "EngineContext.h"
#include "../../backend/renderer/IRenderDevice.h"
#include "../../core/Logger.h"
#include "../systems/InputSystem.h"
#include "../systems/PhysicsSystem.h"
#include "../systems/AnimatorSystem.h"

namespace engine {

	bool FrameScheduler::tick() {
		const uint64_t now = SDL_GetTicks();
		if (lastTick_ == 0) {
			lastTick_ = now;
		}

		float dt = static_cast<float>(now - lastTick_) / 1000.0f;
		if (dt > 0.1f) {
			dt = 0.1f;
		}

		lastTick_ = now;
		lastDt_ = dt;
		ctx_.deltaTime = dt;
		ctx_.frameCounter = frameCount_;

		if (ctx_.systems.has<InputSystem>()) {
			if (!ctx_.systems.get<InputSystem>().pollInput()) {
				return false;
			}
		}

		ctx_.renderDevice().beginFrame();

		for (auto& system : ctx_.systems.systems()) {
			system->preUpdate();
		}

		if (ctx_.systems.has<PhysicsSystem>()) {
			ctx_.systems.get<PhysicsSystem>().update(dt);
		}
		if (ctx_.systems.has<AnimatorSystem>()) {
			ctx_.systems.get<AnimatorSystem>().update(dt);
		}

		for (auto& system : ctx_.systems.systems()) {
			if (!system->isManuallyScheduled()) {
				system->update(dt);
			}
		}

		if (ctx_.beforePresentCallback) {
			ctx_.beforePresentCallback();
		}

		ctx_.renderDevice().present();

		for (auto& system : ctx_.systems.systems()) {
			system->postUpdate();
		}

		ctx_.renderDevice().endFrame();

		if (frameCount_ > 0 && frameCount_ % 300 == 0 && dt > 0.0f) {
			const backend::RenderFrameStats& rs = ctx_.renderDevice().frameStats();
			core::logInfo(
				"frame %llu  fps %.1f  render=%s sprites=%u visible=%u draws=%u gpuBatches=%u uploads=%u/%lluB compute=%u%s%s",
				static_cast<unsigned long long>(frameCount_),
				1.0f / dt,
				backend::renderPathName(rs.path),
				rs.spriteCount,
				rs.visibleSpriteCount,
				rs.drawCallCount,
				rs.gpuDrawBatchCount,
				rs.uploadCallCount,
				static_cast<unsigned long long>(rs.uploadBytes),
				rs.computeDispatchCount,
				rs.fallbackReason ? " fallback=" : "",
				rs.fallbackReason ? rs.fallbackReason : "");
		}

		++frameCount_;
		return true;
	}

} // namespace engine
