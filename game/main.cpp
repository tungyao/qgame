#include <SDL3/SDL_main.h>
#include <engine/runtime/EngineContext.h>
#include <engine/runtime/EngineConfig.h>
#include <engine/api/GameAPI.h>
#include <engine/components/RenderComponents.h>
#include <engine/components/PhysicsComponents.h>
#include <engine/components/AnimatorComponent.h>
#include <engine/components/TextComponent.h>
#include <SDL3/SDL.h>
#include <vector>

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

static std::vector<uint8_t> makeColorTileset(int tileSize, int cols, int rows) {
	int w = tileSize * cols, h = tileSize * rows;
	std::vector<uint8_t> px(w * h * 4, 0);
	core::Color palette[] = {
		{200, 60,  60,  255}, {60, 200, 60,  255},
		{60,  60,  200, 255}, {200,200, 60,  255},
		{200, 60,  200, 255}, {60, 200, 200, 255},
		{200, 140, 60,  255}, {140,140, 140, 255},
	};
	for (int row = 0; row < rows; ++row)
		for (int col = 0; col < cols; ++col) {
			core::Color c = palette[(row * cols + col) % 8];
			for (int ty = 0; ty < tileSize; ++ty)
				for (int tx = 0; tx < tileSize; ++tx) {
					bool border = tx == 0 || ty == 0 || tx == tileSize - 1 || ty == tileSize - 1;
					core::Color fc = border ? core::Color{ 30,30,30,255 } : c;
					int i = ((row * tileSize + ty) * w + col * tileSize + tx) * 4;
					px[i] = fc.r; px[i + 1] = fc.g; px[i + 2] = fc.b; px[i + 3] = fc.a;
				}
		}
	return px;
}

static std::vector<uint8_t> makeTextTexture(const std::string& text, core::Color color) {
	int charW = 8, charH = 16;
	int texW = static_cast<int>(text.length()) * charW;
	int texH = charH;
	std::vector<uint8_t> pixels(texW * texH * 4, 0);
	for (size_t i = 0; i < text.length(); ++i) {
		int cx = static_cast<int>(i) * charW;
		for (int py = 2; py < charH - 2; ++py) {
			for (int px = cx + 1; px < cx + charW - 1; ++px) {
				int idx = (py * texW + px) * 4;
				pixels[idx] = color.r;
				pixels[idx + 1] = color.g;
				pixels[idx + 2] = color.b;
				pixels[idx + 3] = color.a;
			}
		}
	}
	return pixels;
}

