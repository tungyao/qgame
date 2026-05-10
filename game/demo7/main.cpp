#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cmath>

#include <engine/anim/SpringValue.h>
#include <engine/api/GameAPI.h>
#include <engine/components/PhysicsComponents.h>
#include <engine/components/RenderComponents.h>
#include <engine/runtime/EngineConfig.h>
#include <engine/runtime/EngineContext.h>
#include <engine/systems/ISystem.h>
#include <engine/systems/PhysicsSystem.h>
#include <engine/runtime/TransformInterpolation.h>

#include <stb_image.h>
#include <nlohmann/json.hpp>
namespace {


    static bool hasArg(int argc, char** argv, const char* name) {
        for (int i = 1; i < argc; ++i) {
            if (std::strcmp(argv[i], name) == 0) return true;
        }
        return false;
    }

    static const char* getArg(int argc, char** argv, const char* name, const char* defVal) {
        for (int i = 1; i < argc - 1; ++i) {
            if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
        }
        return defVal;
    }
    static std::vector<uint8_t> makeCheckerboard(int w, int h, int cellSize, core::Color a, core::Color b) {
        std::vector<uint8_t> px(w * h * 4);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                bool even = ((x / cellSize) + (y / cellSize)) % 2 == 0;
                core::Color c = even ? a : b;
                int i = (y * w + x) * 4;
                px[i] = c.r; px[i + 1] = c.g; px[i + 2] = c.b; px[i + 3] = c.a;
            }
        return px;
    }

    entt::entity makeText(engine::GameAPI& api,
                      engine::FontHandle font,
                      const std::string& value,
                      float x,
                      float y,
                      float size,
                      core::Color color) {
        auto e = api.spawnEntity();
        api.addComponent(e, engine::Transform{x, y});

        engine::TextComponent text{};
        text.text = value;
        text.font = font;
        text.fontSize = size;
        text.color = color;
        text.pass = engine::RenderPass::UI;
        text.layer = 60;
        api.addComponent(e, text);
        return e;
    }
    class CameraFollowSystem final : public engine::ISystem {
	public:
		CameraFollowSystem(engine::EngineContext& ctx,
			entt::entity player,
			entt::entity camera,
			float mapPixelW,
			float mapPixelH,
			int tileSize,
			int fallbackViewportW,
			int fallbackViewportH)
			: ctx_(ctx)
			, player_(player)
			, camera_(camera)
			, mapPixelW_(mapPixelW)
			, mapPixelH_(mapPixelH)
			, tileSize_(tileSize)
			, fallbackViewportW_(fallbackViewportW)
			, fallbackViewportH_(fallbackViewportH) {
		}

		engine::UpdatePhaseMask phaseMask() const override {
			return engine::updatePhaseBits({
				engine::UpdatePhase::GameplayPrePhysics,
				engine::UpdatePhase::Camera
				});
		}

		void init() override {
			camFollow.setCritical(50.f);
			snapCameraToPlayer();
			// 让弹簧的初始 value 和 target 都对齐到首帧镜头位置，
			// 避免 onCameraPhase 第一次 update 时从 (0,0) 开始收敛。
			if (ctx_.world.valid(camera_) && ctx_.world.all_of<engine::Transform>(camera_)) {
				const auto& camTf = ctx_.world.get<engine::Transform>(camera_);
				camFollow.snap({ camTf.x, camTf.y });
			}
		}

	private:
		engine::SpringValueVec2 camFollow{};

		void onGameplayPrePhysicsPhase(float /*dt*/) override {
			if (!ctx_.world.valid(player_) || !ctx_.world.all_of<engine::RigidBody>(player_)) {
				return;
			}

			float dx = 0.f;
			float dy = 0.f;
			if (ctx_.inputState.isKeyDown(SDLK_W) || ctx_.inputState.isKeyDown(SDLK_UP)) dy -= 1.f;
			if (ctx_.inputState.isKeyDown(SDLK_S) || ctx_.inputState.isKeyDown(SDLK_DOWN)) dy += 1.f;
			if (ctx_.inputState.isKeyDown(SDLK_A) || ctx_.inputState.isKeyDown(SDLK_LEFT)) dx -= 1.f;
			if (ctx_.inputState.isKeyDown(SDLK_D) || ctx_.inputState.isKeyDown(SDLK_RIGHT)) dx += 1.f;

			// 归一化对角线输入，避免斜向速度比水平/垂直快 sqrt(2)。
			const float lenSq = dx * dx + dy * dy;
			if (lenSq > 1.0f) {
				const float invLen = 1.0f / std::sqrt(lenSq);
				dx *= invLen;
				dy *= invLen;
			}

			auto& rb = ctx_.world.get<engine::RigidBody>(player_);
			rb.velocityX = dx * kPlayerSpeed;
			rb.velocityY = dy * kPlayerSpeed;
		}

		void onCameraPhase(float dt) override {
			if (!ctx_.world.valid(player_) || !ctx_.world.valid(camera_)) {
				return;
			}

			if (!ctx_.world.all_of<engine::Transform>(player_) ||
				!ctx_.world.all_of<engine::Transform, engine::Camera>(camera_)) {
				return;
			}

			auto& camTf = ctx_.world.get<engine::Transform>(camera_);
			auto& cam = ctx_.world.get<engine::Camera>(camera_);
			const auto& playerTf = ctx_.world.get<engine::Transform>(player_);

			// 这里故意读取 EngineContext 缓存，而不是直接调用 platform::Window：
			// - demo 目标只链接 engine，不应额外依赖 platform 符号；
			// - FrameScheduler 会在 Input phase 后同步 resize 结果，因此 Camera phase
			//   看到的是当前帧最新窗口尺寸。
			const int viewportW = (ctx_.windowWidth > 0) ? ctx_.windowWidth : fallbackViewportW_;
			const int viewportH = (ctx_.windowHeight > 0) ? ctx_.windowHeight : fallbackViewportH_;
			const float effectiveZoom = applyAutoCameraZoom(cam, viewportW, viewportH);

			const auto target = computeObservedPlayerPosition(playerTf);
			camFollow.target({ target.x, target.y });
			core::Vec2 camPos = camFollow.update(dt);
			camTf.x = camPos.x;
			camTf.y = camPos.y;

			clampCameraToMap(camTf, effectiveZoom, viewportW, viewportH);

			// Transform 被 PhysicsSystem/RenderSystem 订阅；跟随镜头自己改位置后需要 patch，
			// 这样 GPU 路径和任何依赖 on_update<Transform> 的逻辑都能拿到最新结果。
			ctx_.world.patch<engine::Transform>(camera_);
		}

		void snapCameraToPlayer() {
			if (!ctx_.world.valid(player_) || !ctx_.world.valid(camera_)) {
				return;
			}
			if (!ctx_.world.all_of<engine::Transform>(player_) ||
				!ctx_.world.all_of<engine::Transform, engine::Camera>(camera_)) {
				return;
			}

			auto& camTf = ctx_.world.get<engine::Transform>(camera_);
			auto& cam = ctx_.world.get<engine::Camera>(camera_);
			const auto& playerTf = ctx_.world.get<engine::Transform>(player_);

			const int viewportW = (ctx_.windowWidth > 0) ? ctx_.windowWidth : fallbackViewportW_;
			const int viewportH = (ctx_.windowHeight > 0) ? ctx_.windowHeight : fallbackViewportH_;
			const float effectiveZoom = applyAutoCameraZoom(cam, viewportW, viewportH);

			// 初始化阶段不做缓动，直接把镜头放到玩家上，再按地图边界夹紧。
			// 这样首帧渲染和后续 Camera phase 的结果保持一致，不会先看到旧相机。
			const auto target = computeObservedPlayerPosition(playerTf);
			camTf.x = target.x;
			camTf.y = target.y;
			clampCameraToMap(camTf, effectiveZoom, viewportW, viewportH);

			// 这里必须 patch。直接改引用不会触发 Transform 的 on_update 回调，
			// RenderSystem 首帧可能仍使用旧相机缓存，视觉上就像“开场没对准”。
			ctx_.world.patch<engine::Transform>(camera_);
		}

		engine::Transform computeObservedPlayerPosition(const engine::Transform& playerTf) const {
			if (!ctx_.systems.has<engine::PhysicsSystem>()) {
				return playerTf;
			}

			// demo6 的相机应当跟随“本帧真正会被渲染出来的玩家位置”，而不是固定步物理真值。
			// 这样 Camera phase、RenderSystem、UIWorldAnchor 会共享同一个表现时刻，
			// 不会再出现玩家和镜头各自平滑、但彼此相位不一致的顿挫感。
			const float alpha = ctx_.systems.get<engine::PhysicsSystem>().interpolationAlpha();
			return engine::sampleInterpolatedTransform(
				playerTf,
				ctx_.world.try_get<engine::TransformInterpolation>(player_),
				alpha);
		}

		float computeAutoEffectiveZoom(int viewportW, int viewportH) const {
			const float paddedMapW = mapPixelW_ + tileSize_ * kZoomPaddingTiles * 2.0f;
			const float paddedMapH = mapPixelH_ + tileSize_ * kZoomPaddingTiles * 2.0f;
			if (viewportW <= 0 || viewportH <= 0 || paddedMapW <= 0.0f || paddedMapH <= 0.0f) {
				return 1.0f;
			}

			const float fitZoomX = static_cast<float>(viewportW) / paddedMapW;
			const float fitZoomY = static_cast<float>(viewportH) / paddedMapH;
			return std::max(0.05f, std::min(fitZoomX, fitZoomY));
		}

		float applyAutoCameraZoom(engine::Camera& cam, int viewportW, int viewportH) const {
			const float desiredEffectiveZoom = computeAutoEffectiveZoom(viewportW, viewportH);
			const float referenceH =
				(cam.referenceViewportHeight > 1.0f) ? cam.referenceViewportHeight : 1.0f;

			if (cam.projectionMode == engine::CameraProjectionMode::FixedVertical) {
				const float viewportScale = (viewportH > 0)
					? static_cast<float>(viewportH) / referenceH
					: 1.0f;
				cam.zoom = (viewportScale > 0.0f)
					? (desiredEffectiveZoom / viewportScale)
					: desiredEffectiveZoom;
			}
			else {
				cam.zoom = desiredEffectiveZoom;
			}
			return desiredEffectiveZoom;
		}

		void clampCameraToMap(engine::Transform& camTf, float effectiveZoom,
			int viewportW, int viewportH) const {
			if (effectiveZoom <= 0.0f || viewportW <= 0 || viewportH <= 0) return;

			const float visibleWorldW = static_cast<float>(viewportW) / effectiveZoom;
			const float visibleWorldH = static_cast<float>(viewportH) / effectiveZoom;
			const float halfViewW = visibleWorldW * 0.5f;
			const float halfViewH = visibleWorldH * 0.5f;

			if (mapPixelW_ > visibleWorldW) {
				camTf.x = std::clamp(camTf.x, halfViewW, mapPixelW_ - halfViewW);
			}

			if (mapPixelH_ > visibleWorldH) {
				camTf.y = std::clamp(camTf.y, halfViewH, mapPixelH_ - halfViewH);
			}

			// 当地图本身比当前视野还小时，不要再把镜头硬锁回地图中心。
			// 否则玩家一旦离开中心点，跟随结果会立刻被“夹回中心”覆盖，看起来就像
			// 镜头完全没有跟随。这里选择允许 overscan，优先保证跟随语义成立。
		}

		static constexpr float kPlayerSpeed = 200.0f;
		static constexpr float kZoomPaddingTiles = 1.5f;
		static constexpr float kCameraFollowRate = 7.5f;

		engine::EngineContext& ctx_;
		entt::entity player_ = entt::null;
		entt::entity camera_ = entt::null;
		float mapPixelW_ = 0.0f;
		float mapPixelH_ = 0.0f;
		int tileSize_ = 0;
		int fallbackViewportW_ = 1280;
		int fallbackViewportH_ = 720;
	};

}



