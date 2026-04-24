#include "SceneSerializer.h"
#include "../assets/AssetManager.h"
#include "../components/RenderComponents.h"
#include "../components/PhysicsComponents.h"
#include "../../core/Logger.h"
#include "../../platform/FileSystem.h"

#include <nlohmann/json.hpp>
#include <fstream>
#include <vector>
#include <cstdint>
#include <unordered_map>

using json = nlohmann::json;

namespace engine {

// ── JSON 序列化辅助（组件 to/from json）────────────────────────────────────────

static json entityIdToJson(const EntityID& id) {
    return {{"id", id.c_str()}};
}
static EntityID entityIdFromJson(const json& j) {
    return EntityID{j.value("id", "").c_str()};
}

static json nameToJson(const Name& n) {
    return {{"s", n.c_str()}};
}
static Name nameFromJson(const json& j) {
    return Name{j.value("s", "").c_str()};
}

static json transformToJson(const Transform& t) {
    return {{"x", t.x}, {"y", t.y}, {"rot", t.rotation},
            {"sx", t.scaleX}, {"sy", t.scaleY}};
}
static Transform transformFromJson(const json& j) {
    Transform t;
    t.x        = j.value("x",   0.f);
    t.y        = j.value("y",   0.f);
    t.rotation = j.value("rot", 0.f);
    t.scaleX   = j.value("sx",  1.f);
    t.scaleY   = j.value("sy",  1.f);
    return t;
}

static json spriteToJson(const Sprite& s, AssetManager& mgr) {
    return {
        {"tex",    mgr.texturePath(s.texture)},
        {"srcX",   s.srcRect.x},  {"srcY",  s.srcRect.y},
        {"srcW",   s.srcRect.w},  {"srcH",  s.srcRect.h},
        {"layer",  s.layer},
        {"tintR",  (int)s.tint.r}, {"tintG", (int)s.tint.g},
        {"tintB",  (int)s.tint.b}, {"tintA", (int)s.tint.a},
        {"pivX",   s.pivotX}, {"pivY",  s.pivotY}
    };
}
static Sprite spriteFromJson(const json& j, AssetManager& mgr) {
    Sprite s;
    std::string texPath = j.value("tex", "");
    if (!texPath.empty()) s.texture = mgr.loadTexture(texPath);
    s.srcRect = core::Rect{j.value("srcX", 0.f), j.value("srcY", 0.f),
                           j.value("srcW", 0.f), j.value("srcH", 0.f)};
    s.layer   = j.value("layer", 0);
    s.tint    = core::Color{(uint8_t)j.value("tintR", 255),
                            (uint8_t)j.value("tintG", 255),
                            (uint8_t)j.value("tintB", 255),
                            (uint8_t)j.value("tintA", 255)};
    s.pivotX  = j.value("pivX", 0.5f);
    s.pivotY  = j.value("pivY", 0.5f);
    return s;
}

static json tilemapToJson(const TileMap& tm, AssetManager& mgr) {
    json j;
    j["w"]    = tm.width;
    j["h"]    = tm.height;
    j["ts"]   = tm.tileSize;
    j["cols"] = tm.tilesetCols;
    j["tex"]  = mgr.texturePath(tm.tileset);
    for (int l = 0; l < TileMap::MAX_LAYERS; ++l)
        j["layers"][l] = tm.layers[l];
    return j;
}
static TileMap tilemapFromJson(const json& j, AssetManager& mgr) {
    TileMap tm;
    tm.width       = j.value("w",    0);
    tm.height      = j.value("h",    0);
    tm.tileSize    = j.value("ts",   16);
    tm.tilesetCols = j.value("cols", 1);
    std::string texPath = j.value("tex", "");
    if (!texPath.empty()) tm.tileset = mgr.loadTexture(texPath);
    if (j.contains("layers")) {
        for (int l = 0; l < TileMap::MAX_LAYERS; ++l) {
            if (l < (int)j["layers"].size())
                tm.layers[l] = j["layers"][l].get<std::vector<int>>();
        }
    }
    return tm;
}

static json cameraToJson(const Camera& c) {
    return {{"zoom", c.zoom}, {"primary", c.primary}};
}
static Camera cameraFromJson(const json& j) {
    Camera c;
    c.zoom    = j.value("zoom",    1.f);
    c.primary = j.value("primary", true);
    return c;
}

static json rigidBodyToJson(const RigidBody& rb) {
    return {{"vx", rb.velocityX}, {"vy", rb.velocityY},
            {"gs", rb.gravityScale}, {"kin", rb.isKinematic}};
}
static RigidBody rigidBodyFromJson(const json& j) {
    RigidBody rb;
    rb.velocityX    = j.value("vx",  0.f);
    rb.velocityY    = j.value("vy",  0.f);
    rb.gravityScale = j.value("gs",  0.f);
    rb.isKinematic  = j.value("kin", false);
    return rb;
}

static json colliderToJson(const Collider& c) {
    return {{"w", c.width}, {"h", c.height},
            {"ox", c.offsetX}, {"oy", c.offsetY}, {"trig", c.isTrigger}};
}
static Collider colliderFromJson(const json& j) {
    Collider c;
    c.width     = j.value("w",    0.f);
    c.height    = j.value("h",    0.f);
    c.offsetX   = j.value("ox",   0.f);
    c.offsetY   = j.value("oy",   0.f);
    c.isTrigger = j.value("trig", false);
    return c;
}

// ── Save ──────────────────────────────────────────────────────────────────────

bool SceneSerializer::saveScene(entt::registry& reg,
                                AssetManager& mgr,
                                const std::string& path) {
    json root;
    root["version"] = 1;
    json& entities  = root["entities"];

    for (auto e : reg.storage<entt::entity>()) {
        json je;

        if (auto* c = reg.try_get<EntityID>(e))
            je["EntityID"] = entityIdToJson(*c);
        if (auto* c = reg.try_get<Name>(e))
            je["Name"] = nameToJson(*c);
        if (auto* c = reg.try_get<Transform>(e))
            je["Transform"] = transformToJson(*c);
        if (auto* c = reg.try_get<Sprite>(e))
            je["Sprite"] = spriteToJson(*c, mgr);
        if (auto* c = reg.try_get<TileMap>(e))
            je["TileMap"] = tilemapToJson(*c, mgr);
        if (auto* c = reg.try_get<Camera>(e))
            je["Camera"] = cameraToJson(*c);
        if (auto* c = reg.try_get<RigidBody>(e))
            je["RigidBody"] = rigidBodyToJson(*c);
        if (auto* c = reg.try_get<Collider>(e))
            je["Collider"] = colliderToJson(*c);

        entities.push_back(std::move(je));
    }

    std::ofstream ofs(path);
    if (!ofs) {
        core::logError("[SceneSerializer] cannot write: %s", path.c_str());
        return false;
    }
    ofs << root.dump(2);
    return true;
}

// ── Load ──────────────────────────────────────────────────────────────────────

bool SceneSerializer::loadScene(entt::registry& reg,
                                AssetManager& mgr,
                                const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs) {
        core::logError("[SceneSerializer] cannot read: %s", path.c_str());
        return false;
    }

