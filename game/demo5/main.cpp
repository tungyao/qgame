#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <backend/renderer/IRenderDevice.h>
#include <engine/api/GameAPI.h>
#include <engine/components/LightComponents.h>
#include <engine/components/RenderComponents.h>
#include <engine/components/TextComponent.h>
#include <engine/runtime/EngineConfig.h>
#include <engine/runtime/EngineContext.h>

#ifndef QGAME_BAKED_MANIFEST
#define QGAME_BAKED_MANIFEST "assets/manifest.baked.json"
#endif

namespace {

	constexpr float kPi = 3.14159265358979323846f;

	bool hasArg(int argc, char** argv, const char* name) {
		for (int i = 1; i < argc; ++i) {
			if (std::strcmp(argv[i], name) == 0) return true;
		}
		return false;
	}

	float clamp01(float v) {
		return std::max(0.f, std::min(1.f, v));
	}

	std::vector<uint8_t> makeSolidTexture(core::Color c) {
		return { c.r, c.g, c.b, c.a };
	}

	TextureHandle makeCircleTex(engine::GameAPI& api,
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

	entt::entity makeSpritePx(engine::GameAPI& api,
		TextureHandle texture,
		float texW,
		float texH,
		float x,
		float y,
		float w,
		float h,
		core::Color tint,
		int layer) {
		auto e = api.spawnEntity();
		api.addComponent(e, engine::Transform{ x, y, 0.f, w / texW, h / texH });

		engine::Sprite s{};
		s.texture = texture;
		s.srcRect = core::Rect{ 0.f, 0.f, texW, texH };
		s.tint = tint;
		s.layer = layer;
		s.pass = engine::RenderPass::World;
		s.pivotX = 0.5f;
		s.pivotY = 0.5f;
		api.addComponent(e, s);
		return e;
	}

	entt::entity makeRect(engine::GameAPI& api,
		TextureHandle whiteTex,
		float x,
		float y,
		float w,
		float h,
		core::Color tint,
		int layer) {
		return makeSpritePx(api, whiteTex, 1.f, 1.f, x, y, w, h, tint, layer);
	}

	entt::entity makeText(engine::GameAPI& api,
		engine::FontHandle font,
		const std::string& value,
		float x,
		float y,
		float size,
		core::Color color) {
		auto e = api.spawnEntity();
		api.addComponent(e, engine::Transform{ x, y });

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

	void addAABBOccluder(engine::GameAPI& api, entt::entity e, float w, float h, float opacity = 1.f) {
		// Tree trunks are the hard blockers in this 2.5D test. Crowns remain visual
		// mass only, so the shadow shape is stable and easy to reason about.
		engine::LightOccluder2D occ{};
		occ.shape = engine::LightOccluder2D::Shape::AABB;
		occ.width = w;
		occ.height = h;
		occ.opacity = opacity;
		api.addComponent(e, occ);
	}

	void addWaterReflector(engine::GameAPI& api, entt::entity e, float w, float h) {
		// Reflector2D marks the pond as an SSR receiver. The lighting compute pass
		// mirrors the previously rendered World sceneColor here, so tree sprites
		// and the moving lantern can reflect without duplicate "mirror" entities.
		engine::Reflector2D refl{};
		refl.shape = engine::Reflector2D::Shape::AABB;
		refl.width = w;
		refl.height = h;
		refl.reflectivity = 1.0f;
		refl.roughness = 0.16f;
		refl.tint = core::Color{ 160, 210, 255, 255 };
		api.addComponent(e, refl);
	}

	struct TreeVisuals {
		float x;
		float trunkTopY;
		float trunkH;
		float crownW;
		float crownH;
		core::Color crownColor;
	};

	void makeTree(engine::GameAPI& api,
		TextureHandle whiteTex,
		TextureHandle crownTex,
		const TreeVisuals& t,
		float /*waterY*/) {
		const float trunkW = 24.f;
		const float trunkY = t.trunkTopY + t.trunkH * 0.5f;
		makeRect(api, whiteTex, t.x, trunkY, trunkW, t.trunkH,
			core::Color{ 66, 45, 30, 255 }, 14);
		auto trunkOcc = makeRect(api, whiteTex, t.x, trunkY, trunkW + 8.f, t.trunkH,
			core::Color{ 22, 18, 16, 60 }, 13);
		addAABBOccluder(api, trunkOcc, trunkW + 8.f, t.trunkH, 1.f);

		makeSpritePx(api, crownTex, 128.f, 128.f, t.x, t.trunkTopY - 18.f,
			t.crownW, t.crownH, t.crownColor, 15);
		makeSpritePx(api, crownTex, 128.f, 128.f, t.x - t.crownW * 0.22f, t.trunkTopY + 8.f,
			t.crownW * 0.66f, t.crownH * 0.66f, core::Color{ 24, 70, 44, 230 }, 15);
		makeSpritePx(api, crownTex, 128.f, 128.f, t.x + t.crownW * 0.22f, t.trunkTopY + 12.f,
			t.crownW * 0.62f, t.crownH * 0.62f, core::Color{ 18, 58, 38, 225 }, 15);

		// No explicit mirrored sprites here: demo5 validates sceneColor SSR. The
		// water Reflector2D samples the offscreen World color target and mirrors
		// these actual tree sprites into the pond.
	}

} // namespace

int main(int argc, char** argv) {
	const bool useOpenGL = hasArg(argc, argv, "--opengl");
	const bool autoExit = hasArg(argc, argv, "--auto-exit");

	engine::EngineConfig cfg;
	cfg.windowTitle = "QGame Demo5 - 2.5D Forest Shadows and Water Reflections";
	cfg.windowWidth = 1280;
	cfg.windowHeight = 720;
	cfg.vsync = true;
	if (useOpenGL) {
		cfg.renderBackend = engine::RenderBackend::OpenGL;
	}

	engine::EngineContext ctx;
	ctx.init(cfg);
	engine::GameAPI api{ ctx };

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
	constexpr float kFoodRadius = 8.0f;      // 食物半径
	constexpr float kPlayMinX = 100.0f;    // 游戏区域边界
	constexpr float kPlayMaxX = 1180.0f;
	constexpr float kPlayMinY = 60.0f;
	constexpr float kPlayMaxY = 660.0f;
	int foodDiam = static_cast<int>(kFoodRadius * 2);

	TextureHandle foodTex = makeCircleTex(api, foodDiam / 2, core::Color::White);
	auto head = api.spawnEntity();
	float margin = kFoodRadius + 4.0f;
	float fx = kPlayMinX + margin
		+ static_cast<float>(std::rand()) / RAND_MAX
		* (kPlayMaxX - kPlayMinX - 2.0f * margin);
	float fy = kPlayMinY + margin
		+ static_cast<float>(std::rand()) / RAND_MAX
		* (kPlayMaxY - kPlayMinY - 2.0f * margin);
	api.addComponent(head, engine::Transform{ 640.f, 360.f });
	{
		engine::Sprite sp;
		sp.texture = foodTex;
		sp.srcRect = { 0.f, 0.f, static_cast<float>(foodDiam), static_cast<float>(foodDiam) };
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
		col.width = static_cast<float>(foodDiam);
		col.height = static_cast<float>(foodDiam);
		col.offsetX = 0;
		col.offsetY = 0;
		col.isTrigger = true;
		col.mask = 0;
		api.addComponent(head, col);
	}

	while (ctx.scheduler.tick()) {
		const float dt = ctx.scheduler.deltaTime();


		if (api.isKeyJustPressed(SDLK_ESCAPE)) {
			api.quit();
			break;
		}

	}

	ctx.shutdown();
	return 0;
}
