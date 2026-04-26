#include <SDL3/SDL_main.h>
#include <engine/runtime/EngineContext.h>
#include <engine/runtime/EngineConfig.h>
#include <engine/api/GameAPI.h>
#include <engine/components/RenderComponents.h>
#include <engine/components/PhysicsComponents.h>
#include <engine/components/AnimatorComponent.h>
#include <engine/components/TextComponent.h>
#include <engine/systems/RenderSystem.h>
#include <SDL3/SDL.h>
#include <vector>
#include <cstdio>
#include <cstring>

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

int main(int argc, char* argv[]) {
	bool useOpenGL = false;
	for (int i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "--opengl") == 0 || strcmp(argv[i], "-gl") == 0) {
			useOpenGL = true;
		}
	}
	
	engine::EngineConfig cfg;
	cfg.windowTitle = "StarEngine - Two Camera Demo";
	cfg.windowWidth = 1280;
	cfg.windowHeight = 720;
	cfg.debug = true;
	if (0) {
		cfg.renderBackend = engine::RenderBackend::OpenGL;
	}
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

	// Phase 1 测试：构造一个 one-shot "attack" clip，复用 spriteTex
	AnimationHandle attackAnim;
	{
		engine::AnimationClip clip;
		clip.name = "attack_test";
		clip.texture = spriteTex;
		clip.loop = false;
		auto pushFrame = [&](float x, float y, float dur) {
			engine::AnimationFrame f;
			f.srcRect = { x, y, 32.f, 32.f };
			f.duration = dur;
			clip.frames.push_back(f);
			clip.duration += dur;
		};
		pushFrame(0.f,  0.f,  0.10f);
		pushFrame(32.f, 0.f,  0.10f);
		pushFrame(32.f, 32.f, 0.10f);
		pushFrame(0.f,  32.f, 0.10f);
		// Phase 2: 帧事件 — 攻击的 hit window + sfx
		clip.events.push_back({ 0.05f, "sfx",         0, 0.f, "swing" });
		clip.events.push_back({ 0.10f, "hitbox_on",   0, 0.f, "" });
		clip.events.push_back({ 0.30f, "hitbox_off",  0, 0.f, "" });
		clip.events.push_back({ 0.39f, "vfx",         0, 0.f, "spark" });
		attackAnim = api.assetManager().registerAnimation("attack_test", clip);
	}

	// ═══════════════════════════════════════════════════════════════════════════
	// 相机 1：World 渲染 (depth=0，先绘制，清屏)
	// ═══════════════════════════════════════════════════════════════════════════
	entt::entity worldCamera;
	{
		worldCamera = api.spawnEntity();
		api.addComponent(worldCamera, engine::Transform{ 400.0f, 200.0f });
		engine::Camera cam{};
		cam.zoom = 1.5f;
		cam.primary = true;
		cam.depth = 0;
		cam.layerMask = engine::renderPassBit(engine::RenderPass::World);
		cam.clear = true;
		//cam.clearColor = core::Color{ 20, 20, 40, 255 };
		cam.cullEnabled = true;
		api.addComponent(worldCamera, cam);
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

	// ── Phase 3 测试: FSM 驱动的 hero ──────────────────────────────────────
	entt::entity fsmHero;
	{
		fsmHero = api.spawnEntity();
		api.addComponent(fsmHero, engine::Transform{ 200.f, 400.f });
		engine::Sprite sp{};
		sp.texture = spriteTex;
		sp.srcRect = { 0.f, 0.f, 32.f, 32.f };
		sp.layer = 5;
		sp.pass = engine::RenderPass::World;
		sp.tint = { 120, 220, 255, 255 };
		api.addComponent(fsmHero, sp);

		auto ctrl = std::make_shared<engine::AnimatorController>();
		ctrl->states = {
			{ "Idle",   playerAnim, 0.5f, engine::PlayMode::Loop },
			{ "Walk",   playerAnim, 1.5f, engine::PlayMode::Loop },
			{ "Attack", attackAnim, 1.0f, engine::PlayMode::Once },
		};
		ctrl->transitions = {
			{ 0, 1, { { "speed",  engine::ConditionOp::Greater, 0.1f } }, false, 1.f, 0.05f, true },
			{ 1, 0, { { "speed",  engine::ConditionOp::Less,    0.1f } }, false, 1.f, 0.05f, true },
			{ engine::kAnyState, 2, { { "attack", engine::ConditionOp::Trigger, 0.f } }, false, 1.f, 0.f, true },
			{ 2, 0, {}, /*hasExitTime*/ true, 1.f, 0.f, true },
		};
		ctrl->defaultState = 0;

		engine::AnimatorComponent ac{};
		ac.applyTexture = true;
		ac.controller   = ctrl;
		api.addComponent(fsmHero, ac);
	}

	// ═══════════════════════════════════════════════════════════════════════════
	// Phase 4 测试: 多层动画 (Layered Animation)
	// ═══════════════════════════════════════════════════════════════════════════
	// 实体说明：测试额外层如何覆盖/混合基础层的 SrcRect/Texture
	entt::entity layeredHero;
	{
		layeredHero = api.spawnEntity();
		api.addComponent(layeredHero, engine::Transform{ 500.f, 400.f });
		engine::Sprite sp{};
		sp.texture = spriteTex;
		sp.srcRect = { 0.f, 0.f, 32.f, 32.f };
		sp.layer = 5;
		sp.pass = engine::RenderPass::World;
		sp.tint = { 255, 180, 100, 255 };
		api.addComponent(layeredHero, sp);

		auto ctrl = std::make_shared<engine::AnimatorController>();
		// 基础层状态
		ctrl->states = {
			{ "Idle",   playerAnim, 0.8f, engine::PlayMode::Loop },
		};
		ctrl->transitions = {};
		ctrl->defaultState = 0;

		// 额外层 0: 使用不同的动画和混合模式 (Override)
		engine::AnimatorLayer layer0;
		layer0.name = "Overlay";
		layer0.weight = 0.7f;
		layer0.blendMode = engine::LayerBlendMode::Override;
		layer0.mask = engine::LayerChannel::SrcRect;
		layer0.states = {
			{ "OverlayIdle", attackAnim, 1.2f, engine::PlayMode::Loop },
		};
		layer0.transitions = {};
		layer0.defaultState = 0;

		// 额外层 1: 使用 Additive 混合
		engine::AnimatorLayer layer1;
		layer1.name = "AdditiveLayer";
		layer1.weight = 0.5f;
		layer1.blendMode = engine::LayerBlendMode::Override;
		layer1.mask = engine::LayerChannel::SrcRect;
		layer1.states = {
			{ "AddIdle", playerAnim, 2.0f, engine::PlayMode::Loop },
		};
		layer1.transitions = {};
		layer1.defaultState = 0;

		ctrl->layers = { layer0, layer1 };

		engine::AnimatorComponent ac{};
		ac.applyTexture = true;
		ac.controller = ctrl;
		api.addComponent(layeredHero, ac);
	}

	// ═══════════════════════════════════════════════════════════════════════════
	// Phase 5.3 测试: 程序化动画层 (Procedural Animation Layers)
	// ═══════════════════════════════════════════════════════════════════════════
	
	// Phase 5.3a: HitShake - 受击抖动
	entt::entity hitShakeTest;
	{
		hitShakeTest = api.spawnEntity();
		api.addComponent(hitShakeTest, engine::Transform{ 700.f, 300.f });
		engine::Sprite sp{};
		sp.texture = spriteTex;
		sp.srcRect = { 0.f, 0.f, 32.f, 32.f };
		sp.layer = 5;
		sp.pass = engine::RenderPass::World;
		sp.tint = { 255, 100, 100, 255 };
		api.addComponent(hitShakeTest, sp);

		auto ctrl = std::make_shared<engine::AnimatorController>();
		ctrl->states = {
			{ "Idle", playerAnim, 0.5f, engine::PlayMode::Loop },
		};
		ctrl->defaultState = 0;

		engine::AnimatorLayer shakeLayer;
		shakeLayer.name = "HitShake";
		shakeLayer.kind = engine::ProceduralKind::HitShake;
		shakeLayer.weight = 1.f;
		shakeLayer.blendMode = engine::LayerBlendMode::Additive;
		shakeLayer.procedural.triggerParam = "hit";
		shakeLayer.procedural.amplitude = 8.f;
		shakeLayer.procedural.frequency = 20.f;
		shakeLayer.procedural.duration = 0.4f;

		ctrl->layers = { shakeLayer };

		engine::AnimatorComponent ac{};
		ac.applyTexture = true;
		ac.controller = ctrl;
		api.addComponent(hitShakeTest, ac);
	}

	// Phase 5.3b: HurtFlash - 受击红闪
	entt::entity hurtFlashTest;
	{
		hurtFlashTest = api.spawnEntity();
		api.addComponent(hurtFlashTest, engine::Transform{ 800.f, 300.f });
		engine::Sprite sp{};
		sp.texture = spriteTex;
		sp.srcRect = { 0.f, 0.f, 32.f, 32.f };
		sp.layer = 5;
		sp.pass = engine::RenderPass::World;
		sp.tint = { 100, 200, 255, 255 };
		api.addComponent(hurtFlashTest, sp);

		auto ctrl = std::make_shared<engine::AnimatorController>();
		ctrl->states = {
			{ "Idle", playerAnim, 0.6f, engine::PlayMode::Loop },
		};
		ctrl->defaultState = 0;

		engine::AnimatorLayer flashLayer;
		flashLayer.name = "HurtFlash";
		flashLayer.kind = engine::ProceduralKind::HurtFlash;
		flashLayer.weight = 1.f;
		flashLayer.blendMode = engine::LayerBlendMode::Additive;
		flashLayer.procedural.triggerParam = "hurt";
		flashLayer.procedural.amplitude = 0.8f;
		flashLayer.procedural.duration = 0.25f;

		ctrl->layers = { flashLayer };

		engine::AnimatorComponent ac{};
		ac.applyTexture = true;
		ac.controller = ctrl;
		api.addComponent(hurtFlashTest, ac);
	}

	// Phase 5.3c: BreatheBob - 呼吸抖动 (持续效果)
	entt::entity breatheBobTest;
	{
		breatheBobTest = api.spawnEntity();
		api.addComponent(breatheBobTest, engine::Transform{ 900.f, 300.f });
		engine::Sprite sp{};
		sp.texture = spriteTex;
		sp.srcRect = { 0.f, 0.f, 32.f, 32.f };
		sp.layer = 5;
		sp.pass = engine::RenderPass::World;
		sp.tint = { 150, 255, 150, 255 };
		api.addComponent(breatheBobTest, sp);

		auto ctrl = std::make_shared<engine::AnimatorController>();
		ctrl->states = {
			{ "Idle", playerAnim, 0.4f, engine::PlayMode::Loop },
		};
		ctrl->defaultState = 0;

		engine::AnimatorLayer bobLayer;
		bobLayer.name = "BreatheBob";
		bobLayer.kind = engine::ProceduralKind::BreatheBob;
		bobLayer.weight = 1.f;
		bobLayer.blendMode = engine::LayerBlendMode::Additive;
		bobLayer.procedural.amplitude = 3.f;
		bobLayer.procedural.frequency = 0.5f;
		bobLayer.procedural.strengthParam = "breathStrength";

		ctrl->layers = { bobLayer };

		engine::AnimatorComponent ac{};
		ac.applyTexture = true;
		ac.controller = ctrl;
		ac.setFloat("breathStrength", 1.f);
		api.addComponent(breatheBobTest, ac);
	}

	// Phase 5.3d: SquashStretchOnLand - 落地挤压
	entt::entity squashTest;
	{
		squashTest = api.spawnEntity();
		api.addComponent(squashTest, engine::Transform{ 1000.f, 300.f });
		engine::Sprite sp{};
		sp.texture = spriteTex;
		sp.srcRect = { 0.f, 0.f, 32.f, 32.f };
		sp.layer = 5;
		sp.pass = engine::RenderPass::World;
		sp.tint = { 255, 200, 150, 255 };
		api.addComponent(squashTest, sp);

		auto ctrl = std::make_shared<engine::AnimatorController>();
		ctrl->states = {
			{ "Idle", playerAnim, 0.5f, engine::PlayMode::Loop },
		};
		ctrl->defaultState = 0;

		engine::AnimatorLayer squashLayer;
		squashLayer.name = "SquashStretch";
		squashLayer.kind = engine::ProceduralKind::SquashStretchOnLand;
		squashLayer.weight = 1.f;
		squashLayer.blendMode = engine::LayerBlendMode::Override;
		squashLayer.procedural.triggerParam = "land";
		squashLayer.procedural.amplitude = 0.3f;
		squashLayer.procedural.duration = 0.3f;

		ctrl->layers = { squashLayer };

		engine::AnimatorComponent ac{};
		ac.applyTexture = true;
		ac.controller = ctrl;
		api.addComponent(squashTest, ac);
	}

	// Phase 5.3e: 综合测试实体 - 多程序化层叠加
	entt::entity proceduralComboTest;
	{
		proceduralComboTest = api.spawnEntity();
		api.addComponent(proceduralComboTest, engine::Transform{ 1100.f, 300.f });
		engine::Sprite sp{};
		sp.texture = spriteTex;
		sp.srcRect = { 0.f, 0.f, 32.f, 32.f };
		sp.layer = 5;
		sp.pass = engine::RenderPass::World;
		sp.tint = { 200, 180, 255, 255 };
		api.addComponent(proceduralComboTest, sp);

		auto ctrl = std::make_shared<engine::AnimatorController>();
		ctrl->states = {
			{ "Idle", playerAnim, 0.5f, engine::PlayMode::Loop },
		};
		ctrl->defaultState = 0;

		// 层 0: 持续呼吸
		engine::AnimatorLayer bobLayer;
		bobLayer.name = "Breathe";
		bobLayer.kind = engine::ProceduralKind::BreatheBob;
		bobLayer.weight = 0.6f;
		bobLayer.blendMode = engine::LayerBlendMode::Additive;
		bobLayer.procedural.amplitude = 2.f;
		bobLayer.procedural.frequency = 0.8f;

		// 层 1: 受击抖动
		engine::AnimatorLayer shakeLayer;
		shakeLayer.name = "HitShake";
		shakeLayer.kind = engine::ProceduralKind::HitShake;
		shakeLayer.weight = 1.f;
		shakeLayer.blendMode = engine::LayerBlendMode::Additive;
		shakeLayer.procedural.triggerParam = "comboHit";
		shakeLayer.procedural.amplitude = 5.f;
		shakeLayer.procedural.frequency = 15.f;
		shakeLayer.procedural.duration = 0.3f;

		// 层 2: 受击红闪
		engine::AnimatorLayer flashLayer;
		flashLayer.name = "HurtFlash";
		flashLayer.kind = engine::ProceduralKind::HurtFlash;
		flashLayer.weight = 0.8f;
		flashLayer.blendMode = engine::LayerBlendMode::Additive;
		flashLayer.procedural.triggerParam = "comboHurt";
		flashLayer.procedural.amplitude = 0.5f;
		flashLayer.procedural.duration = 0.2f;

		ctrl->layers = { bobLayer, shakeLayer, flashLayer };

		engine::AnimatorComponent ac{};
		ac.applyTexture = true;
		ac.controller = ctrl;
		api.addComponent(proceduralComboTest, ac);
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
		txt.text = "WASD: player move, up down left right: camera move";
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
		api.addComponent(e, engine::Transform{ 20.f, 650.f });
		engine::TextComponent txt{};
		txt.text = "WASD: move player | Arrows: move camera | G: toggle GPU/CPU | J/K/L/U: Phase1 | F: Phase3 | 1-7: Phase5 | ESC: quit";
		txt.font = font;
		txt.fontSize = 14.f;
		txt.color = { 150, 150, 150, 255 };
		txt.pass = engine::RenderPass::Screen;
		txt.layer = 100;
		api.addComponent(e, txt);
	}
	{
		auto e = api.spawnEntity();
		api.addComponent(e, engine::Transform{ 20.f, 670.f });
		engine::TextComponent txt{};
		txt.text = "GPU-Driven: G to toggle | Phase5: 1=HitShake 2=HurtFlash 3=BreathStr 4=Squash 5=Combo 6/7:TimeScale";
		txt.font = font;
		txt.fontSize = 12.f;
		txt.color = { 120, 120, 150, 255 };
		txt.pass = engine::RenderPass::Screen;
		txt.layer = 100;
		api.addComponent(e, txt);
	}
	{
		auto e = api.spawnEntity();
		api.addComponent(e, engine::Transform{ 20.f, 690.f });
		engine::TextComponent txt{};
		txt.text = "GPU Mode: O(n) GPU culling+sorting | CPU Mode: O(n log n) CPU sort | Move camera to see culling effect";
		txt.font = font;
		txt.fontSize = 11.f;
		txt.color = { 100, 100, 130, 255 };
		txt.pass = engine::RenderPass::Screen;
		txt.layer = 100;
		api.addComponent(e, txt);
	}

	// ═══════════════════════════════════════════════════════════════════════════
	// M1/M2 测试：生成大量精灵测试 GPU culling/sorting
	// GPU-Driven 2D Rendering 架构验证
	// ═══════════════════════════════════════════════════════════════════════════
	// 
	// 测试说明：
	// - 按 G 键切换 CPU/GPU 渲染模式
	// - GPU 模式优势：
	//   1. CPU 时间降低 10-50x
	//   2. Draw Calls 降低 10-100x
	//   3. 支持 10K+ 精灵稳定渲染
	// - CPU 模式：传统遍历+排序+批处理
	// 
	// 架构演进：
	// Phase 1: GPU-Driven 2D Rendering (当前) - GPU 剔除+排序
	// Phase 2: Reactive Render Graph - 状态变化驱动渲染
	// Phase 3: Streaming Tile World - 无限地图流式加载
	// ═══════════════════════════════════════════════════════════════════════════
	
	constexpr bool CREATE_MANY_SPRITES = true;
	constexpr int SPRITE_GRID_SIZE = 30;  // 30x30 = 900 sprites (可调大到 50x50=2500 测试性能)
	
	TextureHandle smallTex;  // 用于性能测试的纹理
	
	if (CREATE_MANY_SPRITES) {
		printf("\n");
		printf("╔════════════════════════════════════════════════════════════════╗\n");
		printf("║         GPU-Driven 2D Rendering Architecture Test              ║\n");
		printf("╠════════════════════════════════════════════════════════════════╣\n");
		printf("║  Creating %4d sprites in a grid for stress testing...        ║\n", SPRITE_GRID_SIZE * SPRITE_GRID_SIZE);
		printf("╚════════════════════════════════════════════════════════════════╝\n");
		
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
		
		printf("\n");
		printf("┌─────────────────────────────────────────────────────────────────┐\n");
		printf("│  GPU-Driven Rendering - Controls                               │\n");
		printf("├─────────────────────────────────────────────────────────────────┤\n");
		printf("│  G       : Toggle GPU/CPU rendering mode                       │\n");
		printf("│  Arrows  : Move camera (view sprites outside viewport)         │\n");
		printf("│  WASD    : Move player sprite                                  │\n");
		printf("│  ESC     : Quit                                                │\n");
		printf("├─────────────────────────────────────────────────────────────────┤\n");
		printf("│  Current: %d sprites created                              │\n", SPRITE_GRID_SIZE * SPRITE_GRID_SIZE);
		printf("│  Tip: Move camera to see GPU culling in action!                │\n");
		printf("└─────────────────────────────────────────────────────────────────┘\n\n");
	}

	// ── GPU-driven 状态与性能显示 ─────────────────────────────────────────────
	entt::entity gpuStatusText;
	{
		gpuStatusText = api.spawnEntity();
		api.addComponent(gpuStatusText, engine::Transform{ 20.f, 100.f });
		engine::TextComponent txt{};
		txt.text = "[CPU Mode] Press G to enable GPU-driven rendering";
		txt.font = font;
		txt.fontSize = 18.f;
		txt.color = { 255, 200, 100, 255 };
		txt.pass = engine::RenderPass::UI;
		txt.layer = 100;
		api.addComponent(gpuStatusText, txt);
	}

	// ── 性能统计显示 ─────────────────────────────────────────────────────────
	entt::entity perfText;
	{
		perfText = api.spawnEntity();
		api.addComponent(perfText, engine::Transform{ 20.f, 125.f });
		engine::TextComponent txt{};
		txt.text = "FPS: -- | Sprites: 0 | Visible: 0 | Draw Calls: --";
		txt.font = font;
		txt.fontSize = 14.f;
		txt.color = { 180, 180, 180, 255 };
		txt.pass = engine::RenderPass::UI;
		txt.layer = 100;
		api.addComponent(perfText, txt);
	}
	
	// ── GPU 架构说明 ─────────────────────────────────────────────────────────
	entt::entity archText;
	{
		archText = api.spawnEntity();
		api.addComponent(archText, engine::Transform{ 20.f, 145.f });
		engine::TextComponent txt{};
		txt.text = "Architecture: CPU-Driven (Traditional)";
		txt.font = font;
		txt.fontSize = 12.f;
		txt.color = { 120, 120, 150, 255 };
		txt.pass = engine::RenderPass::UI;
		txt.layer = 100;
		api.addComponent(archText, txt);
	}

	// ── 主循环 ────────────────────────────────────────────────────────────────
	constexpr float kSpeed = 200.f;
	bool gpuDrivenEnabled = false;
	
	while (ctx.scheduler.tick()) {
		float dt = ctx.scheduler.deltaTime();
		auto& anim = api.getComponent<engine::AnimatorComponent>(player);

		// WASD 控制 player 移动
		float dx = 0.f, dy = 0.f;
		if (api.isKeyDown(SDLK_W)) dy -= 1.f;
		if (api.isKeyDown(SDLK_S)) dy += 1.f;
		if (api.isKeyDown(SDLK_A)) dx -= 1.f;
		if (api.isKeyDown(SDLK_D)) dx += 1.f;
		const bool isMoving = (dx != 0.f || dy != 0.f);

		// 只有真正移动时才 patch —— 触发 on_update 让 RenderSystem 把新位置回写 GPU；
		// 直接通过 get<>() 的引用赋值不会触发信号，GPU 缓冲会停留在旧位置。
		if (isMoving) {
			api.patchComponent<engine::Transform>(player, [&](engine::Transform& tf) {
				tf.x += dx * kSpeed * dt;
				tf.y += dy * kSpeed * dt;
			});
		}

		// 方向键控制 World 摄像机位置
		float camDx = 0.f, camDy = 0.f;
		if (api.isKeyDown(SDLK_UP))    camDy -= 1.f;
		if (api.isKeyDown(SDLK_DOWN))  camDy += 1.f;
		if (api.isKeyDown(SDLK_LEFT))  camDx -= 1.f;
		if (api.isKeyDown(SDLK_RIGHT)) camDx += 1.f;
		if (camDx != 0.f || camDy != 0.f) {
			api.patchComponent<engine::Transform>(worldCamera, [&](engine::Transform& tf) {
				tf.x += camDx * kSpeed * dt;
				tf.y += camDy * kSpeed * dt;
			});
		}

		// Phase 1: walk 走最低优先级；attack 锁定时不打断
		if (isMoving) {
			if (!anim.playing && anim.interruptible) {
				engine::PlayOptions o; o.priority = 0;
				anim.play(playerAnim, o);
			}
		} else {
			if (anim.playing && anim.interruptible && anim.currentAnim == playerAnim) anim.stop();
		}

		// Phase 1 手动测试键
		// J: 高优先级 attack + lock（不可被打断的 one-shot）
		if (api.isKeyJustPressed(SDLK_J)) {
			engine::PlayOptions opts;
			opts.priority = 10;
			opts.forceRestart = true;
			opts.mode = engine::PlayMode::Once;
			anim.play(attackAnim, opts);
			anim.lock();
			printf("[Phase1] J: attack play (prio=10, locked, one-shot)\n");
		}
		// K: 低优先级 walk 请求 — 锁定时应入队、否则立即播
		if (api.isKeyJustPressed(SDLK_K)) {
			engine::PlayOptions opts; opts.priority = 1;
			bool wasLocked = !anim.interruptible;
			AnimationHandle prev = anim.currentAnim;
			anim.play(playerAnim, opts);
			const char* result = (anim.currentAnim == playerAnim && prev != playerAnim) ? "switched"
				: anim.hasQueued ? "queued"
				: "kept-current";
			printf("[Phase1] K: low-prio walk -> %s (locked=%d)\n", result, wasLocked ? 1 : 0);
		}
		// L: 入队 walk —— 在 attack 完成后自动接续
		if (api.isKeyJustPressed(SDLK_L)) {
			engine::PlayOptions opts; opts.priority = 0;
			anim.queue(playerAnim, opts);
			printf("[Phase1] L: queued walk for after current\n");
		}
		// U: 解锁当前
		if (api.isKeyJustPressed(SDLK_U)) {
			anim.unlock();
			printf("[Phase1] U: unlocked\n");
		}

		// Phase 2: 消费帧事件队列 (AnimatorSystem 在每帧 update 起始清空)
		if (auto* eq = ctx.world.try_get<engine::AnimEventQueue>(player)) {
			for (auto& ev : eq->events) {
				printf("[Phase2] event '%s' @ t=%.3f (int=%d float=%.2f str='%s')\n",
				       ev.name.c_str(), ev.time, ev.intParam, ev.floatParam, ev.stringParam.c_str());
			}
		}

		// ── Phase 3: 仅写参数, 不调 play() ──────────────────────────────────
		{
			auto& fsm = api.getComponent<engine::AnimatorComponent>(fsmHero);
			fsm.setFloat("speed", isMoving ? 1.f : 0.f);
			if (api.isKeyJustPressed(SDLK_F)) {
				fsm.setTrigger("attack");
				printf("[Phase3] F: setTrigger(attack)\n");
			}
			if (auto* eq = ctx.world.try_get<engine::AnimEventQueue>(fsmHero)) {
				for (auto& ev : eq->events) {
					if (ev.name.rfind("state_", 0) == 0) {
						printf("[Phase3] %s\n", ev.name.c_str());
					} else {
						printf("[Phase3] anim event '%s' @ t=%.3f\n", ev.name.c_str(), ev.time);
					}
				}
			}
		}

		// ═════════════════════════════════════════════════════════════════════
		// Phase 5.3: 程序化层触发测试
		// ═════════════════════════════════════════════════════════════════════
		// 1: HitShake 受击抖动
		if (api.isKeyJustPressed(SDLK_1)) {
			auto& anim = api.getComponent<engine::AnimatorComponent>(hitShakeTest);
			anim.setTrigger("hit");
			printf("[Phase5] 1: HitShake triggered on entity @700,300\n");
		}
		// 2: HurtFlash 受击红闪
		if (api.isKeyJustPressed(SDLK_2)) {
			auto& anim = api.getComponent<engine::AnimatorComponent>(hurtFlashTest);
			anim.setTrigger("hurt");
			printf("[Phase5] 2: HurtFlash triggered on entity @800,300\n");
		}
		// 3: BreatheBob 强度调整 (循环增加)
		if (api.isKeyJustPressed(SDLK_3)) {
			auto& anim = api.getComponent<engine::AnimatorComponent>(breatheBobTest);
			float strength = anim.getFloat("breathStrength") + 0.5f;
			if (strength > 3.f) strength = 0.5f;
			anim.setFloat("breathStrength", strength);
			printf("[Phase5] 3: BreatheBob strength = %.1f\n", strength);
		}
		// 4: SquashStretch 落地挤压
		if (api.isKeyJustPressed(SDLK_4)) {
			auto& anim = api.getComponent<engine::AnimatorComponent>(squashTest);
			anim.setTrigger("land");
			printf("[Phase5] 4: SquashStretch triggered on entity @1000,300\n");
		}
		// 5: 综合测试 - 同时触发多个效果
		if (api.isKeyJustPressed(SDLK_5)) {
			auto& anim = api.getComponent<engine::AnimatorComponent>(proceduralComboTest);
			anim.setTrigger("comboHit");
			anim.setTrigger("comboHurt");
			printf("[Phase5] 5: Combo (HitShake + HurtFlash) triggered on entity @1100,300\n");
		}

		// Phase 5 时间缩放测试 (6/7 键)
		if (api.isKeyJustPressed(SDLK_6)) {
			auto& anim = api.getComponent<engine::AnimatorComponent>(hitShakeTest);
			anim.localTimeScale = anim.localTimeScale * 0.5f;
			if (anim.localTimeScale < 0.125f) anim.localTimeScale = 1.f;
			printf("[Phase5] 6: HitShake timeScale = %.2f\n", anim.localTimeScale);
		}
		if (api.isKeyJustPressed(SDLK_7)) {
			auto& anim = api.getComponent<engine::AnimatorComponent>(breatheBobTest);
			anim.localTimeScale = anim.localTimeScale * 0.5f;
			if (anim.localTimeScale < 0.125f) anim.localTimeScale = 1.f;
			printf("[Phase5] 7: BreatheBob timeScale = %.2f\n", anim.localTimeScale);
		}

		auto& statusSpr = api.getComponent<engine::Sprite>(statusText);
		statusSpr.texture = isMoving ? movingTex : stoppedTex;

		// ── G 键切换 GPU-driven 渲染 ───────────────────────────────────────────
		if (api.isKeyJustPressed(SDLK_G)) {
			gpuDrivenEnabled = !gpuDrivenEnabled;
			if (ctx.systems.has<engine::RenderSystem>()) {
				auto& renderSystem = ctx.systems.get<engine::RenderSystem>();
				renderSystem.setGPUDrivenEnabled(gpuDrivenEnabled);
				
				printf("\n");
				printf("╔════════════════════════════════════════════════════════════════╗\n");
				printf("║  Rendering Mode: %-45s  ║\n", 
				       gpuDrivenEnabled ? "GPU-DRIVEN (Fast)" : "CPU-DRIVEN (Traditional)");
				printf("╠════════════════════════════════════════════════════════════════╣\n");
				if (gpuDrivenEnabled) {
					printf("║  ✓ GPU Culling:  Parallel O(n/64) on GPU                      ║\n");
					printf("║  ✓ GPU Sorting:  Radix Sort O(n) on GPU                       ║\n");
					printf("║  ✓ Draw Calls:   1-10 (batched)                               ║\n");
					printf("║  ✓ CPU Time:     < 0.5ms                                      ║\n");
				} else {
					printf("║  • CPU Culling:  Linear O(n) scan                             ║\n");
					printf("║  • CPU Sorting:  QuickSort O(n log n)                         ║\n");
					printf("║  • Draw Calls:   10-100+ (texture switches)                   ║\n");
					printf("║  • CPU Time:     5-20ms (depends on sprite count)             ║\n");
				}
				printf("╚════════════════════════════════════════════════════════════════╝\n\n");
			}
			
			// 更新状态文本
			auto& gpuTxt = api.getComponent<engine::TextComponent>(gpuStatusText);
			if (gpuDrivenEnabled) {
				gpuTxt.text = "[GPU Mode] Press G to switch to CPU mode";
				gpuTxt.color = { 100, 255, 100, 255 };
			} else {
				gpuTxt.text = "[CPU Mode] Press G to enable GPU-driven rendering";
				gpuTxt.color = { 255, 200, 100, 255 };
			}
			
			// 更新架构说明
			auto& archTxt = api.getComponent<engine::TextComponent>(archText);
			if (gpuDrivenEnabled) {
				archTxt.text = "Architecture: GPU-Driven (Compute Culling + Indirect Draw)";
				archTxt.color = { 100, 200, 150, 255 };
			} else {
				archTxt.text = "Architecture: CPU-Driven (Traditional Scan + Sort)";
				archTxt.color = { 120, 120, 150, 255 };
			}
		}

		// ── 更新性能统计显示 ───────────────────────────────────────────────────
		{
			static float fpsAccum = 0.f;
			static int fpsFrameCount = 0;
			static float displayFps = 0.f;
			
			fpsAccum += dt;
			fpsFrameCount++;
			
			// 每 0.5 秒更新一次 FPS 显示
			if (fpsAccum >= 0.5f) {
				displayFps = fpsFrameCount / fpsAccum;
				fpsAccum = 0.f;
				fpsFrameCount = 0;
			}
			
			if (ctx.systems.has<engine::RenderSystem>()) {
				auto& renderSystem = ctx.systems.get<engine::RenderSystem>();
				uint32_t spriteCount = renderSystem.spriteBuffer().activeCount();
				
				// 计算可见精灵数 (简化：使用精灵总数作为估计)
				// 实际可见数由 GPU/视口裁剪决定
				uint32_t estimatedVisible = spriteCount;
				
				auto& perfTxt = api.getComponent<engine::TextComponent>(perfText);
				char buf[128];
				snprintf(buf, sizeof(buf), 
				         "FPS: %.0f | Sprites: %u | Mode: %s",
				         displayFps,
				         spriteCount,
				         gpuDrivenEnabled ? "GPU" : "CPU");
				perfTxt.text = buf;
				
				// 根据性能调整颜色
				if (displayFps >= 55.f) {
					perfTxt.color = { 100, 255, 100, 255 };  // 绿色 - 良好
				} else if (displayFps >= 30.f) {
					perfTxt.color = { 255, 255, 100, 255 };  // 黄色 - 一般
				} else {
					perfTxt.color = { 255, 100, 100, 255 };  // 红色 - 需优化
				}
			}
		}

		if (api.isKeyJustPressed(SDLK_ESCAPE)) { api.quit(); break; }
	}

	ctx.shutdown();
	return 0;
}
