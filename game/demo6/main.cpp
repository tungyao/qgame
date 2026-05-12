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

#include <engine/api/GameAPI.h>
#include <engine/components/RenderComponents.h>
#include <engine/components/PhysicsComponents.h>
#include <engine/runtime/EngineConfig.h>
#include <engine/runtime/EngineContext.h>
#include <engine/systems/ISystem.h>

// ═══════════════════════════════════════════════════════════════════════
//  可配置参数
// ═══════════════════════════════════════════════════════════════════════
// 命令行: --speed <px/s>

constexpr float kDefaultSpeed = 220.0f;    // 蛇头移动速度 (像素/秒)
constexpr float kSegmentSpacing = 24.0f;     // 身体段中心间距 (像素)
constexpr float kSegmentRadius = 10.0f;     // 身体段视觉半径
constexpr float kFoodRadius = 8.0f;      // 食物半径
constexpr float kPlayMinX = 100.0f;    // 游戏区域边界
constexpr float kPlayMaxX = 1180.0f;
constexpr float kPlayMinY = 60.0f;
constexpr float kPlayMaxY = 660.0f;
constexpr int   kInitialBody = 5;         // 初始身体段数
constexpr float kSpeedUpPerFive = 15.0f;     // 每吃 5 个食物速度增量

// ═══════════════════════════════════════════════════════════════════════
//  方向辅助
// ═══════════════════════════════════════════════════════════════════════

enum class SnakeDir : uint8_t { Right = 0, Down, Left, Up };

static void dirVector(SnakeDir d, float& dx, float& dy) {
	static constexpr float kDirs[4][2] = { {1,0},{0,1},{-1,0},{0,-1} };
	auto i = static_cast<int>(d) & 3;
	dx = kDirs[i][0];
	dy = kDirs[i][1];
}

static bool isOpposite(SnakeDir a, SnakeDir b) {
	return (static_cast<int>(a) + 2) % 4 == static_cast<int>(b);
}

// ═══════════════════════════════════════════════════════════════════════
//  ECS 组件
// ═══════════════════════════════════════════════════════════════════════

struct SnakeHead {
	SnakeDir   direction = SnakeDir::Right;
	SnakeDir   nextDir = SnakeDir::Right;
	float      speed = kDefaultSpeed;
	bool       alive = true;
	int        score = 0;
	int        pendingGrow = 0;
	std::vector<entt::entity> body;
	std::deque<std::pair<float, float>> trail; // 蛇头路径轨迹
};

struct SnakeFood {};

// ═══════════════════════════════════════════════════════════════════════
//  SnakeSystem
// ═══════════════════════════════════════════════════════════════════════

class SnakeSystem : public engine::ISystem {
public:
	TextureHandle headTex;
	TextureHandle bodyTex;
	TextureHandle foodTex;

	SnakeSystem(engine::EngineContext& ctx, entt::entity head)
		: ctx_(ctx), head_(head) {
	}

	engine::UpdatePhaseMask phaseMask() const override {
		return engine::updatePhaseBit(engine::UpdatePhase::GameplayPrePhysics);
	}

	void spawnFood() {
		auto& reg = ctx_.world;
		auto food = reg.create();
		float margin = kFoodRadius + 4.0f;
		float fx = kPlayMinX + margin
			+ static_cast<float>(std::rand()) / RAND_MAX
			* (kPlayMaxX - kPlayMinX - 2.0f * margin);
		float fy = kPlayMinY + margin
			+ static_cast<float>(std::rand()) / RAND_MAX
			* (kPlayMaxY - kPlayMinY - 2.0f * margin);

		reg.emplace<engine::Transform>(food, engine::Transform{ fx, fy });
		engine::Sprite sp;
		sp.texture = foodTex;
		sp.srcRect = { 0.f, 0.f, kFoodRadius * 2, kFoodRadius * 2 };
		sp.layer = 2;
		sp.visible = true;
		sp.tint = core::Color{ 220, 40, 40, 255 };
		sp.pivotX = 0.5f;
		sp.pivotY = 0.5f;
		sp.pass = engine::RenderPass::World;
		reg.emplace<engine::Sprite>(food, sp);
		{
			engine::Collider col;
			col.width = kFoodRadius * 2;
			col.height = kFoodRadius * 2;
			col.offsetX = 0;
			col.offsetY = 0;
			col.isTrigger = true;
			col.mask = 0;
			reg.emplace<engine::Collider>(food, col);
		}
		reg.emplace<SnakeFood>(food);
	}