int main(int argc, char** argv) {


	const bool useOpenGL = hasArg(argc, argv, "--opengl");
	const bool autoExit = hasArg(argc, argv, "--auto-exit");
	const char* pkgPath = getArg(argc, argv, "--package", "tilemap_engine_package.json");

	engine::EngineConfig cfg;
	cfg.windowTitle = "QGame Demo6 — TileMap Engine Package Loader";
	cfg.windowWidth = 1280;
	cfg.windowHeight = 720;
	cfg.vsync = true;
	cfg.debug = true;
	if (useOpenGL) cfg.renderBackend = engine::RenderBackend::OpenGL;

	engine::EngineContext ctx;
	ctx.init(cfg);
	engine::GameAPI api{ ctx };
    api.loadAssetManifest(QGAME_BAKED_MANIFEST);
    engine::FontHandle font = api.loadFontById("font.demo.main");

	// ── camera ───────────────────────────────────────────────────────────────
	auto camEnt = api.spawnEntity();
	// 相机先放一个临时位置。真正的跟随逻辑会在 player 创建后，把它平滑拉向玩家。
	api.addComponent(camEnt, engine::Transform{ 640.f, 360.f });
	engine::Camera wcam{};
	// zoom 不再写死；主循环里会按窗口尺寸和地图尺寸自动解算。
	wcam.zoom = 1.f;
	wcam.primary = true;
	wcam.depth = 0;
	wcam.layerMask = engine::renderPassBit(engine::RenderPass::World);
	wcam.clear = true;
	wcam.clearColor = core::Color{ 8, 12, 22, 255 };
	wcam.pixelSnap = true;    // 开启像素对齐，防止次像素抖动
	api.addComponent(camEnt, wcam);



    auto uiCam = api.spawnEntity();
    api.addComponent(uiCam, engine::Transform{640.f, 360.f});
    engine::Camera ucam{};
    ucam.zoom = 1.f;
    ucam.primary = true;
    ucam.depth = 1;
    ucam.layerMask = engine::renderPassBit(engine::RenderPass::UI);
    ucam.clear = false;
    ucam.cullEnabled = false;
    api.addComponent(uiCam, ucam);

    //makeText(api, font,
    //         "Moving lantern casts tree shadows; water uses sceneColor SSR for trees and light.",
    //         28.f, 68.f, 14.f, core::Color{176, 198, 220, 255});

    api.createDebugOverlay(font);

	constexpr bool CREATE_MANY_SPRITES = true;
	constexpr int SPRITE_GRID_SIZE = 150;  // 30x30 = 900 sprites (可调大到 50x50=2500 测试性能)

	TextureHandle smallTex;  // 用于性能测试的纹理

	if (CREATE_MANY_SPRITES) {
		printf("\n");
		printf("+----------------------------------------------------------------+\n");
		printf("|         GPU-Driven 2D Rendering Architecture Test              |\n");
		printf("+----------------------------------------------------------------+\n");
		printf("|  Creating %4d sprites in a grid for stress testing...        |\n", SPRITE_GRID_SIZE * SPRITE_GRID_SIZE);
		printf("+----------------------------------------------------------------+\n");

		auto smallPx = makeCheckerboard(16, 16, 4, { 200, 200, 100, 255 }, { 100, 100, 200, 255 });
		smallTex = api.createTextureFromMemory(smallPx.data(), 16, 16);

		float startX = -800.f;
		float startY = -400.f;
		float spacing = 40.f;

		// 创建网格精灵
		for (int gy = 0; gy < SPRITE_GRID_SIZE; ++gy) {
			for (int gx = 0; gx < SPRITE_GRID_SIZE; ++gx) {
				auto e = api.spawnEntity();
				float x = startX + gx * spacing;
				float y = startY + gy * spacing;
				api.addComponent(e, engine::Transform{ x, y });

				engine::Sprite sp{};
				sp.texture = smallTex;
				sp.srcRect = { 0.f, 0.f, 16.f, 16.f };
				sp.layer = (gx + gy) % 5;
				sp.pass = engine::RenderPass::World;
				sp.ySort = true;
				sp.tint = core::Color{
					static_cast<uint8_t>(100 + gx * 5),
					static_cast<uint8_t>(100 + gy * 5),
					static_cast<uint8_t>(150 + (gx + gy) * 2),
					255
				};
				api.addComponent(e, sp);
			}
		}

	}
		// player
		auto checkerPx = makeCheckerboard(32, 32, 1, { 255,255,255,255 }, { 255,255,255,255 });
		TextureHandle spriteTex = api.createTextureFromMemory(checkerPx.data(), 32, 32);
		auto player = api.spawnEntity();
		engine::Sprite sprite{};
		sprite.texture = spriteTex;
		sprite.srcRect = { 0, 0, 32, 32 };
		sprite.layer = 2;
		sprite.pass = engine::RenderPass::World;
		engine::Transform transform{};
		transform.x = 800.f;
		transform.y = 100.f;
		api.addComponent(player, transform);
		api.addComponent(player, sprite);
		engine::Collider collider{ 32.f, 32.f, 0.f, 0.f, false };
		collider.layer = engine::COLLISION_LAYER_PLAYER;
		collider.mask = engine::COLLISION_LAYER_STATIC;
		api.addComponent(player, collider);
		api.addComponent(player, engine::RigidBody{ 0.f, 0.f, 0.f, false });
		api.addComponent(player, engine::TransformInterpolation{});

		// 在 systems.initAll() 之后新增的 demo system 需要手动 init() 一次。
		// phase 驱动已经保证它会在 GameplayPrePhysics + Camera 两个阶段被正确调用，
		// 不再需要改 RenderSystem 的注册顺序。
		auto& followSystem = ctx.systems.registerSystem<CameraFollowSystem>(
			ctx, player, camEnt, 1080, 720, 32, cfg.windowWidth, cfg.windowHeight);
		followSystem.init();

		std::fprintf(stdout, "\nControls: WASD/arrows=move player  ESC=quit\n");



		// ── main loop ────────────────────────────────────────────────────────────
		float t = 0.f;
		while (ctx.scheduler.tick()) {
			t += ctx.scheduler.deltaTime();

			if (api.isKeyJustPressed(SDLK_ESCAPE)) {
				api.quit();
				break;
			}
			if (autoExit && t >= 2.0f) {
				api.quit();
				break;
			}
		}

		ctx.shutdown();
		return 0;

	}
