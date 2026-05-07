# QGame TileMap Animation And Collision v2 Plan

This document defines the complete target architecture for TileMap animation,
visual materials, collision profiles, GPU rendering, and editor support.

The current runtime contract remains `qgame.tilemap.v1` and is documented in
`docs/TileMap_API.md`. This plan describes the intended `qgame.tilemap.v2`
contract and the engine changes needed to support animated lakes, moving tree
leaves, accurate collision shapes, and editor-facing authoring tools.

## Goals

- Make TileMap animation a first-class engine feature, not a temporary render
  workaround.
- Keep placed map data stable: layer cells store a stable `gid`, not a
  per-frame visual frame.
- Store animation, material, and collision data in tileset metadata so many
  cells can share the same definitions.
- Keep collision independent from visual animation.
- Avoid rebuilding the full TileMap GPU cache every frame.
- Expose every editor-facing parameter explicitly through JSON and C++ APIs.
- Keep `qgame.tilemap.v1` import compatibility while exporting v2 once the
  implementation is complete.

## Non-Goals

- Visual animation must not implicitly change physics collision.
- TileMap animation should not be implemented by mutating `Layer::tiles` every
  frame.
- Tree wind and water shimmer should not require every tile to have a large
  flipbook sequence if a shader material can express the effect.
- The editor should not infer collision from visibility, animation frames, or
  image opacity.

## Core Model

TileMap v2 separates four responsibilities:

- `Layer`: placed grid data, visibility, collision participation, and render
  layer.
- `Tileset`: atlas texture, gid range, visual metadata, animation metadata, and
  collision metadata.
- `TileVisual`: how a placed gid is drawn.
- `TileCollision`: how a placed gid contributes to physics.

The placed cell value is still a stable global gid. Rendering resolves that gid
through `TileVisual`; physics resolves the same gid through `TileCollision`.

## Runtime C++ Shape

The following structures should live under `engine::TileMap` or a closely owned
TileMap header. The exact file split can be decided during implementation, but
the API should stay editor-facing and serializable.

```cpp
struct TileAnimationFrame {
    int gid = EMPTY_GID;      // Global gid displayed by this frame.
    float duration = 0.1f;    // Seconds. Must be greater than zero.
};

struct TileAnimation {
    int baseGid = EMPTY_GID;  // Gid stored in Layer::tiles.
    std::vector<TileAnimationFrame> frames;
    bool randomStart = false; // Desynchronizes repeated water/grass tiles.
    float speed = 1.0f;       // Playback multiplier.
};

enum class TileVisualKind : uint8_t {
    Static,
    Flipbook,
    Wind,
    Water,
    WaterFlipbook,
    Emissive,
    Autotile
};

struct TileVisual {
    int gid = EMPTY_GID;                 // Gid this visual definition applies to.
    TileVisualKind kind = TileVisualKind::Static;
    int animation = -1;                  // Index into Tileset::animations.
    float speed = 1.0f;                  // Material speed multiplier.
    float strength = 0.0f;               // Wind/water/emissive strength.
    float phase = 0.0f;                  // Author-controlled phase offset.
    uint32_t flags = 0;                  // Reserved for renderer/editor flags.
};

enum class TileCollisionShape : uint8_t {
    None,
    Full,
    Rect,
    Polygon,
    OneWay,
    Trigger
};

struct TileCollision {
    int gid = EMPTY_GID;                 // Gid this collision definition applies to.
    TileCollisionShape shape = TileCollisionShape::None;
    std::vector<float> points;           // Rect: x,y,w,h. Polygon: x0,y0,x1,y1...
};
```

`Tileset` should become:

```cpp
struct Tileset {
    TextureHandle texture;
    int firstGid = 0;
    int count = 0;
    int columns = 1;

    std::vector<TileAnimation> animations;
    std::vector<TileVisual> visuals;
    std::vector<TileCollision> collisions;

    // Backward-compatibility only. v1 import can populate collisions from this.
    std::vector<uint8_t> legacyCollision;
};
```

## Runtime API

TileMap should expose explicit query helpers so RenderSystem, PhysicsSystem,
and editor tools do not duplicate gid lookup rules.

```cpp
const Tileset* tilesetForGid(int gid) const;
int localTileId(int gid) const;

const TileVisual* visualForGid(int gid) const;
const TileAnimation* animationForGid(int gid) const;
const TileCollision* collisionForGid(int gid) const;

bool inBounds(int x, int y) const;
size_t cellIndex(int x, int y) const;
int tileAt(int layer, int x, int y) const;

bool tileBlocks(int gid) const;
bool tileTriggers(int gid) const;
TileCollision collisionAt(int layer, int x, int y) const;
```