	void restart(entt::entity headEntity) {
		head_ = headEntity;
		auto* snake = ctx_.world.try_get<SnakeHead>(head_);
		if (snake) {
			for (auto seg : snake->body)
				if (ctx_.world.valid(seg))
					ctx_.world.destroy(seg);
			snake->body.clear();
		}

		for (auto f : ctx_.world.view<SnakeFood>())
			ctx_.world.destroy(f);

		if (!snake) return;

		auto* headTF = ctx_.world.try_get<engine::Transform>(head_);
		if (headTF) { headTF->x = 640.f; headTF->y = 360.f; }

		snake->direction = SnakeDir::Right;
		snake->nextDir = SnakeDir::Right;
		snake->alive = true;
		snake->score = 0;
		snake->pendingGrow = 0;
		snake->speed = kDefaultSpeed;
		snake->trail.clear();

		if (headTF) {
			// 预填充虚拟路径点，使身体段从第 1 帧就有足够路径历史
			{
				float step = kSegmentSpacing / 4.0f; // ~6px 精度
				int vCount = (kInitialBody + 1) * static_cast<int>(kSegmentSpacing / step) + 6;
				for (int i = vCount; i >= 0; --i)
					snake->trail.push_back({ headTF->x - i * step, headTF->y });
			}
			engine::Sprite bodySp;
			bodySp.texture = bodyTex;
			bodySp.srcRect = { 0.f, 0.f, kSegmentRadius * 2, kSegmentRadius * 2 };
			bodySp.layer = 2;
			bodySp.visible = true;
			bodySp.tint = core::Color{ 30, 150, 30, 255 };
			bodySp.pivotX = 0.5f;
			bodySp.pivotY = 0.5f;
			bodySp.pass = engine::RenderPass::World;

			engine::Collider bodyCol;
			bodyCol.width = kSegmentRadius * 2;
			bodyCol.height = kSegmentRadius * 2;
			bodyCol.offsetX = -kSegmentRadius;
			bodyCol.offsetY = -kSegmentRadius;
			bodyCol.isTrigger = true;
			bodyCol.mask = 0;

			for (int i = 1; i <= kInitialBody; ++i) {
				auto seg = ctx_.world.create();
				ctx_.world.emplace<engine::Transform>(seg,
					engine::Transform{
						headTF->x - i * kSegmentSpacing,
						headTF->y
					});
				ctx_.world.emplace<engine::Sprite>(seg, bodySp);
				ctx_.world.emplace<engine::Collider>(seg, bodyCol);
				snake->body.push_back(seg);
			}
		}

		spawnFood();
	}

protected:
	void onGameplayPrePhysicsPhase(float dt) override {
		auto* snake = ctx_.world.try_get<SnakeHead>(head_);
		if (!snake || !snake->alive) return;

		// ── 1. WASD input ──
		// 使用 isKeyJustPressed（按下即转），不依赖持续按键状态，
		// 避免多键同时按下时 else-if 链产生竞争导致蛇卡顿。
		{
			auto& input = ctx_.inputState;
			SnakeDir newDir = snake->direction;
			bool changed = false;
			if (input.isKeyJustPressed(SDLK_W) || input.isKeyJustPressed(SDLK_UP)) {
				newDir = SnakeDir::Up;    changed = true;
			}
			if (input.isKeyJustPressed(SDLK_S) || input.isKeyJustPressed(SDLK_DOWN)) {
				newDir = SnakeDir::Down;  changed = true;
			}
			if (input.isKeyJustPressed(SDLK_A) || input.isKeyJustPressed(SDLK_LEFT)) {
				newDir = SnakeDir::Left;  changed = true;
			}
			if (input.isKeyJustPressed(SDLK_D) || input.isKeyJustPressed(SDLK_RIGHT)) {
				newDir = SnakeDir::Right; changed = true;
			}
			if (changed && !isOpposite(newDir, snake->direction))
				snake->nextDir = newDir;
		}

		// ── 2. 立即转弯 ──
		snake->direction = snake->nextDir;

		// ── 3. 蛇头连续移动 ──
		auto* headTF = ctx_.world.try_get<engine::Transform>(head_);
		if (!headTF) return;

		float dx, dy;
		dirVector(snake->direction, dx, dy);
		float newX = headTF->x + dx * snake->speed * dt;
		float newY = headTF->y + dy * snake->speed * dt;

		// ── 4. 墙壁碰撞 ──
		if (newX < kPlayMinX || newX > kPlayMaxX ||
			newY < kPlayMinY || newY > kPlayMaxY) {
			snake->alive = false;
			return;
		}
		headTF->x = newX;
		headTF->y = newY;

		// ── 5. 记录轨迹 ──
		snake->trail.push_back({ headTF->x, headTF->y });

		// ── 6. 身体沿轨迹定位 ──
		// 从蛇头沿轨迹向后行走，每段距离 = kSegmentSpacing
		{
			size_t segIdx = 0;
			float targetDist = kSegmentSpacing;
			float accum = 0.f;
			auto prev = snake->trail.rbegin();               // ← 最新的轨迹点（蛇头）
			auto it = prev; ++it;                           // ← 其前一个点

			while (it != snake->trail.rend() && segIdx < snake->body.size()) {
				float dx = prev->first - it->first;
				float dy = prev->second - it->second;
				float segLen = std::sqrt(dx * dx + dy * dy);
				accum += segLen;

				while (accum >= targetDist && segIdx < snake->body.size()) {
					float overshoot = accum - targetDist;     // 越过目标多远
					float t = (segLen > 0.001f)
						? std::clamp(1.f - overshoot / segLen, 0.f, 1.f)
						: 0.f;
					auto* tf = ctx_.world.try_get<engine::Transform>(snake->body[segIdx]);
					if (tf) {
						tf->x = prev->first + (it->first - prev->first) * t;
						tf->y = prev->second + (it->second - prev->second) * t;
					}
					targetDist += kSegmentSpacing;
					++segIdx;
				}

				prev = it;
				++it;
			}

			// 轨迹不够长 → 剩余段塞到最旧点
			while (segIdx < snake->body.size()) {
				auto* tf = ctx_.world.try_get<engine::Transform>(snake->body[segIdx]);
				if (tf) { tf->x = prev->first; tf->y = prev->second; }
				++segIdx;
			}
		}

		// 修剪轨迹：只保留身体长度 + 50% 余量
		while (snake->trail.size() > (snake->body.size() + 2) * 3)
			snake->trail.pop_front();

		// ── 6. 自碰撞 ──
		{
			float r2 = kSegmentRadius * kSegmentRadius * 4.0f;
			for (size_t i = 3; i < snake->body.size(); ++i) {
				auto* tf = ctx_.world.try_get<engine::Transform>(snake->body[i]);
				if (!tf) continue;
				float ddx = headTF->x - tf->x;
				float ddy = headTF->y - tf->y;
				if (ddx * ddx + ddy * ddy < r2) {
					snake->alive = false;
					return;
				}
			}
		}

		// ── 7. 生长 ──
		while (snake->pendingGrow > 0) {
			auto newSeg = ctx_.world.create();
			float px = headTF->x, py = headTF->y;
			if (!snake->body.empty()) {
				auto* last = ctx_.world.try_get<engine::Transform>(snake->body.back());
				if (last) { px = last->x; py = last->y; }
			}
			engine::Sprite bodySp;
			bodySp.texture = bodyTex;
			bodySp.srcRect = { 0.f, 0.f, kSegmentRadius * 2, kSegmentRadius * 2 };
			bodySp.layer = 2;
			bodySp.visible = true;
			bodySp.tint = core::Color{ 30, 150, 30, 255 };
			bodySp.pivotX = 0.5f;
			bodySp.pivotY = 0.5f;
			bodySp.pass = engine::RenderPass::World;
			ctx_.world.emplace<engine::Transform>(newSeg,
				engine::Transform{ px, py });
			ctx_.world.emplace<engine::Sprite>(newSeg, bodySp);
			{
				engine::Collider col;
				col.width = kSegmentRadius * 2;
				col.height = kSegmentRadius * 2;
				col.offsetX = -kSegmentRadius;
				col.offsetY = -kSegmentRadius;
				col.isTrigger = true;
				col.mask = 0;
				ctx_.world.emplace<engine::Collider>(newSeg, col);
			}
			snake->body.push_back(newSeg);
			snake->pendingGrow--;
		}

		// ── 8. 食物碰撞 ──
		{
			auto foodView = ctx_.world.view<SnakeFood, engine::Transform>();
			float eatR2 = (kSegmentRadius + kFoodRadius)
				* (kSegmentRadius + kFoodRadius);
			for (auto foodEnt : foodView) {
				auto* ftf = ctx_.world.try_get<engine::Transform>(foodEnt);
				if (!ftf) continue;
				float fdx = headTF->x - ftf->x;
				float fdy = headTF->y - ftf->y;
				if (fdx * fdx + fdy * fdy < eatR2) {
					ctx_.world.destroy(foodEnt);
					snake->score++;
					snake->pendingGrow++;
					spawnFood();
					break;
				}
			}
		}

		// ── 9. 加速 ──
		snake->speed = kDefaultSpeed + (snake->score / 5) * kSpeedUpPerFive;
	}

private:
	engine::EngineContext& ctx_;
	entt::entity           head_;
};

