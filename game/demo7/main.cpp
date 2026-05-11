#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <engine/api/GameAPI.h>
#include <engine/components/RenderComponents.h>
#include <engine/runtime/EngineConfig.h>
#include <engine/runtime/EngineContext.h>
#include <engine/systems/ISystem.h>

namespace {

	bool hasArg(int argc, char** argv, const char* name) {
		for (int i = 1; i < argc; ++i) {
			if (std::strcmp(argv[i], name) == 0) return true;
		}
		return false;
	}

	const char* getArg(int argc, char** argv, const char* name, const char* defVal) {
		for (int i = 1; i < argc - 1; ++i) {
			if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
		}
		return defVal;
	}

	int getIntArg(int argc, char** argv, const char* name, int defVal) {
		const char* raw = getArg(argc, argv, name, nullptr);
		if (!raw || raw[0] == '\0') return defVal;
		return std::max(1, std::atoi(raw));
	}

	float getFloatArg(int argc, char** argv, const char* name, float defVal) {
		const char* raw = getArg(argc, argv, name, nullptr);
		if (!raw || raw[0] == '\0') return defVal;
		return std::max(1.0f, static_cast<float>(std::atof(raw)));
	}

	std::vector<uint8_t> makeCheckerboard(int w, int h, int cellSize,
		core::Color a, core::Color b) {
		std::vector<uint8_t> px(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
		for (int y = 0; y < h; ++y) {
			for (int x = 0; x < w; ++x) {
				const bool even = (((x / cellSize) + (y / cellSize)) % 2) == 0;
				const core::Color c = even ? a : b;
				const size_t i = static_cast<size_t>(y * w + x) * 4u;
				px[i + 0] = c.r;
				px[i + 1] = c.g;
				px[i + 2] = c.b;
				px[i + 3] = c.a;
			}
		}
		return px;
	}

	class StressCameraSystem final : public engine::ISystem {
	public:
		StressCameraSystem(engine::EngineContext& ctx,
			entt::entity worldCamera,
			float worldMinX,
			float worldMinY,
			float worldMaxX,
			float worldMaxY,
			int fallbackViewportW,
			int fallbackViewportH)
			: ctx_(ctx)
			, worldCamera_(worldCamera)
			, worldMinX_(worldMinX)
			, worldMinY_(worldMinY)
			, worldMaxX_(worldMaxX)
			, worldMaxY_(worldMaxY)
			, fallbackViewportW_(fallbackViewportW)
			, fallbackViewportH_(fallbackViewportH) {
		}

		engine::UpdatePhaseMask phaseMask() const override {
			return engine::updatePhaseBit(engine::UpdatePhase::Camera);
		}

	private:
		void onCameraPhase(float dt) override {
			if (!ctx_.world.valid(worldCamera_)) return;
			if (!ctx_.world.all_of<engine::Transform, engine::Camera>(worldCamera_)) return;

			auto& camTf = ctx_.world.get<engine::Transform>(worldCamera_);
			auto& cam = ctx_.world.get<engine::Camera>(worldCamera_);

			// 这里把验证入口做成相机控制而不是玩家控制：
			// 1. demo7 的目标是验证 culling，而不是 gameplay/physics；
			// 2. 相机直接扫大世界，更容易看到 visible count 的变化；
			// 3. Camera phase 在 Render 前执行，本帧输入会立刻影响裁剪结果。
			if (ctx_.inputState.isKeyJustPressed(SDLK_C)) {
				cam.cullEnabled = !cam.cullEnabled;
			}
			if (ctx_.inputState.isKeyJustPressed(SDLK_R)) {
				camTf.rotation = 0.0f;
			}
			if (ctx_.inputState.isKeyJustPressed(SDLK_HOME)) {
				camTf.x = 0.0f;
				camTf.y = 0.0f;
				camTf.rotation = 0.0f;
				cam.zoom = 1.0f;
			}

			float moveX = 0.0f;
			float moveY = 0.0f;
			if (ctx_.inputState.isKeyDown(SDLK_A) || ctx_.inputState.isKeyDown(SDLK_LEFT))  moveX -= 1.0f;
			if (ctx_.inputState.isKeyDown(SDLK_D) || ctx_.inputState.isKeyDown(SDLK_RIGHT)) moveX += 1.0f;
			if (ctx_.inputState.isKeyDown(SDLK_W) || ctx_.inputState.isKeyDown(SDLK_UP))    moveY -= 1.0f;
			if (ctx_.inputState.isKeyDown(SDLK_S) || ctx_.inputState.isKeyDown(SDLK_DOWN))  moveY += 1.0f;

			const float moveLenSq = moveX * moveX + moveY * moveY;
			if (moveLenSq > 1.0f) {
				const float invLen = 1.0f / std::sqrt(moveLenSq);
				moveX *= invLen;
				moveY *= invLen;
			}

			const bool boosting =
				ctx_.inputState.isKeyDown(SDLK_LSHIFT) || ctx_.inputState.isKeyDown(SDLK_RSHIFT);
			const float moveSpeed = boosting ? kMoveSpeed * kBoostMultiplier : kMoveSpeed;
			camTf.x += moveX * moveSpeed * dt;
			camTf.y += moveY * moveSpeed * dt;

			float zoomDir = 0.0f;
			if (ctx_.inputState.isKeyDown(SDLK_Q) || ctx_.inputState.isKeyDown(SDLK_PAGEUP))   zoomDir -= 1.0f;
			if (ctx_.inputState.isKeyDown(SDLK_E) || ctx_.inputState.isKeyDown(SDLK_PAGEDOWN)) zoomDir += 1.0f;
			if (zoomDir != 0.0f) {
				cam.zoom *= std::exp(zoomDir * kZoomRate * dt);
				cam.zoom = std::clamp(cam.zoom, 0.05f, 8.0f);
			}

			float rotationDir = 0.0f;
			if (ctx_.inputState.isKeyDown(SDLK_Z)) rotationDir -= 1.0f;
			if (ctx_.inputState.isKeyDown(SDLK_X)) rotationDir += 1.0f;
			camTf.rotation += rotationDir * kRotationRate * dt;

			clampCameraToWorld(camTf, cam);
		}

		void clampCameraToWorld(engine::Transform& camTf, const engine::Camera& cam) const {
			const int viewportW = (ctx_.windowWidth > 0) ? ctx_.windowWidth : fallbackViewportW_;
			const int viewportH = (ctx_.windowHeight > 0) ? ctx_.windowHeight : fallbackViewportH_;
			const float zoom = (cam.zoom > 0.0f) ? cam.zoom : 1.0f;

			// 这里的夹紧故意只用未旋转视图的 half extents：
			// demo7 的重点是验证海量 sprite 的裁剪与收集，而不是做一套完美的旋转边界约束。
			// 这样实现简单、可预测，也避免相机转动时被过度收缩到一个很小的区域。
			const float halfViewW = static_cast<float>(viewportW) * 0.5f / zoom;
			const float halfViewH = static_cast<float>(viewportH) * 0.5f / zoom;

			if ((worldMaxX_ - worldMinX_) > halfViewW * 2.0f) {
				camTf.x = std::clamp(camTf.x, worldMinX_ + halfViewW, worldMaxX_ - halfViewW);
			}
			else {
				camTf.x = (worldMinX_ + worldMaxX_) * 0.5f;
			}

			if ((worldMaxY_ - worldMinY_) > halfViewH * 2.0f) {
				camTf.y = std::clamp(camTf.y, worldMinY_ + halfViewH, worldMaxY_ - halfViewH);
			}
			else {
				camTf.y = (worldMinY_ + worldMaxY_) * 0.5f;
			}
		}

		static constexpr float kMoveSpeed = 1800.0f;
		static constexpr float kBoostMultiplier = 4.0f;
		static constexpr float kZoomRate = 1.6f;
		static constexpr float kRotationRate = 1.35f;

		engine::EngineContext& ctx_;
		entt::entity worldCamera_ = entt::null;
		float worldMinX_ = 0.0f;
		float worldMinY_ = 0.0f;
		float worldMaxX_ = 0.0f;
		float worldMaxY_ = 0.0f;
		int fallbackViewportW_ = 1280;
		int fallbackViewportH_ = 720;
	};

} // namespace

int main(int argc, char** argv) {
	const bool useOpenGL = hasArg(argc, argv, "--opengl");
	const bool autoExit = hasArg(argc, argv, "--auto-exit");
	const int gridSize = getIntArg(argc, argv, "--grid-size", 1000);
	const float spacing = getFloatArg(argc, argv, "--spacing", 36.0f);

	engine::EngineConfig cfg;
	cfg.windowTitle = "QGame Demo7 - Sprite Culling Stress Test";
	cfg.windowWidth = 1280;
	cfg.windowHeight = 720;
	cfg.vsync = true;
	cfg.debug = true;
	if (useOpenGL) cfg.renderBackend = engine::RenderBackend::OpenGL;

	engine::EngineContext ctx;
	ctx.init(cfg);
	engine::GameAPI api{ ctx };

	api.loadAssetManifest(QGAME_BAKED_MANIFEST);
	const engine::FontHandle font = api.loadFontById("font.demo.main");

	// 世界相机：只负责 World pass。demo7 里所有压力测试 sprite 都走这台相机。
	const entt::entity worldCamera = api.spawnEntity();
	api.addComponent(worldCamera, engine::Transform{ 0.0f, 0.0f });
	engine::Camera worldCam{};
	worldCam.zoom = 1.0f;
	worldCam.primary = true;
	worldCam.depth = 0;
	worldCam.layerMask = engine::renderPassBit(engine::RenderPass::World);
	worldCam.clear = true;
	worldCam.clearColor = core::Color{ 12, 16, 28, 255 };
	worldCam.pixelSnap = true;
	worldCam.cullEnabled = true;
	api.addComponent(worldCamera, worldCam);

	// UI 相机：承担 DebugOverlay 和状态文字。
	const entt::entity uiCamera = api.spawnEntity();
	api.addComponent(uiCamera, engine::Transform{ 640.0f, 360.0f });
	engine::Camera overlayCam{};
	overlayCam.zoom = 1.0f;
	overlayCam.primary = true;
	overlayCam.depth = 1;
	overlayCam.layerMask = engine::renderPassBit(engine::RenderPass::UI);
	overlayCam.clear = false;
	overlayCam.cullEnabled = false;
	api.addComponent(uiCamera, overlayCam);

	api.createDebugOverlay(font);
	std::printf("\n");
	std::printf("+------------------------------------------------------------------+\n");
	std::printf("| Demo7: Spatial Hash + Precomputed AABB Stress Test               |\n");
	std::printf("+------------------------------------------------------------------+\n");
	std::printf("| grid-size=%d  total-sprites=%u  spacing=%.1f                     |\n",
		gridSize,
		static_cast<uint32_t>(gridSize) * static_cast<uint32_t>(gridSize),
		spacing);
	std::printf("| controls: WASD move, Q/E zoom, Z/X rotate, C toggle culling      |\n");
	std::printf("+------------------------------------------------------------------+\n");

	const auto smallPixels = makeCheckerboard(
		16, 16, 4,
		core::Color{ 235, 236, 240, 255 },
		core::Color{ 110, 145, 230, 255 });
	const TextureHandle spriteTex =
		api.createTextureFromMemory(smallPixels.data(), 16, 16);

	// 把大世界中心放在 (0,0)，这样相机 reset 时就能直接看到场景中心。
	const float startX = -0.5f * static_cast<float>(gridSize - 1) * spacing;
	const float startY = -0.5f * static_cast<float>(gridSize - 1) * spacing;

	for (int gy = 0; gy < gridSize; ++gy) {
		if ((gy % 64) == 0) {
			std::printf("  creating row %d / %d\n", gy, gridSize);
		}

		for (int gx = 0; gx < gridSize; ++gx) {
			const entt::entity e = api.spawnEntity();

			engine::Transform tf{};
			tf.x = startX + static_cast<float>(gx) * spacing;
			tf.y = startY + static_cast<float>(gy) * spacing;

			// 这里故意混入不同 rotation / scale：
			// 1. 让 spatial hash 看到大量跨 cell 的 AABB；
			// 2. 验证预计算 halfW/halfH 公式能正确覆盖旋转对象；
			// 3. 避免 demo 只测“所有 sprite 都是完美 16x16 正方形”的最简单路径。
			if (((gx + gy) % 17) == 0) {
				tf.rotation = 0.78539816339f;
				tf.scaleX = 2.5f;
				tf.scaleY = 0.8f;
			}
			else if ((gx % 11) == 0) {
				tf.rotation = -0.55f;
				tf.scaleX = 1.2f;
				tf.scaleY = 2.2f;
			}
			else if ((gy % 13) == 0) {
				tf.rotation = 0.25f;
				tf.scaleX = 1.8f;
				tf.scaleY = 1.1f;
			}
			else {
				tf.rotation = 0.0f;
				tf.scaleX = 1.0f;
				tf.scaleY = 1.0f;
			}
			api.addComponent(e, tf);

			engine::Sprite sp{};
			sp.texture = spriteTex;
			sp.srcRect = { 0.0f, 0.0f, 16.0f, 16.0f };
			sp.layer = (gx + gy) % 6;
			sp.sortOrder = (gx * 3 + gy * 5) % 32;
			sp.ySort = true;
			sp.pass = engine::RenderPass::World;
			sp.tint = core::Color{
				static_cast<uint8_t>(80 + ((gx * 37 + gy * 11) % 150)),
				static_cast<uint8_t>(90 + ((gx * 17 + gy * 29) % 140)),
				static_cast<uint8_t>(120 + ((gx * 13 + gy * 19) % 120)),
				255
			};
			api.addComponent(e, sp);
		}
	}

	// 世界边界按 sprite 的最大拉伸尺寸略微放大，避免相机边缘裁掉最后一圈对象。
	const float maxSpriteHalfExtent = 24.0f;
	const float worldMinX = startX - maxSpriteHalfExtent;
	const float worldMinY = startY - maxSpriteHalfExtent;
	const float worldMaxX = startX + static_cast<float>(gridSize - 1) * spacing + maxSpriteHalfExtent;
	const float worldMaxY = startY + static_cast<float>(gridSize - 1) * spacing + maxSpriteHalfExtent;

	auto& stressSystem = ctx.systems.registerSystem<StressCameraSystem>(
		ctx,
		worldCamera,
		worldMinX,
		worldMinY,
		worldMaxX,
		worldMaxY,
		cfg.windowWidth,
		cfg.windowHeight);
	stressSystem.init();

	float elapsed = 0.0f;
	while (ctx.scheduler.tick()) {
		elapsed += ctx.scheduler.deltaTime();

		if (api.isKeyJustPressed(SDLK_ESCAPE)) {
			api.quit();
			break;
		}
		if (autoExit && elapsed >= 2.0f) {
			api.quit();
			break;
		}
	}

	ctx.shutdown();
	return 0;
}