Default behavior must be deterministic:

- Missing visual means `Static`.
- Missing collision means `None`.
- v1 `collision[localTileId] != 0` imports as `TileCollisionShape::Full`.
- `Layer::collidable=false` disables all collision from that layer.
- `Layer::visible=false` disables rendering only.

## TileMap v2 JSON

`qgame.tilemap.v2` is the target editor/runtime schema.

```json
{
  "type": "qgame.tilemap.v2",
  "version": 2,
  "w": 64,
  "h": 64,
  "ts": 16,
  "tilesets": [
    {
      "id": "nature",
      "name": "Nature",
      "firstGid": 0,
      "count": 256,
      "cols": 16,
      "tex": "assets/tiles/nature.png",
      "assetId": "optional.texture.asset.id",
      "visuals": [
        {
          "gid": 20,
          "kind": "waterFlipbook",
          "animation": 0,
          "speed": 1.0,
          "strength": 0.15,
          "phase": 0.0,
          "flags": 0
        },
        {
          "gid": 80,
          "kind": "wind",
          "speed": 0.8,
          "strength": 1.4,
          "phase": 0.3,
          "flags": 0
        }
      ],
      "animations": [
        {
          "baseGid": 20,
          "randomStart": true,
          "speed": 1.0,
          "frames": [
            { "gid": 20, "duration": 0.12 },
            { "gid": 21, "duration": 0.12 },
            { "gid": 22, "duration": 0.12 },
            { "gid": 23, "duration": 0.12 }
          ]
        }
      ],
      "collisions": [
        { "gid": 4, "shape": "full" },
        { "gid": 5, "shape": "rect", "points": [0, 8, 16, 8] },
        { "gid": 6, "shape": "none" },
        { "gid": 7, "shape": "trigger", "points": [0, 0, 16, 16] }
      ]
    }
  ],
  "layers": [
    {
      "name": "Ground",
      "visible": true,
      "collidable": false,
      "renderLayer": 0,
      "tiles": []
    },
    {
      "name": "Objects",
      "visible": true,
      "collidable": true,
      "renderLayer": 10,
      "tiles": []
    }
  ]
}
```

## Visual Kinds

`Static`

- Uses the gid's atlas UV directly.
- No animation table lookup.

`Flipbook`

- Uses `TileAnimation` frames.
- Good for fire, torches, simple water, sparkle effects, and item pickups.

`Wind`

- Uses shader time, `speed`, `strength`, `phase`, and a stable per-cell random
  seed.
- Good for leaves, grass, hanging vines, and cloth-like decorations.
- Should usually move only vertices or UVs visually, not collision.

`Water`

- Uses shader UV shimmer, brightness modulation, and optional noise.
- Good for lakes, puddles, shallow water, and reflective surfaces.

`WaterFlipbook`

- Combines frame animation with water shimmer.
- Good for lakes with hand-authored frame variation plus procedural sparkle.

`Emissive`

- Marks tiles that should contribute to lighting or bloom once the renderer has
  a matching pass.

`Autotile`

- Reserved for terrain rules that choose a visual variant based on neighbors.
- Autotile output should resolve to stable visual gids before GPU instance
  upload or through a dedicated variant table.

## GPU Rendering Architecture

The current GPU-driven path caches TileMap instances and avoids rebuilding them
unless map data changes. TileMap v2 should preserve that property.

The renderer should introduce a tile-specific instance buffer instead of
encoding all tile behavior into the existing sprite instance flags.

```cpp
struct GPUTileInstance {
    float transform[12];
    float baseUV[4];
    float color[4];

    uint32_t textureIndex;
    uint32_t visualKind;
    uint32_t animationIndex;
    uint32_t flags;

    float speed;
    float strength;
    float phase;
    float randomSeed;
};

struct GPUTileAnimation {
    uint32_t firstFrame;
    uint32_t frameCount;
    float totalDuration;
    float speed;
};

struct GPUTileAnimationFrame {
    float uv[4];
    float startTime;
    float duration;
};
```

Render flow:

1. Rebuild static tile instances only when map structure, layer data, tileset
   data, transform, or texture handles change.
2. Upload animation tables when tileset animation metadata changes.
3. Upload global frame time once per frame.
4. Shader resolves the visual frame using `animationIndex`, `time`, `phase`,
   and `randomSeed`.
