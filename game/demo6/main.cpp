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

	constexpr const char* kBuiltinColors[] = {
		"#5a9f4d", "#6fb85f", "#a47b48", "#7f5f3b",
		"#4a8cbf", "#3d76a3", "#c9b36a", "#d9cf8f",
		"#6d737a", "#89919a", "#9c5d4e", "#bd7a69",
		"#2f6f43", "#255838", "#b7b7b7", "#e3e3e3"
	};

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

	core::Color parseHexColor(const char* hex) {
		if (!hex || hex[0] != '#') return core::Color::White;
		unsigned int r = 0, g = 0, b = 0;
		if (std::sscanf(hex + 1, "%02x%02x%02x", &r, &g, &b) == 3) {
			return core::Color{ static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b), 255 };
		}
		return core::Color::White;
	}

	/**
	 * demo6 直接从 engine package JSON 构造 TileMap，因此这里需要和运行时
	 * ComponentJson 的 v2 约定保持一致，显式解析字符串 shape。
	 */
	engine::TileMap::TileCollisionShape parseTileCollisionShape(const nlohmann::json& j, const char* key) {
		const std::string value = j.value(key, "none");
		if (value == "full") return engine::TileMap::TileCollisionShape::Full;
		if (value == "rect") return engine::TileMap::TileCollisionShape::Rect;
		if (value == "polygon") return engine::TileMap::TileCollisionShape::Polygon;
		if (value == "oneWay") return engine::TileMap::TileCollisionShape::OneWay;
		if (value == "trigger") return engine::TileMap::TileCollisionShape::Trigger;
		return engine::TileMap::TileCollisionShape::None;
	}

	std::string readFile(const std::string& path) {
		std::ifstream ifs(path, std::ios::binary);
		if (!ifs.is_open()) return {};
		std::ostringstream oss;
		oss << ifs.rdbuf();
		return oss.str();
	}

	// ── base64 decode ────────────────────────────────────────────────────────────────
	std::string base64Decode(const std::string& input) {
		static const char kTable[] =
			"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		static int kDec[256];
		static bool kInit = false;
		if (!kInit) {
			kInit = true;
			for (int i = 0; i < 256; ++i) kDec[i] = -1;
			for (int i = 0; i < 64; ++i) kDec[static_cast<unsigned char>(kTable[i])] = i;
		}

		std::string out;
		out.reserve(input.size() * 3 / 4);
		int val = 0, bits = 0;
		for (char c : input) {
			int idx = kDec[static_cast<unsigned char>(c)];
			if (idx < 0) continue;
			val = (val << 6) | idx;
			bits += 6;
			if (bits >= 8) {
				bits -= 8;
				out.push_back(static_cast<char>((val >> bits) & 0xFF));
			}
		}
		return out;
	}

	// ── procedural builtin tileset atlas ─────────────────────────────────────────────
	std::vector<uint8_t> makeBuiltinAtlas(int tileSize, int cols, int count) {
		int rows = (count + cols - 1) / cols;
		int w = cols * tileSize;
		int h = rows * tileSize;
		std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4, 0);

		for (int ti = 0; ti < count; ++ti) {
			int cx = (ti % cols) * tileSize;
			int cy = (ti / cols) * tileSize;
			core::Color base = parseHexColor(kBuiltinColors[ti % 16]);

			for (int dy = 0; dy < tileSize; ++dy) {
				for (int dx = 0; dx < tileSize; ++dx) {
					size_t i = (static_cast<size_t>(cy + dy) * w + (cx + dx)) * 4;
					px[i + 0] = base.r;
					px[i + 1] = base.g;
					px[i + 2] = base.b;
					px[i + 3] = 255;
				}
			}
			// lighter overlay stripe (matches editor appearance)
			int overlayH = std::max(2, tileSize * 3 / 16);
			for (int dy = 3; dy < 3 + overlayH && dy < tileSize; ++dy) {
				for (int dx = 3; dx < tileSize - 6; ++dx) {
					if (dx < 0 || dx >= tileSize) continue;
					size_t i = (static_cast<size_t>(cy + dy) * w + (cx + dx)) * 4;
					px[i + 0] = std::min(255, px[i + 0] + 40);
					px[i + 1] = std::min(255, px[i + 1] + 40);
					px[i + 2] = std::min(255, px[i + 2] + 40);
				}
			}
		}
		return px;
	}

	// ── decode data:image/...;base64,... → RGBA pixels ──────────────────────────────
	TextureHandle textureFromDataUrl(engine::GameAPI& api,
		const std::string& dataUrl,
		int& outW, int& outH) {
		if (dataUrl.empty()) return {};

		auto pos = dataUrl.find(";base64,");
		if (pos == std::string::npos) return {};

		std::string raw = base64Decode(dataUrl.substr(pos + 8));
		if (raw.empty()) return {};

		int w = 0, h = 0, ch = 0;
		unsigned char* pixels = stbi_load_from_memory(
			reinterpret_cast<const stbi_uc*>(raw.data()),
			static_cast<int>(raw.size()), &w, &h, &ch, 4);
		if (!pixels) {
			std::fprintf(stderr, "stbi failed: %s\n", stbi_failure_reason());
			return {};
		}

		TextureHandle tex = api.createTextureFromMemory(pixels, w, h, backend::TextureFilter::Linear);
		outW = w;
		outH = h;
		stbi_image_free(pixels);
		return tex;
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

	/**
	 * 玩家输入系统 - 在 GameplayPrePhysics 阶段读取输入并更新玩家 Transform。
	 *
	 * 为何不能放在主循环里做（在 ctx.scheduler.tick() 之后）：
	 * 渲染发生在 Render phase（tick 内部），若位置更新在 tick 之后，则本帧渲染
	 * 看到的是上一帧末尾的位置，形成一帧延迟。当帧时间抖动时（比如 SDL_GetTicks
	 * 毫秒精度导致的 dt 跳变），这一帧延迟会把不均匀的帧时间压缩到单帧视觉里，
	 * 表现为「卡一下然后瞬移」的断断续续。
	 */
	class PlayerInputSystem : public engine::ISystem {
	public:
		PlayerInputSystem(engine::EngineContext& ctx, entt::entity player)
			: ctx_(ctx), player_(player) {}

		engine::UpdatePhaseMask phaseMask() const override {
			return engine::updatePhaseBit(engine::UpdatePhase::GameplayPrePhysics);
		}

	protected:
		void onGameplayPrePhysicsPhase(float dt) override {
			float dx = 0.f;
			float dy = 0.f;
			if (ctx_.inputState.isKeyDown(SDLK_W) || ctx_.inputState.isKeyDown(SDLK_UP)) dy -= 1.f;
			if (ctx_.inputState.isKeyDown(SDLK_S) || ctx_.inputState.isKeyDown(SDLK_DOWN)) dy += 1.f;
			if (ctx_.inputState.isKeyDown(SDLK_A) || ctx_.inputState.isKeyDown(SDLK_LEFT)) dx -= 1.f;
			if (ctx_.inputState.isKeyDown(SDLK_D) || ctx_.inputState.isKeyDown(SDLK_RIGHT)) dx += 1.f;

			const float lenSq = dx * dx + dy * dy;
			if (lenSq > 1.0f) {
				const float invLen = 1.0f / std::sqrt(lenSq);
				dx *= invLen;
				dy *= invLen;
			}

			static constexpr float kPlayerSpeed = 300.0f;
			ctx_.world.patch<engine::Transform>(player_, [&](engine::Transform& tf) {

				tf.x += dx * kPlayerSpeed * dt;
				tf.y += dy * kPlayerSpeed * dt;
				});
			ctx_.world.patch<engine::RigidBody>(player_, [&](engine::RigidBody& rd) {
				rd.velocityX = dx * kPlayerSpeed;
				rd.velocityY = dy * kPlayerSpeed;
				});
				// 手动同步插值状态，消除本帧延迟
			//if (auto* interp = ctx_.world.try_get<engine::TransformInterpolation>(player_)) {
			//	interp->previous = ctx_.world.get<engine::Transform>(player_);
			//}
			
		}

	private:
		engine::EngineContext& ctx_;
		entt::entity player_;
	};

} // anonymous namespace

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

	// 让物理更新匹配真实帧率，消除固定步长在高帧率下的跳动/拖影
	if (ctx.systems.has<engine::PhysicsSystem>()) {
		ctx.systems.get<engine::PhysicsSystem>().setVariableTimestep(true);
	}

	api.loadAssetManifest(QGAME_BAKED_MANIFEST);
	const engine::FontHandle font = api.loadFontById("font.demo.main");

	// ── load JSON ────────────────────────────────────────────────────────────
	std::string jsonStr = readFile(pkgPath);
	if (jsonStr.empty()) {
		std::fprintf(stderr, "Cannot open: %s\n", pkgPath);
		std::fprintf(stderr, "Usage: demo6 [--package path] [--opengl] [--auto-exit]\n");
		return 1;
	}

	nlohmann::json root;
	try {
		root = nlohmann::json::parse(jsonStr);
	}
	catch (const std::exception& e) {
		std::fprintf(stderr, "JSON parse error: %s\n", e.what());
		return 1;
	}

	const auto& tileMap = root.value("tileMap", nlohmann::json::object());
	int mapW = tileMap.value("w", 20);
	int mapH = tileMap.value("h", 15);
	int tileSize = tileMap.value("ts", 32);
	const float mapPixelW = static_cast<float>(mapW * tileSize);
	const float mapPixelH = static_cast<float>(mapH * tileSize);

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
	wcam.cullEnabled = true; // don't cull tiles
	// 关闭相机级 pixelSnap，避免平滑跟随时的顿挫感。
	// 次像素抖动改在顶点着色器内做屏幕空间 round（方法1），
	// 这样相机坐标保持浮点平滑，sprite 边缘仍对齐物理像素。
	wcam.pixelSnap = true;
	api.addComponent(camEnt, wcam);

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

	// ── build TileMap component ──────────────────────────────────────────────
	engine::TileMap tmap;
	tmap.width = mapW;
	tmap.height = mapH;
	tmap.tileSize = tileSize;

	std::fprintf(stdout, "=== Demo6: TileMap Engine Package Loader ===\n");
	std::fprintf(stdout, "Package: %s\n", pkgPath);
	std::fprintf(stdout, "Map: %dx%d tiles @ %dpx\n", mapW, mapH, tileSize);

	if (tileMap.contains("tilesets")) {
		int tsIdx = 0;
		for (const auto& tj : tileMap["tilesets"]) {
			engine::TileMap::Tileset ts;
			ts.firstGid = tj.value("firstGid", 0);
			ts.count = tj.value("count", 0);
			ts.columns = tj.value("cols", 1);

			// v2 collision profiles are the canonical runtime data. Keep reading
			// legacy collision[] as a fallback so old sample packages still run.
			if (tj.contains("collisions") && tj["collisions"].is_array()) {
				for (const auto& cj : tj["collisions"]) {
					engine::TileMap::TileCollision collision;
					collision.gid = cj.value("gid", engine::TileMap::EMPTY_GID);
					collision.shape = parseTileCollisionShape(cj, "shape");
					if (cj.contains("points") && cj["points"].is_array()) {
						collision.points = cj["points"].get<std::vector<float>>();
					}
					ts.collisions.push_back(std::move(collision));
				}
			}

			if (tj.contains("collision") && tj["collision"].is_array()) {
				std::vector<uint8_t> legacyCollision = tj["collision"].get<std::vector<uint8_t>>();
				if (ts.collisions.empty()) {
					for (int local = 0; local < static_cast<int>(legacyCollision.size()); ++local) {
						if (legacyCollision[local] == 0) continue;
						engine::TileMap::TileCollision collision;
						collision.gid = ts.firstGid + local;
						collision.shape = engine::TileMap::TileCollisionShape::Full;
						ts.collisions.push_back(std::move(collision));
					}
				}
			}

			std::string kind = tj.value("sourceKind", "");
			std::string dataUrl = tj.value("sourceDataUrl", "");
			std::string name = tj.value("name", "?");

			if (kind == "builtin" || ts.firstGid == 0) {
				auto atlas = makeBuiltinAtlas(tileSize, ts.columns, ts.count);
				int atlasW = ts.columns * tileSize;
				int atlasH = ((ts.count + ts.columns - 1) / ts.columns) * tileSize;
				ts.texture = api.createTextureFromMemory(atlas.data(), atlasW, atlasH, backend::TextureFilter::Linear);
				std::fprintf(stdout, "  tileset[%d] BUILTIN %s  gid=%d..%d  cols=%d  tex=%dx%d\n",
					tsIdx, name.c_str(), ts.firstGid,
					ts.firstGid + ts.count - 1, ts.columns, atlasW, atlasH);
			}
			else {
				int texW = 0, texH = 0;
				ts.texture = textureFromDataUrl(api, dataUrl, texW, texH);
				std::fprintf(stdout, "  tileset[%d] IMAGE %s  gid=%d..%d  cols=%d  tex=%dx%d  %s\n",
					tsIdx, name.c_str(), ts.firstGid,
					ts.firstGid + ts.count - 1, ts.columns, texW, texH,
					ts.texture.valid() ? "ok" : "FAILED");
			}

			if (!ts.texture.valid()) {
				std::fprintf(stderr, "  WARNING: tileset %s has no valid texture\n", name.c_str());
			}

			tmap.tilesets.push_back(std::move(ts));
			++tsIdx;
		}
	}

	if (tileMap.contains("layers")) {
		int li = 0;
		for (const auto& lj : tileMap["layers"]) {
			engine::TileMap::Layer layer;
			layer.name = lj.value("name", "");
			layer.visible = lj.value("visible", true);
			layer.collidable = lj.value("collidable", true);
			layer.renderLayer = lj.value("renderLayer", li);
			if (lj.contains("tiles") && lj["tiles"].is_array()) {
				layer.tiles = lj["tiles"].get<std::vector<int>>();
			}
			tmap.layers.push_back(std::move(layer));
			++li;
		}
	}

	std::fprintf(stdout, "Layers: %zu\n", tmap.layers.size());
	std::fprintf(stdout, "Collision tiles: ");
	bool first = true;
	for (const auto& ts : tmap.tilesets) {
		for (const auto& collision : ts.collisions) {
			if (collision.shape == engine::TileMap::TileCollisionShape::None) continue;
			if (!first) std::fprintf(stdout, ", ");
			std::fprintf(stdout, "gid=%d", collision.gid);
			first = false;
		}
	}
	std::fprintf(stdout, "%s\n", first ? "(none)" : "");

	// ── spawn tilemap entity ─────────────────────────────────────────────────
	auto mapEnt = api.spawnEntity();
	api.addComponent(mapEnt, engine::Transform{ 0.f, 0.f });
	api.addComponent(mapEnt, std::move(tmap));


	// player
	auto checkerPx = makeCheckerboard(32, 32, 1, { 255,255,255,255 }, { 255,255,255,255 });
	TextureHandle spriteTex = api.createTextureFromMemory(checkerPx.data(), 32, 32, backend::TextureFilter::Linear);
	spriteTex = api.loadTexture("C:\\Users\\Administrator\\Downloads\\f0m.png");
	//ctx.renderDevice().getTextureDimensions(spriteTex, w, h);
	int w = 0, h = 0;
	if (api.getTextureDimensions(spriteTex, w, h)) {
		printf("texture: %dx%d\n", w, h);
	}
	auto player = api.spawnEntity();
	engine::Sprite sprite{};
	sprite.texture = spriteTex;
	sprite.srcRect = { 0, 0, w, h };
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

	// 注册玩家输入系统（在 systems.initAll() 之后新增的系统需要手动 init）。
	// 它会在 GameplayPrePhysics 阶段（即 Physics 之前、Render 之前）读取输入
	// 更新 Transform，从而消除「在主循环里改位置 → 渲染看到的总是上一帧位置」
	// 导致的断续瞬移问题。
	auto& playerInputSystem = ctx.systems.registerSystem<PlayerInputSystem>(ctx, player);

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
