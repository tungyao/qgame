#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <algorithm>
#include <string>

#include <engine/api/GameAPI.h>
#include <engine/components/RenderComponents.h>
#include <engine/components/PhysicsComponents.h>
#include <engine/runtime/EngineConfig.h>
#include <engine/runtime/EngineContext.h>
#include <engine/systems/ISystem.h>

// ── 场景常量 ──────────────────────────────────────────────────────────

constexpr float kPlayW = 900.f;
constexpr float kPlayH = 600.f;
constexpr float kPlayCX = 640.f;
constexpr float kPlayCY = 360.f;
constexpr float kPlayMinX = kPlayCX - kPlayW * 0.5f;
constexpr float kPlayMaxX = kPlayCX + kPlayW * 0.5f;
constexpr float kPlayMinY = kPlayCY - kPlayH * 0.5f;
constexpr float kPlayMaxY = kPlayCY + kPlayH * 0.5f;
constexpr float kWallThick = 20.f;
constexpr float kPlayerR = 14.f;
constexpr float kBallR = 10.f;
constexpr float kObsW = 60.f;
constexpr float kObsH = 120.f;
constexpr float kPickupR = 10.f;
constexpr int   kNumBalls = 8;
constexpr float kPlayerSpeed = 280.f;

// ── 自定义组件 ────────────────────────────────────────────────────────

struct PickupTag {};
struct PlayerTag {};

// ── 程序化纹理 ────────────────────────────────────────────────────────

static TextureHandle makeCircleTex(engine::GameAPI& api,
	int radius, core::Color color) {
	int d = radius * 2;
	std::vector<uint8_t> px(static_cast<size_t>(d) * d * 4, 0);
	int r2 = radius * radius;
	for (int y = 0; y < d; ++y)
		for (int x = 0; x < d; ++x) {
			int dx = x - radius, dy = y - radius;
			if (dx * dx + dy * dy <= r2) {
				size_t i = (static_cast<size_t>(y) * d + x) * 4;
				px[i] = color.r; px[i+1] = color.g;
				px[i+2] = color.b; px[i+3] = 255;
			}
		}
	return api.createTextureFromMemory(px.data(), d, d,
		backend::TextureFilter::Linear);
}

static TextureHandle makeSolidTex(engine::GameAPI& api,
	int w, int h, core::Color color) {
	std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4, 0);
	for (int y = 0; y < h; ++y)
		for (int x = 0; x < w; ++x) {
			size_t i = (static_cast<size_t>(y) * w + x) * 4;
			px[i] = color.r; px[i+1] = color.g;
			px[i+2] = color.b; px[i+3] = 255;
		}
	return api.createTextureFromMemory(px.data(), w, h,
		backend::TextureFilter::Nearest);
}

// ── Sprite 创建辅助 ───────────────────────────────────────────────────

static engine::Sprite makeSprite(TextureHandle tex,
	int w, int h, int layer, engine::RenderPass pass,
	core::Color tint = core::Color::White) {
	engine::Sprite sp;
	sp.texture = tex;
	sp.srcRect = { 0.f, 0.f, static_cast<float>(w), static_cast<float>(h) };
	sp.pivotX = 0.5f; sp.pivotY = 0.5f;
	sp.layer = layer; sp.pass = pass;
	sp.tint = tint; sp.visible = true;
	return sp;
}

// ── 碰撞测试系统 ─────────────────────────────────────────────────────

class CollisionTestSystem : public engine::ISystem {
public:
	entt::entity player;
	entt::entity pickup;

	int  score = 0;
	int  collisionCount = 0;
	float queryRayDist = 0.f;
	bool queryRayHit = false;
	int  queryBoxCount = 0;
	int  queryCircleCount = 0;
	bool jShotHit = false;
	float jShotDist = 0.f;

	CollisionTestSystem(engine::EngineContext& ctx, engine::GameAPI* api)
		: ctx_(ctx), api_(api) {}

