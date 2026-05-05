#include "ComponentJson.h"

#include "../assets/AssetManager.h"
#include "../components/PhysicsComponents.h"
#include "../components/RenderComponents.h"
#include "../components/TextComponent.h"

#include <cstdint>
#include <vector>

namespace engine::scene_json {

// ── JSON 序列化辅助（组件 to/from json）────────────────────────────────────────

static Json entityIdToJson(const EntityID& id) {
    return {{"id", id.c_str()}};
}

static EntityID entityIdFromJson(const Json& j) {
    return EntityID{j.value("id", "").c_str()};
}

static Json nameToJson(const Name& n) {
    return {{"s", n.c_str()}};
}

static Name nameFromJson(const Json& j) {
    return Name{j.value("s", "").c_str()};
}

static Json transformToJson(const Transform& t) {
    return {{"x", t.x}, {"y", t.y}, {"rot", t.rotation},
            {"sx", t.scaleX}, {"sy", t.scaleY}};
}

static Transform transformFromJson(const Json& j) {
    Transform t;
    t.x        = j.value("x",   0.f);
    t.y        = j.value("y",   0.f);
    t.rotation = j.value("rot", 0.f);
    t.scaleX   = j.value("sx",  1.f);
    t.scaleY   = j.value("sy",  1.f);
    return t;
}

static Json spriteToJson(const Sprite& s, AssetManager& mgr) {
    Json j = {
        {"srcX",   s.srcRect.x},  {"srcY",  s.srcRect.y},
        {"srcW",   s.srcRect.w},  {"srcH",  s.srcRect.h},
        {"layer",  s.layer},
        {"sortOrder", s.sortOrder},
        {"ySort", s.ySort},
        {"visible", s.visible},
        {"pass", static_cast<int>(s.pass)},
        {"tintR",  (int)s.tint.r}, {"tintG", (int)s.tint.g},
        {"tintB",  (int)s.tint.b}, {"tintA", (int)s.tint.a},
        {"pivX",   s.pivotX}, {"pivY",  s.pivotY}
    };
    // Scene/Prefab files prefer stable IDs. The legacy path field remains as a
    // migration fallback so old tools can still inspect the original file.
    if (const std::string& id = mgr.textureAssetId(s.texture); !id.empty()) {
        j["assetId"] = id;
    }
    j["tex"] = mgr.texturePath(s.texture);
    return j;
}

static Sprite spriteFromJson(const Json& j, AssetManager& mgr) {
    Sprite s;
    const std::string texId = j.value("assetId", "");
    const std::string texPath = j.value("tex", "");
    if (!texId.empty()) s.texture = mgr.loadTextureById(texId);
    if (!s.texture.valid() && !texPath.empty()) s.texture = mgr.loadTexture(texPath);
    s.srcRect = core::Rect{j.value("srcX", 0.f), j.value("srcY", 0.f),
                           j.value("srcW", 0.f), j.value("srcH", 0.f)};
    s.layer     = j.value("layer", 0);
    s.sortOrder = j.value("sortOrder", 0);
    s.ySort     = j.value("ySort", false);
    s.visible   = j.value("visible", true);
    s.pass      = static_cast<RenderPass>(j.value("pass", static_cast<int>(RenderPass::World)));
    s.tint      = core::Color{(uint8_t)j.value("tintR", 255),
                              (uint8_t)j.value("tintG", 255),
                              (uint8_t)j.value("tintB", 255),
                              (uint8_t)j.value("tintA", 255)};
    s.pivotX    = j.value("pivX", 0.5f);
    s.pivotY    = j.value("pivY", 0.5f);
    return s;
}

static Json tilemapToJson(const TileMap& tm, AssetManager& mgr) {
    Json j;
    j["w"]    = tm.width;
    j["h"]    = tm.height;
    j["ts"]   = tm.tileSize;
    j["cols"] = tm.tilesetCols;
    if (const std::string& id = mgr.textureAssetId(tm.tileset); !id.empty()) {
        j["assetId"] = id;
    }
    j["tex"]  = mgr.texturePath(tm.tileset);
    for (int l = 0; l < TileMap::MAX_LAYERS; ++l) {
        j["layers"][l] = tm.layers[l];
    }
    return j;
}

static TileMap tilemapFromJson(const Json& j, AssetManager& mgr) {
    TileMap tm;
    tm.width       = j.value("w",    0);
    tm.height      = j.value("h",    0);
    tm.tileSize    = j.value("ts",   16);
    tm.tilesetCols = j.value("cols", 1);
    const std::string texId = j.value("assetId", "");
    const std::string texPath = j.value("tex", "");
    if (!texId.empty()) tm.tileset = mgr.loadTextureById(texId);
    if (!tm.tileset.valid() && !texPath.empty()) tm.tileset = mgr.loadTexture(texPath);
    if (j.contains("layers")) {
        for (int l = 0; l < TileMap::MAX_LAYERS; ++l) {
            if (l < static_cast<int>(j["layers"].size())) {
                tm.layers[l] = j["layers"][l].get<std::vector<int>>();
            }
        }
    }
    return tm;
}

static Json cameraToJson(const Camera& c) {
    return {{"zoom", c.zoom}, {"primary", c.primary},
            {"rotation", c.rotation}, {"depth", c.depth},
            {"layerMask", c.layerMask}, {"clear", c.clear},
            {"clearR", (int)c.clearColor.r}, {"clearG", (int)c.clearColor.g},
            {"clearB", (int)c.clearColor.b}, {"clearA", (int)c.clearColor.a},
            {"cullEnabled", c.cullEnabled}};
}

static Camera cameraFromJson(const Json& j) {
    Camera c;
    c.zoom        = j.value("zoom",        1.f);
    c.primary     = j.value("primary",     true);
    c.rotation    = j.value("rotation",    0.f);
    c.depth       = j.value("depth",       0);
    c.layerMask   = j.value("layerMask",   kCameraLayerMaskAll);
    c.clear       = j.value("clear",       true);
    c.clearColor  = core::Color{(uint8_t)j.value("clearR", 0),
                                (uint8_t)j.value("clearG", 0),
                                (uint8_t)j.value("clearB", 0),
                                (uint8_t)j.value("clearA", 255)};
    c.cullEnabled = j.value("cullEnabled", true);
    return c;
}

static Json rigidBodyToJson(const RigidBody& rb) {
    return {{"vx", rb.velocityX}, {"vy", rb.velocityY},
            {"gs", rb.gravityScale}, {"kin", rb.isKinematic}};
}

static RigidBody rigidBodyFromJson(const Json& j) {
    RigidBody rb;
    rb.velocityX    = j.value("vx",  0.f);
    rb.velocityY    = j.value("vy",  0.f);
    rb.gravityScale = j.value("gs",  0.f);
    rb.isKinematic  = j.value("kin", false);
    return rb;
}

static Json colliderToJson(const Collider& c) {
    return {{"w", c.width}, {"h", c.height},
            {"ox", c.offsetX}, {"oy", c.offsetY}, {"trig", c.isTrigger}};
}

static Collider colliderFromJson(const Json& j) {
    Collider c;
    c.width     = j.value("w",    0.f);
    c.height    = j.value("h",    0.f);
    c.offsetX   = j.value("ox",   0.f);
    c.offsetY   = j.value("oy",   0.f);
    c.isTrigger = j.value("trig", false);
    return c;
}

static Json textToJson(const TextComponent& t, AssetManager& mgr) {
    Json j = {
        {"text", t.text},
        {"fontSize", t.fontSize},
        {"layer", t.layer},
        {"sortOrder", t.sortOrder},
        {"ySort", t.ySort},
        {"pass", static_cast<int>(t.pass)},
        {"visible", t.visible},
        {"colorR", (int)t.color.r}, {"colorG", (int)t.color.g},
        {"colorB", (int)t.color.b}, {"colorA", (int)t.color.a}
    };
    if (const std::string& id = mgr.fontAssetId(t.font); !id.empty()) {
        j["fontId"] = id;
    }
    j["font"] = mgr.fontPath(t.font);
    return j;
}

static TextComponent textFromJson(const Json& j, AssetManager& mgr) {
    TextComponent t;
    t.text      = j.value("text", std::string{});
    t.fontSize  = j.value("fontSize", 16.f);
    t.layer     = j.value("layer", 10);
    t.sortOrder = j.value("sortOrder", 0);
    t.ySort     = j.value("ySort", false);
    t.pass      = static_cast<RenderPass>(j.value("pass", static_cast<int>(RenderPass::UI)));
    t.visible   = j.value("visible", true);
    t.color     = core::Color{(uint8_t)j.value("colorR", 255),
                              (uint8_t)j.value("colorG", 255),
                              (uint8_t)j.value("colorB", 255),
                              (uint8_t)j.value("colorA", 255)};

    const std::string fontId = j.value("fontId", "");
    const std::string fontPath = j.value("font", "");
    if (!fontId.empty()) t.font = mgr.loadFontById(fontId);
    if (!t.font.valid() && !fontPath.empty()) t.font = mgr.loadFont(fontPath);
    return t;
}

void writeKnownComponents(entt::registry& reg,
                          entt::entity entity,
                          AssetManager& assetMgr,
                          Json& out) {
    if (auto* c = reg.try_get<EntityID>(entity)) out["EntityID"] = entityIdToJson(*c);
    if (auto* c = reg.try_get<Name>(entity)) out["Name"] = nameToJson(*c);
    if (auto* c = reg.try_get<Transform>(entity)) out["Transform"] = transformToJson(*c);
    if (auto* c = reg.try_get<Sprite>(entity)) out["Sprite"] = spriteToJson(*c, assetMgr);
    if (auto* c = reg.try_get<TileMap>(entity)) out["TileMap"] = tilemapToJson(*c, assetMgr);
    if (auto* c = reg.try_get<Camera>(entity)) out["Camera"] = cameraToJson(*c);
    if (auto* c = reg.try_get<RigidBody>(entity)) out["RigidBody"] = rigidBodyToJson(*c);
    if (auto* c = reg.try_get<Collider>(entity)) out["Collider"] = colliderToJson(*c);
    if (auto* c = reg.try_get<TextComponent>(entity)) out["TextComponent"] = textToJson(*c, assetMgr);
}

void applyKnownComponents(entt::registry& reg,
                          entt::entity entity,
                          AssetManager& assetMgr,
                          const Json& components) {
    if (components.contains("EntityID")) {
        reg.emplace_or_replace<EntityID>(entity, entityIdFromJson(components["EntityID"]));
    }
    if (components.contains("Name")) {
        reg.emplace_or_replace<Name>(entity, nameFromJson(components["Name"]));
    }
    if (components.contains("Transform")) {
        reg.emplace_or_replace<Transform>(entity, transformFromJson(components["Transform"]));
    }
    if (components.contains("Sprite")) {
        reg.emplace_or_replace<Sprite>(entity, spriteFromJson(components["Sprite"], assetMgr));
    }
    if (components.contains("TileMap")) {
        reg.emplace_or_replace<TileMap>(entity, tilemapFromJson(components["TileMap"], assetMgr));
    }
    if (components.contains("Camera")) {
        reg.emplace_or_replace<Camera>(entity, cameraFromJson(components["Camera"]));
    }
    if (components.contains("RigidBody")) {
        reg.emplace_or_replace<RigidBody>(entity, rigidBodyFromJson(components["RigidBody"]));
    }
    if (components.contains("Collider")) {
        reg.emplace_or_replace<Collider>(entity, colliderFromJson(components["Collider"]));
    }
    if (components.contains("TextComponent")) {
        reg.emplace_or_replace<TextComponent>(entity, textFromJson(components["TextComponent"], assetMgr));
    }
}

Json collectComponentObject(const Json& entityJson) {
    Json components = Json::object();

    // New S2 shape:
    //   { "prefab": "prefab.game.player", "components": { ... } }
    // The "components" object is copied first, then old top-level fields below
    // can still override it if a hand-authored file mixes both during migration.
    if (entityJson.contains("components") && entityJson["components"].is_object()) {
        components = entityJson["components"];
    }

    static constexpr const char* kKnownComponentNames[] = {
        "EntityID", "Name", "Transform", "Sprite", "TileMap",
        "Camera", "RigidBody", "Collider", "TextComponent"
    };
    for (const char* name : kKnownComponentNames) {
        if (entityJson.contains(name)) {
            components[name] = entityJson[name];
        }
    }

    return components;
}

} // namespace engine::scene_json