    json root;
    try { ifs >> root; }
    catch (const json::exception& ex) {
        core::logError("[SceneSerializer] JSON parse error: %s", ex.what());
        return false;
    }

    reg.clear();

    std::unordered_map<std::string, entt::entity> idToEntity;

    for (const auto& je : root["entities"]) {
        EntityID eid;
        if (je.contains("EntityID"))
            eid = entityIdFromJson(je["EntityID"]);

        entt::entity e = reg.create();

        if (eid.valid()) {
            idToEntity[eid.c_str()] = e;
            reg.emplace<EntityID>(e, eid);
        }

        if (je.contains("Name"))
            reg.emplace<Name>(e, nameFromJson(je["Name"]));
        if (je.contains("Transform"))
            reg.emplace<Transform>(e, transformFromJson(je["Transform"]));
        if (je.contains("Sprite"))
            reg.emplace<Sprite>(e, spriteFromJson(je["Sprite"], mgr));
        if (je.contains("TileMap"))
            reg.emplace<TileMap>(e, tilemapFromJson(je["TileMap"], mgr));
        if (je.contains("Camera"))
            reg.emplace<Camera>(e, cameraFromJson(je["Camera"]));
        if (je.contains("RigidBody"))
            reg.emplace<RigidBody>(e, rigidBodyFromJson(je["RigidBody"]));
        if (je.contains("Collider"))
            reg.emplace<Collider>(e, colliderFromJson(je["Collider"]));
    }

    core::logInfo("[SceneSerializer] loaded %zu entities from %s",
                       root["entities"].size(), path.c_str());
    return true;
}

} // namespace engine