	engine::UpdatePhaseMask phaseMask() const override {
		return engine::updatePhaseBit(engine::UpdatePhase::GameplayPrePhysics);
	}

	// ── 查询测试状态（公开，供 main 读取用于 HUD）──
	bool showRaycast_ = false;
	bool showBox_ = false;
	bool showCircle_ = false;

	void onCollision(const engine::CollisionInfo& info) {
		collisionCount++;

		// 收集 Pickup
		entt::entity pickupEnt = entt::null;
		if (info.self == player && ctx_.world.all_of<PickupTag>(info.other))
			pickupEnt = info.other;
		else if (info.other == player && ctx_.world.all_of<PickupTag>(info.self))
			pickupEnt = info.self;

		if (pickupEnt != entt::null && pickupEnt == pickup) {
			score++;
			respawnPickup();
		}
	}

	void respawnPickup() {
		auto* tf = ctx_.world.try_get<engine::Transform>(pickup);
		if (!tf) return;
		float margin = kPickupR + kWallThick + 20.f;
		tf->x = kPlayMinX + margin +
			static_cast<float>(std::rand()) / RAND_MAX *
			(kPlayMaxX - kPlayMinX - 2.f * margin);
		tf->y = kPlayMinY + margin +
			static_cast<float>(std::rand()) / RAND_MAX *
			(kPlayMaxY - kPlayMinY - 2.f * margin);
	}

protected:
	void onGameplayPrePhysicsPhase(float dt) override {
		auto* pTf = ctx_.world.try_get<engine::Transform>(player);
		if (!pTf) return;

		// ── 1. WASD 控制玩家 ──
		float dx = 0.f, dy = 0.f;
		if (ctx_.inputState.isKeyDown(SDLK_W) || ctx_.inputState.isKeyDown(SDLK_UP))    dy = -1.f;
		if (ctx_.inputState.isKeyDown(SDLK_S) || ctx_.inputState.isKeyDown(SDLK_DOWN))  dy =  1.f;
		if (ctx_.inputState.isKeyDown(SDLK_A) || ctx_.inputState.isKeyDown(SDLK_LEFT))  dx = -1.f;
		if (ctx_.inputState.isKeyDown(SDLK_D) || ctx_.inputState.isKeyDown(SDLK_RIGHT)) dx =  1.f;
		if (dx != 0.f || dy != 0.f) {
			float len = std::sqrt(dx * dx + dy * dy);
			dx /= len; dy /= len;
			pTf->x += dx * kPlayerSpeed * dt;
			pTf->y += dy * kPlayerSpeed * dt;

			playerDirX_ = dx;
			playerDirY_ = dy;
		}

		// ── 2. 查询测试（按帧计数触发） ──
		frame_++;
		if (ctx_.inputState.isKeyJustPressed(SDLK_T)) {
			showRaycast_ = !showRaycast_;
		}
		if (ctx_.inputState.isKeyJustPressed(SDLK_B)) {
			showBox_ = !showBox_;
		}
		if (ctx_.inputState.isKeyJustPressed(SDLK_C)) {
			showCircle_ = !showCircle_;
		}

		// ── 3. J 键向鼠标方向发射射线，检测是否击中 pickup（红球）──
		if (ctx_.inputState.isKeyJustPressed(SDLK_J)) {
			float mx = ctx_.inputState.pointerX(0);
			float my = ctx_.inputState.pointerY(0);
			float rdx = mx - pTf->x;
			float rdy = my - pTf->y;
			float rlen = std::sqrt(rdx * rdx + rdy * rdy);
			if (rlen > 0.001f) { rdx /= rlen; rdy /= rlen; }
			auto hit = api_->raycast(
				pTf->x, pTf->y,
				rdx, rdy,
				500.f,
				engine::COLLISION_LAYER_ALL);
			jShotHit = hit.hit && hit.entity == pickup;
			jShotDist = hit.distance;
			if (jShotHit) {
				score++;
				respawnPickup();
				std::printf("[J-Shot] HIT pickup at dist=%.0f  score=%d\n",
					jShotDist, score);
			} else {
				std::printf("[J-Shot] MISS  dist=%.0f  entity=%s\n",
					hit.distance,
					hit.hit ? "other" : "none");
			}
		}

		// Raycast 测试
		queryRayHit = false;
		queryRayDist = 0.f;
		if (showRaycast_) {
			auto hit = api_->raycast(
				pTf->x, pTf->y,
				playerDirX_, playerDirY_,
				300.f,
				engine::COLLISION_LAYER_ALL);
			queryRayHit = hit.hit;
			queryRayDist = hit.distance;
		}

		// OverlapBox 测试
		queryBoxCount = 0;
		if (showBox_) {
			auto hits = api_->overlapBox(
				pTf->x, pTf->y,
				80.f, 60.f,
				engine::COLLISION_LAYER_ALL);
			queryBoxCount = static_cast<int>(hits.size());
		}

		// OverlapCircle 测试
		queryCircleCount = 0;
		if (showCircle_) {
			auto hits = api_->overlapCircle(
				pTf->x, pTf->y,
				100.f,
				engine::COLLISION_LAYER_ALL);
			queryCircleCount = static_cast<int>(hits.size());
		}
	}

private:
	engine::EngineContext& ctx_;
	engine::GameAPI* api_;
	float playerDirX_ = 1.f;
	float playerDirY_ = 0.f;
	int frame_ = 0;
};