5. Shader applies water or wind material offsets using `visualKind`.
6. CPU still performs camera culling, sorting, and batching using stable cached
   metadata.

This avoids full TileMap cache rebuilds for animated lakes and wind-driven
trees.

## Collision Architecture

Physics must read collision profiles, not current visual frames.

Rules:

- `Layer::collidable=false` disables collision for the entire layer.
- `TileCollisionShape::None` has no blocking volume.
- `Full` uses the full cell AABB.
- `Rect` uses local tile-space rectangle points `[x, y, w, h]`.
- `Polygon` uses local tile-space polygon points.
- `OneWay` blocks only along the configured direction once direction metadata is
  added.
- `Trigger` reports overlap but does not block movement.
- Visual animation never changes collision by itself.

This fixes two common bugs:

- A visible tile can animate without shifting the physics body upward or
  downward.
- A non-collision tile cannot accidentally get collision just because the
  collision array has an implicit or stale value.

If gameplay needs dynamic collision, such as a bridge opening or a door closing,
that state should replace or override the collision profile explicitly. It
should not be inferred from the current animation frame.

## Editor Requirements

The TileMap editor should expose these panels and controls explicitly.

Map:

- `w`, `h`, `ts`
- Export schema version.
- Import compatibility mode for v1.

Tileset:

- `id`, `name`, `firstGid`, `count`, `cols`, `tex`, `assetId`
- Tile atlas preview.
- Per-tile metadata indicator badges for animation, wind/water material, and
  collision.

Layer:

- `name`
- `visible`
- `collidable`
- `renderLayer`
- Cell painting and erase using `TileMap::EMPTY_GID`.

Visual inspector:

- Selected gid.
- `kind`: `static`, `flipbook`, `wind`, `water`, `waterFlipbook`, `emissive`,
  `autotile`.
- `speed`
- `strength`
- `phase`
- `flags`

Animation timeline:

- Add/remove/reorder frames.
- Pick frame gid from the same tileset.
- Edit per-frame duration.
- Preview playback.
- Toggle `randomStart`.
- Show total duration.

Collision inspector:

- Shape: `none`, `full`, `rect`, `polygon`, `oneWay`, `trigger`.
- Rect editor in local tile coordinates.
- Polygon point editor.
- Collision overlay on the map canvas.
- Collision must be edited independently from layer visibility and animation.

Preview:

- Play/pause animation.
- Scrub time.
- Toggle collision overlay.
- Toggle material simulation.
- Show warnings for invalid animation frames, invalid collision points, and gids
  outside the tileset range.

## Import And Migration

v1 import should map fields as follows:

- `collision[localTileId] == 0` becomes no explicit collision entry.
- `collision[localTileId] != 0` becomes `{ "gid": gid, "shape": "full" }`.
- Missing `visuals` means every tile is `static`.
- Missing `animations` means no animated tiles.
- Existing layers, gids, visibility, collidability, and render layers are
  preserved.

After v2 is implemented, serialization should export v2 only. The reader should
continue accepting v1 so old maps and existing demo assets keep working.

## Implementation Order

1. Add v2 TileMap C++ structures and query helpers.
2. Add v2 JSON read/write in `ComponentJson.cpp`.
3. Keep v1 reader and convert v1 collision arrays into v2 collision profiles.
4. Update `docs/TileMap_API.md` after v2 becomes the current runtime contract.
5. Update `PhysicsSystem` to use `TileCollision` profiles.
6. Add full and rect collision support first, then polygon/trigger/one-way.
7. Add tile-specific GPU instance and animation table buffers.
8. Add tile shader support for `Static`, `Flipbook`, `Wind`, `Water`, and
   `WaterFlipbook`.
9. Update RenderSystem cache signatures so static data rebuilds only when
   structural data changes.
10. Update the TileMap editor for visual, animation, and collision inspectors.
11. Add map fixtures for water animation, wind animation, rect collision, and
   non-collision animated tiles.
12. Add regression tests for v1 migration, collision profile lookup, animation
   table generation, and cache stability.

## Validation Checklist

- Animated water changes visually while TileMap cache signature remains stable
  across frames.
- Wind tiles move visually without changing physics collision.
- Non-collision animated tiles never block the player.
- Rect collision aligns with the visible tile area in world coordinates.
- Hidden collision layers still collide when `collidable=true`.
- Visible decorative layers do not collide when `collidable=false`.
- v1 maps load correctly and produce the same solid tile behavior as before.
- v2 maps round-trip through scene serialization without losing metadata.
- The editor can author and export every field in the v2 schema.
