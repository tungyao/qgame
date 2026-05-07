#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <algorithm>

#include <engine/api/GameAPI.h>
#include <engine/components/PhysicsComponents.h>
#include <engine/components/RenderComponents.h>
#include <engine/runtime/EngineConfig.h>
#include <engine/runtime/EngineContext.h>

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

    // ── camera ───────────────────────────────────────────────────────────────
    auto camEnt = api.spawnEntity();
    api.addComponent(camEnt, engine::Transform{640.f, 360.f});
    engine::Camera wcam{};
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

            if (tj.contains("collision") && tj["collision"].is_array()) {
                ts.collision = tj["collision"].get<std::vector<uint8_t>>();
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
        for (int i = 0; i < static_cast<int>(ts.collision.size()); ++i) {
            if (ts.collision[i]) {
                if (!first) std::fprintf(stdout, ", ");
                std::fprintf(stdout, "gid=%d", ts.firstGid + i);
                first = false;
            }
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


    std::fprintf(stdout, "\nControls: WASD/arrows=pan  Q/E=zoom  ESC=quit\n");

    // ── main loop ────────────────────────────────────────────────────────────
    float t = 0.f;
    constexpr float kSpeed = 200.f;
    while (ctx.scheduler.tick()) {
        const float dt = ctx.scheduler.deltaTime();
        t += dt;

        float dx = 0.f, dy = 0.f;
        if (api.isKeyDown(SDLK_W)) dy -= 1.f;
        if (api.isKeyDown(SDLK_S)) dy += 1.f;
        if (api.isKeyDown(SDLK_A)) dx -= 1.f;
        if (api.isKeyDown(SDLK_D)) dx += 1.f;
        const bool isMoving = (dx != 0.f || dy != 0.f);

        auto& rb = api.getComponent<engine::RigidBody>(player);
        rb.velocityX = isMoving ? dx * kSpeed : 0.f;
        rb.velocityY = isMoving ? dy * kSpeed : 0.f;

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
