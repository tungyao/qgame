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

#include <engine/api/GameAPI.h>
#include <engine/components/PhysicsComponents.h>
#include <engine/components/RenderComponents.h>
#include <engine/runtime/EngineConfig.h>
#include <engine/runtime/EngineContext.h>
#include <engine/systems/ISystem.h>
#include <engine/systems/RenderSystem.h>

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
        return core::Color{static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b), 255};
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

    TextureHandle tex = api.createTextureFromMemory(pixels, w, h);
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

// Demo6CameraFollowSystem 把 demo 层“玩家输入 -> 物理速度”和“物理结果 -> 相机跟随”
// 拆成一个真正参与引擎帧调度的 System。
//
// 这么做的原因是：
// 1. 之前 demo6 把控制逻辑写在 scheduler.tick() 之后。
//    但 tick() 内部已经执行了：
//      Input -> Physics -> UISystem -> RenderSystem -> Present
//    所以玩家速度和相机位置都会晚一帧生效。
//
// 2. 对 RigidBody 来说，“晚一帧写 velocity”最容易引出固定时间步下的偶发停顿：
//    某些帧 PhysicsSystem 已经消费完旧速度了，本帧 render 却还在看旧相机，
//    观感上就像角色平移时会轻微卡一下。
//
// 3. 通过 System 接入后：
//    - preUpdate() 在 PhysicsSystem 之前，负责写玩家 velocity
//    - update(dt) 在 PhysicsSystem 之后、RenderSystem 之前，负责相机跟随
//    这样同一帧的输入、物理和渲染就重新对齐了。
class Demo6CameraFollowSystem final : public engine::ISystem {
public:
    Demo6CameraFollowSystem(engine::EngineContext& ctx,
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
        , fallbackViewportH_(fallbackViewportH) {}

    void preUpdate() override {
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

    void update(float dt) override {
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

        const int viewportW = ctx_.window ? ctx_.window->width() : fallbackViewportW_;
        const int viewportH = ctx_.window ? ctx_.window->height() : fallbackViewportH_;
        const float effectiveZoom = applyAutoCameraZoom(cam, viewportW, viewportH);

        const float alpha = 1.0f - std::exp(-kCameraFollowRate * std::max(0.0f, dt));
        camTf.x += (playerTf.x - camTf.x) * alpha;
        camTf.y += (playerTf.y - camTf.y) * alpha;

        clampCameraToMap(camTf, effectiveZoom, viewportW, viewportH);

        // Transform 被 PhysicsSystem/RenderSystem 订阅；跟随镜头自己改位置后需要 patch，
        // 这样 GPU 路径和任何依赖 on_update<Transform> 的逻辑都能拿到最新结果。
        ctx_.world.patch<engine::Transform>(camera_);
    }

private:
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
        } else {
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
        } else {
            camTf.x = mapPixelW_ * 0.5f;
        }

        if (mapPixelH_ > visibleWorldH) {
            camTf.y = std::clamp(camTf.y, halfViewH, mapPixelH_ - halfViewH);
        } else {
            camTf.y = mapPixelH_ * 0.5f;
        }
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

} // namespace

int main(int argc, char** argv) {
    const bool useOpenGL  = hasArg(argc, argv, "--opengl");
    const bool autoExit   = hasArg(argc, argv, "--auto-exit");
    const char* pkgPath   = getArg(argc, argv, "--package", "tilemap_engine_package.json");

    engine::EngineConfig cfg;
    cfg.windowTitle  = "QGame Demo6 — TileMap Engine Package Loader";
    cfg.windowWidth  = 1280;
    cfg.windowHeight = 720;
    cfg.vsync = true;
    if (useOpenGL) cfg.renderBackend = engine::RenderBackend::OpenGL;

    engine::EngineContext ctx;
    ctx.init(cfg);
    engine::GameAPI api{ctx};

    // 默认 RenderSystem 已在 EngineContext::init() 末尾注册。
    // demo6 需要在 Physics 之后、Render 之前插入一层“玩法控制 + 相机跟随”，
    // 所以先把现有 RenderSystem 卸载，等 demo system 注册完再加回去。
    auto* oldRenderSystem = &ctx.systems.get<engine::RenderSystem>();
    ctx.systems.unregisterSystem(oldRenderSystem, true);

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
    } catch (const std::exception& e) {
        std::fprintf(stderr, "JSON parse error: %s\n", e.what());
        return 1;
    }