// ═══════════════════════════════════════════════════════════════════════
//  程序化纹理
// ═══════════════════════════════════════════════════════════════════════

static TextureHandle makeCircleTex(engine::GameAPI& api,
	int radius,
	core::Color color) {
	int d = radius * 2;
	std::vector<uint8_t> px(static_cast<size_t>(d) * d * 4, 0);
	int r2 = radius * radius;
	for (int y = 0; y < d; ++y) {
		for (int x = 0; x < d; ++x) {
			int dx = x - radius;
			int dy = y - radius;
			if (dx * dx + dy * dy <= r2) {
				size_t i = (static_cast<size_t>(y) * d + x) * 4;
				px[i + 0] = color.r;
				px[i + 1] = color.g;
				px[i + 2] = color.b;
				px[i + 3] = 255;
			}
		}
	}
	return api.createTextureFromMemory(
		px.data(), d, d, backend::TextureFilter::Linear);
}

// ═══════════════════════════════════════════════════════════════════════
//  CLI 辅助
// ═══════════════════════════════════════════════════════════════════════

static bool hasArg(int argc, char** argv, const char* name) {
	for (int i = 1; i < argc; ++i)
		if (std::strcmp(argv[i], name) == 0) return true;
	return false;
}

static const char* getArg(int argc, char** argv,
	const char* name, const char* def) {
	for (int i = 1; i < argc - 1; ++i)
		if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
	return def;
}

