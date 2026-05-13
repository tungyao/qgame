#include "DebugOverlaySystem.h"
#include "../components/TextComponent.h"
#include "../components/PhysicsComponents.h"
#include "../components/RenderComponents.h"
#include "../../backend/renderer/IRenderDevice.h"
#include <cstdio>
#include <cmath>

namespace engine {

	DebugOverlaySystem::DebugOverlaySystem(EngineContext& ctx) : ctx_(ctx) {}

	UpdatePhaseMask DebugOverlaySystem::phaseMask() const {
		return updatePhaseBit(UpdatePhase::UI);
	}

	void DebugOverlaySystem::init() {
		debugEnabled_ = ctx_.debug;
	}

	void DebugOverlaySystem::shutdown() {
		if (debugWhiteTexture_.valid()) {
			ctx_.renderDevice().destroyTexture(debugWhiteTexture_);
			debugWhiteTexture_ = {};
		}
	}

	void DebugOverlaySystem::setDebugEnabled(bool enabled) {
		debugEnabled_ = enabled;
	}

	void DebugOverlaySystem::ensureDebugTexture() {
		if (debugWhiteTexture_.valid()) return;
		uint8_t whitePixel[4] = { 255, 255, 255, 255 };
		backend::TextureDesc desc;
		desc.width = 1;
		desc.height = 1;
		desc.channels = 4;
		desc.data = whitePixel;
		desc.filter = backend::TextureFilter::Nearest;
		debugWhiteTexture_ = ctx_.renderDevice().createTexture(desc);
	}

	void DebugOverlaySystem::collectColliderDebugRects() {
		colliderRects_.clear();
		if (!debugEnabled_) return;

		auto view = ctx_.world.view<Transform, Collider>();
		for (auto [ent, tf, collider] : view.each()) {
			if (collider.width <= 0.f || collider.height <= 0.f) continue;

			float rx, ry;
			if (const Sprite* sprite = ctx_.world.try_get<Sprite>(ent)) {
				const float spriteW = sprite->srcRect.w * std::abs(tf.scaleX);
				const float spriteH = sprite->srcRect.h * std::abs(tf.scaleY);
				rx = tf.x - sprite->pivotX * spriteW + collider.offsetX;
				ry = tf.y - sprite->pivotY * spriteH + collider.offsetY;
			}
			else {
				rx = tf.x + collider.offsetX;
				ry = tf.y + collider.offsetY;
			}

			core::Color color = collider.isTrigger
				? core::Color{ 0, 200, 255, 200 }
			: core::Color{ 0, 255, 0, 200 };

			colliderRects_.push_back({ rx, ry, collider.width, collider.height, color });
		}
	}

	bool DebugOverlaySystem::runPhase(UpdatePhase phase, float dt) {
		if (phase != UpdatePhase::UI) return true;

		// Collect collider debug rects from the current world state
		collectColliderDebugRects();

		// Update FPS/stats text overlay
		auto view = ctx_.world.view<DebugOverlayComponent, TextComponent>();
		for (auto [ent, overlay, label] : view.each()) {
			overlay.fpsAccumulator += (dt > 0.f ? (1.0f / dt) : 0.0f);
			overlay.fpsFrames++;
			overlay.updateTimer += dt;

			if (overlay.updateTimer >= 0.25f) {
				float avgFps = overlay.fpsAccumulator / static_cast<float>(overlay.fpsFrames);
				const auto& stats = ctx_.frameStats;

				char buf[768];
				int len = std::snprintf(buf, sizeof(buf),
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
					"GPU Batches: %u\n"
					"Debug Rects: %zu",
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
					stats.drawCallCount, stats.gpuDrawBatchCount,
					colliderRects_.size());

				if (len > 0 && !customInfo_.empty()) {
					std::snprintf(buf + len, sizeof(buf) - static_cast<size_t>(len),
						"\n%s", customInfo_.c_str());
				}

				label.text = buf;

				overlay.updateTimer = 0.0f;
				overlay.fpsAccumulator = 0.0f;
				overlay.fpsFrames = 0;
			}
		}

		return true;
	}

	static void emitRectOutline(backend::CommandBuffer& cb, const TextureHandle& tex,
		const DebugRect& rect, float lineThickness) {
		backend::DrawSpriteCmd edge;
		edge.texture = tex;
		edge.srcRect = { 0.f, 0.f, 1.f, 1.f };
		edge.pivotX = 0.f;
		edge.pivotY = 0.f;
		edge.tint = rect.color;
		edge.layer = 1000;
		edge.pass = RenderPass::World;

		// Top edge
		edge.scaleX = rect.w;
		edge.scaleY = lineThickness;
		edge.x = rect.x;
		edge.y = rect.y;
		cb.drawSprite(edge);

		// Bottom edge
		edge.y = rect.y + rect.h - lineThickness;
		cb.drawSprite(edge);

		// Left edge
		edge.scaleX = lineThickness;
		edge.scaleY = rect.h;
		edge.x = rect.x;
		edge.y = rect.y;
		cb.drawSprite(edge);

		// Right edge
		edge.x = rect.x + rect.w - lineThickness;
		cb.drawSprite(edge);
	}

	void DebugOverlaySystem::emitDebugDrawCommands(backend::CommandBuffer& cb) {
		if (!debugEnabled_) return;
		if (colliderRects_.empty() && debugRects_.empty()) return;

		ensureDebugTexture();
		if (!debugWhiteTexture_.valid()) return;

		// Auto-collected collider rects
		for (const DebugRect& rect : colliderRects_) {
			emitRectOutline(cb, debugWhiteTexture_, rect, lineThickness_);
		}

		// Custom debug rects added by other systems
		for (const DebugRect& rect : debugRects_) {
			emitRectOutline(cb, debugWhiteTexture_, rect, lineThickness_);
		}
		debugRects_.clear();
	}

} // namespace engine