    const auto& tileMap = root.value("tileMap", nlohmann::json::object());
    int mapW     = tileMap.value("w", 20);
    int mapH     = tileMap.value("h", 15);
    int tileSize = tileMap.value("ts", 32);
    const float mapPixelW = static_cast<float>(mapW * tileSize);
    const float mapPixelH = static_cast<float>(mapH * tileSize);

    // ── camera ───────────────────────────────────────────────────────────────
    auto camEnt = api.spawnEntity();
    // 相机先放一个临时位置。真正的跟随逻辑会在 player 创建后，把它平滑拉向玩家。
    api.addComponent(camEnt, engine::Transform{640.f, 360.f});
    engine::Camera wcam{};
    // zoom 不再写死；主循环里会按窗口尺寸和地图尺寸自动解算。
    wcam.zoom       = 1.f;
    wcam.primary    = true;
    wcam.depth      = 0;
    wcam.layerMask  = engine::renderPassBit(engine::RenderPass::World);
    wcam.clear      = true;
    wcam.clearColor = core::Color{8, 12, 22, 255};
    wcam.cullEnabled = false; // don't cull tiles
    api.addComponent(camEnt, wcam);

    // ── build TileMap component ──────────────────────────────────────────────
    engine::TileMap tmap;
    tmap.width    = mapW;
    tmap.height   = mapH;
    tmap.tileSize = tileSize;

    std::fprintf(stdout, "=== Demo6: TileMap Engine Package Loader ===\n");
    std::fprintf(stdout, "Package: %s\n", pkgPath);
    std::fprintf(stdout, "Map: %dx%d tiles @ %dpx\n", mapW, mapH, tileSize);

    if (tileMap.contains("tilesets")) {
        int tsIdx = 0;
        for (const auto& tj : tileMap["tilesets"]) {
            engine::TileMap::Tileset ts;
            ts.firstGid = tj.value("firstGid", 0);
            ts.count    = tj.value("count", 0);
            ts.columns  = tj.value("cols", 1);

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
                ts.legacyCollision = tj["collision"].get<std::vector<uint8_t>>();
                if (ts.collisions.empty()) {
                    for (int local = 0; local < static_cast<int>(ts.legacyCollision.size()); ++local) {
                        if (ts.legacyCollision[local] == 0) continue;
                        engine::TileMap::TileCollision collision;
                        collision.gid = ts.firstGid + local;
                        collision.shape = engine::TileMap::TileCollisionShape::Full;
                        ts.collisions.push_back(std::move(collision));
                    }
                }
            }

            std::string kind    = tj.value("sourceKind", "");
            std::string dataUrl = tj.value("sourceDataUrl", "");
            std::string name    = tj.value("name", "?");

            if (kind == "builtin" || ts.firstGid == 0) {
                auto atlas = makeBuiltinAtlas(tileSize, ts.columns, ts.count);
                int atlasW = ts.columns * tileSize;
                int atlasH = ((ts.count + ts.columns - 1) / ts.columns) * tileSize;
                ts.texture = api.createTextureFromMemory(atlas.data(), atlasW, atlasH);
                std::fprintf(stdout, "  tileset[%d] BUILTIN %s  gid=%d..%d  cols=%d  tex=%dx%d\n",
                             tsIdx, name.c_str(), ts.firstGid,
                             ts.firstGid + ts.count - 1, ts.columns, atlasW, atlasH);
            } else {
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
            layer.name        = lj.value("name", "");
            layer.visible     = lj.value("visible", true);
            layer.collidable  = lj.value("collidable", true);
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
    api.addComponent(mapEnt, engine::Transform{0.f, 0.f});
    api.addComponent(mapEnt, std::move(tmap));


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
    engine::Collider collider{32.f, 32.f, 0.f, 0.f, false};
    collider.layer = engine::COLLISION_LAYER_PLAYER;
    collider.mask  = engine::COLLISION_LAYER_STATIC;
    api.addComponent(player, collider);
    api.addComponent(player, engine::RigidBody{0.f, 0.f, 0.f, false});

    // 让相机一开始就对准玩家，避免首帧先从窗口中心“飞”过去。
    {
        auto& camTf = api.getComponent<engine::Transform>(camEnt);
        const auto& playerTf = api.getComponent<engine::Transform>(player);
        camTf.x = playerTf.x;
        camTf.y = playerTf.y;
    }

    auto& followSystem = ctx.systems.registerSystem<Demo6CameraFollowSystem>(
        ctx, player, camEnt, mapPixelW, mapPixelH, tileSize, cfg.windowWidth, cfg.windowHeight);
    followSystem.init();

    auto& renderSystem = ctx.systems.registerSystem<engine::RenderSystem>(ctx);
    renderSystem.init();

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