// ── CLI 辅助 ─────────────────────────────────────────────────────────

static bool hasArg(int argc, char** argv, const char* name) {
	for (int i = 1; i < argc; ++i)
		if (std::strcmp(argv[i], name) == 0) return true;
	return false;
}

// ═══════════════════════════════════════════════════════════════════════
//  main
// ═══════════════════════════════════════════════════════════════════════

int main(int argc, char** argv) {
	const bool useOpenGL = hasArg(argc, argv, "--opengl");
	std::srand(static_cast<unsigned>(std::time(nullptr)));

	// ── 引擎初始化 ──
	engine::EngineConfig cfg;
	cfg.windowTitle = "Collision Test — QGame Demo8";
	cfg.windowWidth = 1280;
	cfg.windowHeight = 720;
	cfg.vsync = true;
	cfg.debug = true;
	if (useOpenGL) cfg.renderBackend = engine::RenderBackend::OpenGL;

	engine::EngineContext ctx;
	ctx.init(cfg);
	engine::GameAPI api{ ctx };

	// ── 程序化纹理 ──
	auto playerTex = makeCircleTex(api, static_cast<int>(kPlayerR), { 50, 220, 50, 255 });
	auto ballTex   = makeCircleTex(api, static_cast<int>(kBallR),   { 220, 180, 50, 255 });
	auto pickupTex = makeCircleTex(api, static_cast<int>(kPickupR), { 220, 50, 50, 255 });
	auto wallTex   = makeSolidTex(api, 64, 64, { 60, 70, 90, 255 });
	auto obsTex    = makeSolidTex(api, static_cast<int>(kObsW), static_cast<int>(kObsH), { 100, 60, 60, 255 });

	// ── 相机 ──
	auto camEnt = api.spawnEntity();
	api.addComponent(camEnt, engine::Transform{ kPlayCX, kPlayCY });
	{
		engine::Camera cam;
		cam.zoom = 1.f;
		cam.primary = true;
		cam.depth = 0;
		cam.layerMask = engine::renderPassBit(engine::RenderPass::World);
		cam.clear = true;
		cam.clearColor = core::Color{ 12, 16, 28, 255 };
		cam.cullEnabled = false;
		cam.pixelSnap = false;
		api.addComponent(camEnt, cam);
	}

	// ── UI 相机（debug overlay 需要 UI/Screen pass）──
	auto uiCamEnt = api.spawnEntity();
	api.addComponent(uiCamEnt, engine::Transform{ 640.0f, 360.0f });
	{
		engine::Camera cam;
		cam.zoom = 1.f;
		cam.primary = true;
		cam.depth = 1;
		cam.layerMask = engine::renderPassBit(engine::RenderPass::UI)
			| engine::renderPassBit(engine::RenderPass::Screen);
		cam.clear = false;
		cam.cullEnabled = false;
		cam.pixelSnap = true;
		api.addComponent(uiCamEnt, cam);
	}

	// ── 加载 manifest + debug overlay ──
#ifndef QGAME_BAKED_MANIFEST
#define QGAME_BAKED_MANIFEST ""
#endif
	api.loadAssetManifest(QGAME_BAKED_MANIFEST);
	api.enableDebugOverlay();

	// ── 墙体（4 条静态碰撞体）──
	auto makeWall = [&](float x, float y, float w, float h, core::Color tint) {
		auto e = api.spawnEntity();
		api.addComponent(e, engine::Transform{ x, y });
		api.addComponent(e, makeSprite(wallTex, static_cast<int>(w), static_cast<int>(h),
			1, engine::RenderPass::World, tint));
		engine::Collider col;
		col.width = w; col.height = h; col.offsetX = 0; col.offsetY = 0;
		col.layer = engine::COLLISION_LAYER_STATIC;
		col.mask = engine::COLLISION_LAYER_ALL;
		api.addComponent(e, col);
		return e;
	};

	makeWall(kPlayCX, kPlayMinY, kPlayW, kWallThick, { 50, 55, 70, 255 });  // top
	makeWall(kPlayCX, kPlayMaxY, kPlayW, kWallThick, { 50, 55, 70, 255 });  // bottom
	makeWall(kPlayMinX, kPlayCY, kWallThick, kPlayH, { 50, 55, 70, 255 });  // left
	makeWall(kPlayMaxX, kPlayCY, kWallThick, kPlayH, { 50, 55, 70, 255 });  // right

	// ── 静态障碍物 ──
	auto makeObs = [&](float x, float y) {
		auto e = api.spawnEntity();
		api.addComponent(e, engine::Transform{ x, y });
		api.addComponent(e, makeSprite(obsTex, static_cast<int>(kObsW), static_cast<int>(kObsH),
			2, engine::RenderPass::World));
		engine::Collider col;
		col.width = kObsW; col.height = kObsH;
		col.offsetX = 0; col.offsetY = 0;
		col.layer = engine::COLLISION_LAYER_STATIC;
		col.mask = engine::COLLISION_LAYER_ALL;
		api.addComponent(e, col);
		return e;
	};

	makeObs(kPlayCX - 180.f, kPlayCY);
	makeObs(kPlayCX + 180.f, kPlayCY);

	// ── 弹跳球（动态刚体）──
	auto makeBall = [&](float x, float y, float vx, float vy) {
		auto e = api.spawnEntity();
		api.addComponent(e, engine::Transform{ x, y });
		api.addComponent(e, makeSprite(ballTex, static_cast<int>(kBallR * 2),
			static_cast<int>(kBallR * 2), 3, engine::RenderPass::World));
		engine::Collider col;
		col.width = kBallR * 2; col.height = kBallR * 2;
		col.offsetX = 0; col.offsetY = 0;
		col.layer = engine::COLLISION_LAYER_DEFAULT;
		col.mask = engine::COLLISION_LAYER_ALL;
		api.addComponent(e, col);
		engine::RigidBody rb;
		rb.velocityX = vx; rb.velocityY = vy;
		rb.gravityScale = 0.f;
		api.addComponent(e, rb);
		return e;
	};

	for (int i = 0; i < kNumBalls; ++i) {
		float angle = static_cast<float>(i) / kNumBalls * 6.2832f;
		float r = 100.f + static_cast<float>(std::rand()) / RAND_MAX * 80.f;
		float bx = kPlayCX + std::cos(angle) * r;
		float by = kPlayCY + std::sin(angle) * r;
		float speed = 120.f + static_cast<float>(std::rand()) / RAND_MAX * 100.f;
		makeBall(bx, by,
			std::cos(angle + 1.57f) * speed,
			std::sin(angle + 1.57f) * speed);
	}

	// ── 玩家 ──
	auto player = api.spawnEntity();
	api.addComponent(player, engine::Transform{ kPlayCX, kPlayCY + 120.f });
	api.addComponent(player, makeSprite(playerTex, static_cast<int>(kPlayerR * 2),
		static_cast<int>(kPlayerR * 2), 4, engine::RenderPass::World));
	{
		engine::Collider col;
		col.width = kPlayerR * 2; col.height = kPlayerR * 2;
		col.offsetX = 0; col.offsetY = 0;
		col.isTrigger = true;
		col.layer = engine::COLLISION_LAYER_PLAYER;
		col.mask = engine::COLLISION_LAYER_ALL;
		api.addComponent(player, col);
	}
	{
		engine::RigidBody rb;
		rb.isKinematic = true;
		api.addComponent(player, rb);
	}
	ctx.world.emplace<PlayerTag>(player);

	// ── Pickup（Trigger）──
	auto pickup = api.spawnEntity();
	{
		float px = kPlayMaxX - 80.f;
		float py = kPlayMinY + 80.f;
		api.addComponent(pickup, engine::Transform{ px, py });
		api.addComponent(pickup, makeSprite(pickupTex, static_cast<int>(kPickupR * 2),
			static_cast<int>(kPickupR * 2), 3, engine::RenderPass::World));
		engine::Collider col;
		col.width = kPickupR * 2; col.height = kPickupR * 2;
		col.offsetX = 0; col.offsetY = 0;
		col.isTrigger = true;
		col.layer = engine::COLLISION_LAYER_DEFAULT;
		col.mask = engine::COLLISION_LAYER_ALL;
		api.addComponent(pickup, col);
	}
	ctx.world.emplace<PickupTag>(pickup);

	// ── 注册碰撞测试系统 ──
	auto& testSys = ctx.systems.registerSystem<CollisionTestSystem>(ctx, &api);
	testSys.player = player;
	testSys.pickup = pickup;

	api.onCollision<CollisionTestSystem, &CollisionTestSystem::onCollision>(testSys);

	// ── 主循环 ──
	float t = 0.f;
	int frameCount = 0;
	float fpsTimer = 0.f;
	int fps = 0;

	while (ctx.scheduler.tick()) {
		t += ctx.scheduler.deltaTime();
		frameCount++;
		fpsTimer += ctx.scheduler.deltaTime();
		if (fpsTimer >= 1.f) {
			fps = frameCount;
			frameCount = 0;
			fpsTimer -= 1.f;
		}

		if (api.isKeyJustPressed(SDLK_ESCAPE)) {
			api.quit();
			break;
		}

		auto* snake = ctx.world.try_get<CollisionTestSystem>(player);
		(void)snake;

		// ── 调试 HUD ──
		char buf[512];
		auto& ts = testSys;
		std::snprintf(buf, sizeof(buf),
			"FPS: %d  |  Score: %d  |  Collisions/frame: %d\n"
			"[T] Raycast: %s  dist=%.0f  hit=%s\n"
			"[B] OverlapBox(160x120): %d entities\n"
			"[C] OverlapCircle(r=100): %d entities\n"
			"[J] Shot pickup: %s  dist=%.0f\n"
			"WASD=Move  J=ShootRay  T=Raycast  B=Box  C=Circle  ESC=Exit",
			fps, ts.score, ts.collisionCount,
			(ts.showRaycast_ ? "ON" : "OFF"), ts.queryRayDist,
			(ts.queryRayHit ? "YES" : "no"),
			ts.queryBoxCount, ts.queryCircleCount,
			(ts.jShotHit ? "HIT!" : "---"), ts.jShotDist);
		api.setDebugInfo(buf);
		ts.collisionCount = 0;
	}

	ctx.shutdown();
	return 0;
}