int main(int, char*[]) {
	engine::EngineConfig cfg;
	cfg.windowTitle = "StarEngine - Two Camera Demo";
	cfg.windowWidth = 1280;
	cfg.windowHeight = 720;
	engine::EngineContext ctx;
	ctx.init(cfg);
	engine::GameAPI api{ ctx };

	// ── 上传程序化纹理 ────────────────────────────────────────────────────────
	auto checkerPx = makeCheckerboard(64, 64, 8, { 255,100,100,255 }, { 100,100,255,255 });
	TextureHandle spriteTex = api.createTextureFromMemory(checkerPx.data(), 64, 64);

	auto tilesetPx = makeColorTileset(32, 4, 2);
	TextureHandle tilesetTex = api.createTextureFromMemory(tilesetPx.data(), 128, 64);

	auto movingPx = makeTextTexture("Moving", { 100, 255, 100, 255 });
	TextureHandle movingTex = api.createTextureFromMemory(movingPx.data(), 56, 16);

	auto stoppedPx = makeTextTexture("Stopped", { 255, 100, 100, 255 });
	TextureHandle stoppedTex = api.createTextureFromMemory(stoppedPx.data(), 56, 16);

	// ── 字体 ───────────────────────────────────────────────────────────────────
	engine::FontHandle font = api.loadFont("assets/fonts/DejaVuSans.ttf");

	// ── 动画 ───────────────────────────────────────────────────────────────────
	AnimationHandle playerAnim = api.assetManager().loadAnimation("assets/test_anim.json");

	// ═══════════════════════════════════════════════════════════════════════════
	// 相机 1：World 渲染 (depth=0，先绘制，清屏)
	// ═══════════════════════════════════════════════════════════════════════════
	{
		auto e = api.spawnEntity();
		api.addComponent(e, engine::Transform{ 0.f, 0.f });
		engine::Camera cam{};
		cam.zoom = 1.5f;
		cam.primary = true;
		cam.depth = 0;
		cam.layerMask = engine::renderPassBit(engine::RenderPass::World);
		cam.clear = true;
		cam.clearColor = core::Color{ 20, 20, 40, 255 };
		cam.cullEnabled = true;
		api.addComponent(e, cam);
	}

	// ═══════════════════════════════════════════════════════════════════════════
	// 相机 2：UI 叠加渲染 (depth=1，后绘制，不清屏)
	// ═══════════════════════════════════════════════════════════════════════════
	{
		auto e = api.spawnEntity();
		api.addComponent(e, engine::Transform{ 0.f, 0.f });
		engine::Camera cam{};
		cam.zoom = 1.f;
		cam.primary = true;
		cam.depth = 1;
		cam.layerMask = engine::renderPassBit(engine::RenderPass::UI) | engine::renderPassBit(engine::RenderPass::Screen);
		cam.clear = false;
		cam.cullEnabled = false;
		api.addComponent(e, cam);
	}

	// ── TileMap ────────────────────────────────────────────────────────────────
	{
		auto e = api.spawnEntity();
		api.addComponent(e, engine::Transform{ 0.f, 400.f });
		engine::TileMap tmap{};
		tmap.width = 20; tmap.height = 5; tmap.tileSize = 32;
		tmap.tileset = tilesetTex;
		for (int y = 0; y < 5; ++y)
			for (int x = 0; x < 20; ++x)
				tmap.layers[0].push_back((x + y) % 8);
		tmap.layers[1].resize(100, -1);
		tmap.layers[1][2] = 4; tmap.layers[1][12] = 5;
		api.addComponent(e, tmap);
	}

	// ── Sprite 1：原始大小 ─────────────────────────────────────────────────────
	{
		auto e = api.spawnEntity();
		api.addComponent(e, engine::Transform{ 200.f, 200.f });
		engine::Sprite sp{};
		sp.texture = spriteTex;
		sp.srcRect = { 0.f, 0.f, 64.f, 64.f };
		sp.layer = 1;
		sp.pass = engine::RenderPass::World;
		api.addComponent(e, sp);
	}

	// ── Sprite 2：旋转 45°，放大 2×，半透明橙色 tint ──────────────────────────
	{
		auto e = api.spawnEntity();
		engine::Transform tf{}; tf.x = 400.f; tf.y = 200.f;
		tf.rotation = 0.785f; tf.scaleX = 2.f; tf.scaleY = 2.f;
		api.addComponent(e, tf);
		engine::Sprite sp{};
		sp.texture = spriteTex;
		sp.srcRect = { 0.f, 0.f, 64.f, 64.f };
		sp.layer = 1;
		sp.pass = engine::RenderPass::World;
		sp.tint = { 255, 200, 100, 200 };
		api.addComponent(e, sp);
	}

	// ── Sprite 3：横向拉伸，绿色 tint ────────────────────────────────────────
	{
		auto e = api.spawnEntity();
		engine::Transform tf{}; tf.x = 640.f; tf.y = 200.f;
		tf.scaleX = 3.f; tf.scaleY = 1.5f;
		api.addComponent(e, tf);
		engine::Sprite sp{};
		sp.texture = spriteTex;
		sp.srcRect = { 0.f, 0.f, 64.f, 64.f };
		sp.layer = 2;
		sp.pass = engine::RenderPass::World;
		sp.tint = { 100, 255, 100, 255 };
		api.addComponent(e, sp);
	}

	// ── Player (带动画) ───────────────────────────────────────────────────────
	entt::entity player;
	{
		player = api.spawnEntity();
		api.addComponent(player, engine::Transform{ 640.f, 300.f });
		engine::Sprite sp{};
		sp.texture = spriteTex;
		sp.srcRect = { 0.f, 0.f, 32.f, 32.f };
		sp.layer = 3;
		sp.pass = engine::RenderPass::World;
		sp.tint = { 255, 255, 100, 255 };
		sp.ySort = true;
		api.addComponent(player, sp);
		engine::AnimatorComponent anim{};
		anim.currentAnim = playerAnim;
		anim.playing = false;
		anim.applyTexture = true;
		api.addComponent(player, anim);
	}

	// ── 状态文字 (World 层) ───────────────────────────────────────────────────
	entt::entity statusText;
	{
		statusText = api.spawnEntity();
		api.addComponent(statusText, engine::Transform{ 640.f, 250.f });
		engine::Sprite sp{};
		sp.texture = stoppedTex;
		sp.srcRect = { 0.f, 0.f, 56.f, 16.f };
		sp.layer = 10;
		sp.pass = engine::RenderPass::World;
		sp.ySort = false;
		api.addComponent(statusText, sp);
	}

	// ── MSDF 文字渲染 (World 层) ─────────────────────────────────────────────
	{
		auto e = api.spawnEntity();
		api.addComponent(e, engine::Transform{ 40.f, 80.f });
		engine::TextComponent txt{};
		txt.text = "Hello, StarEngine! @#$%^&*()";
		txt.font = font;
		txt.fontSize = 48.f;
		txt.color = { 255, 255, 255, 255 };
		txt.pass = engine::RenderPass::World;
		api.addComponent(e, txt);
	}

	// ── UI 标题 ───────────────────────────────────────────────────────────────
	{
		auto e = api.spawnEntity();
		api.addComponent(e, engine::Transform{ 20.f, 20.f });
		engine::TextComponent txt{};
		txt.text = "Two Camera Demo";
		txt.font = font;
		txt.fontSize = 32.f;
		txt.color = { 255, 255, 100, 255 };
		txt.pass = engine::RenderPass::UI;
		txt.layer = 100;
		api.addComponent(e, txt);
	}

	// ── UI 说明 ───────────────────────────────────────────────────────────────
	{
		auto e = api.spawnEntity();
		api.addComponent(e, engine::Transform{ 20.f, 60.f });
		engine::TextComponent txt{};
		txt.text = "World: zoom=1.5x | UI: zoom=1.0x";
		txt.font = font;
		txt.fontSize = 20.f;
		txt.color = { 200, 200, 200, 255 };
		txt.pass = engine::RenderPass::UI;
		txt.layer = 100;
		api.addComponent(e, txt);
	}

	// ── Screen 提示 ──────────────────────────────────────────────────────────
	{
		auto e = api.spawnEntity();
		api.addComponent(e, engine::Transform{ 20.f, 680.f });
		engine::TextComponent txt{};
		txt.text = "WASD to move | ESC to quit";
		txt.font = font;
		txt.fontSize = 18.f;
		txt.color = { 150, 150, 150, 255 };
		txt.pass = engine::RenderPass::Screen;
		txt.layer = 100;
		api.addComponent(e, txt);
	}

	// ── 主循环 ────────────────────────────────────────────────────────────────
	constexpr float kSpeed = 200.f;
	while (ctx.scheduler.tick()) {
		float dt = ctx.scheduler.deltaTime();
		auto& tf = api.getComponent<engine::Transform>(player);
		auto& anim = api.getComponent<engine::AnimatorComponent>(player);

		bool isMoving = false;
		if (api.isKeyDown(SDLK_W) || api.isKeyDown(SDLK_UP)) { tf.y -= kSpeed * dt; isMoving = true; }
		if (api.isKeyDown(SDLK_S) || api.isKeyDown(SDLK_DOWN)) { tf.y += kSpeed * dt; isMoving = true; }
		if (api.isKeyDown(SDLK_A) || api.isKeyDown(SDLK_LEFT)) { tf.x -= kSpeed * dt; isMoving = true; }
		if (api.isKeyDown(SDLK_D) || api.isKeyDown(SDLK_RIGHT)) { tf.x += kSpeed * dt; isMoving = true; }

		if (isMoving) {
			if (!anim.playing) anim.play(playerAnim);
		} else {
			if (anim.playing) anim.stop();
		}

		auto& statusSpr = api.getComponent<engine::Sprite>(statusText);
		statusSpr.texture = isMoving ? movingTex : stoppedTex;

		if (api.isKeyJustPressed(SDLK_ESCAPE)) { api.quit(); break; }
	}

	ctx.shutdown();
	return 0;
}