// ═══════════════════════════════════════════════════════════════════════
//  main
// ═══════════════════════════════════════════════════════════════════════

int main(int argc, char** argv) {
	if (hasArg(argc, argv, "--help")) {
		std::printf(
			"Snake Game — QGame Demo6\n"
			"  --speed <px/s>   蛇头速度 (default: %.0f)\n"
			"  --opengl         使用 OpenGL 后端\n"
			"  --help           此帮助\n"
			"\n操作:\n"
			"  WASD / 方向键    转弯\n"
			"  R                重新开始\n"
			"  ESC              退出\n",
			kDefaultSpeed);
		return 0;
	}

	float speed = kDefaultSpeed;
	if (const char* s = getArg(argc, argv, "--speed", nullptr)) {
		speed = std::max(10.0f, static_cast<float>(std::atof(s)));
	}

	const bool useOpenGL = hasArg(argc, argv, "--opengl");

	// ── 引擎初始化 ──
	engine::EngineConfig cfg;
	cfg.windowTitle = "Snake Game — QGame Demo6";
	cfg.windowWidth = 1280;
	cfg.windowHeight = 720;
	cfg.vsync = true;
	cfg.debug = true;
	if (useOpenGL)
		cfg.renderBackend = engine::RenderBackend::OpenGL;

	engine::EngineContext ctx;
	ctx.init(cfg);
	engine::GameAPI api{ ctx };

	std::srand(static_cast<unsigned>(std::time(nullptr)));

	// ── 程序化纹理 ──
	int segDiam = static_cast<int>(kSegmentRadius * 2);
	int foodDiam = static_cast<int>(kFoodRadius * 2);

	TextureHandle headTex = makeCircleTex(api, segDiam / 2, core::Color::White);
	TextureHandle bodyTex = makeCircleTex(api, segDiam / 2, core::Color::White);
	TextureHandle foodTex = makeCircleTex(api, foodDiam / 2, core::Color::White);

	// ── 世界相机（固定视角，显示整个游戏区域）──
	auto camEnt = api.spawnEntity();
	api.addComponent(camEnt, engine::Transform{ 640.f, 360.f });
	{
		engine::Camera cam;
		cam.zoom = 1.f;
		cam.primary = true;
		cam.depth = 0;
		cam.layerMask = engine::renderPassBit(engine::RenderPass::World);
		cam.clear = true;
		cam.clearColor = core::Color{ 8, 12, 22, 255 };
		cam.cullEnabled = false;
		cam.pixelSnap = false;
		api.addComponent(camEnt, cam);
	}

	// ── UI 相机 ──
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

	// ── 调试覆盖层 ──
	api.loadAssetManifest(QGAME_BAKED_MANIFEST);
	api.enableDebugOverlay();

	// ── 蛇头实体 ──
	auto head = api.spawnEntity();
	api.addComponent(head, engine::Transform{ 640.f, 360.f });
	{
		engine::Sprite sp;
		sp.texture = headTex;
		sp.srcRect = { 0.f, 0.f, static_cast<float>(segDiam), static_cast<float>(segDiam) };
		sp.layer = 3;
		sp.visible = true;
		sp.tint = core::Color{ 50, 220, 50, 255 };
		sp.pivotX = 0.5f;
		sp.pivotY = 0.5f;
		sp.pass = engine::RenderPass::World;
		api.addComponent(head, sp);
	}
	{
		engine::Collider col;
		col.width = static_cast<float>(segDiam);
		col.height = static_cast<float>(segDiam);
		col.offsetX = 0;
		col.offsetY = 0;
		col.isTrigger = true;
		col.mask = 0;
		api.addComponent(head, col);
	}
	{
		SnakeHead sh;
		sh.speed = speed;
		api.addComponent(head, sh);
	}

	// ── 系统注册 ──
	auto& sys = ctx.systems.registerSystem<SnakeSystem>(ctx, head);
	sys.headTex = headTex;
	sys.bodyTex = bodyTex;
	sys.foodTex = foodTex;

	// ── 初始化身体与食物 ──
	sys.restart(head);

	std::printf(
		"=== Snake Game ===\n"
		"Speed: %.0f px/s\n"
		"操作: WASD=转弯  R=重新开始  ESC=退出\n\n",
		speed);

	// ── 主循环 ──
	float t = 0.f;
	bool  prevAlive = true;
	int   prevScore = -1;

	while (ctx.scheduler.tick()) {
		t += ctx.scheduler.deltaTime();

		if (api.isKeyJustPressed(SDLK_ESCAPE)) {
			api.quit();
			break;
		}

		auto* snake = ctx.world.try_get<SnakeHead>(head);
		if (snake && !snake->alive && api.isKeyJustPressed(SDLK_R)) {
			sys.restart(head);
		}

		if (snake) {
			if (snake->alive != prevAlive) {
				if (!snake->alive)
					std::printf("游戏结束! 得分: %d  (按 R 重新开始)\n",
						snake->score);
				prevAlive = snake->alive;
			}
			if (snake->score != prevScore) {
				std::printf("得分: %d  速度: %.0f\n",
					snake->score, snake->speed);
				prevScore = snake->score;
			}

			char infoBuf[128];
			std::snprintf(infoBuf, sizeof(infoBuf),
				"得分: %d  |  速度: %.0f  |  长度: %zu  |  %s",
				snake->score, snake->speed,
				snake->body.size() + 1,
				snake->alive ? "存活" : "死亡");
			api.setDebugInfo(infoBuf);
		}
	}

	ctx.shutdown();
	return 0;
}
