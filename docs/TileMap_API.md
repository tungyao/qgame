# QGame TileMap API

This document is the editor-facing contract for the current TileMap runtime.
Keep `tools/tilemap_editor/`, scene serialization, and game-side loaders aligned
with this file and `engine::TileMap` in `src/engine/components/RenderComponents.h`.

## Runtime Component

`engine::TileMap` is an ECS component. A tilemap entity should also have a
`Transform`; the transform position is the world-space top-left corner of the
whole map.

```cpp
auto entity = api.spawnEntity();

engine::TileMap map;
map.width = 20;
map.height = 15;
map.tileSize = 32;

engine::TileMap::Tileset tileset;
tileset.firstGid = 0;
tileset.count = 16;
tileset.columns = 4;
tileset.collisions.push_back({
    .gid = 11,
    .shape = engine::TileMap::TileCollisionShape::Full
});
tileset.visuals.push_back({
    .gid = 12,
    .kind = engine::TileMap::TileVisualKind::Flipbook,
    .animation = 0
});
tileset.animations.push_back({
    .baseGid = 12,
    .frames = {
        {12, 0.12f},
        {13, 0.12f},
        {14, 0.12f}
    }
});
map.tilesets.push_back(tileset);

engine::TileMap::Layer ground;
ground.name = "ground";
ground.visible = true;
ground.collidable = true;
ground.renderLayer = 0;
ground.tiles.resize(map.width * map.height, engine::TileMap::EMPTY_GID);
ground.tiles[0] = 11;
map.layers.push_back(ground);

api.addComponent(entity, engine::Transform{0.f, 0.f});
api.addComponent(entity, map);
```

## Coordinate Rules

- `Transform.x/y` is the world-space top-left corner of the map.
- Tile `(x, y)` covers:
  - left: `Transform.x + x * tileSize`
  - top: `Transform.y + y * tileSize`
  - right: `Transform.x + (x + 1) * tileSize`
  - bottom: `Transform.y + (y + 1) * tileSize`
- TileMap rendering and collision currently ignore `Transform.rotation`,
  `Transform.scaleX`, and `Transform.scaleY`.
- `TileMap::EMPTY_GID` is `-1`; empty cells are not rendered and never collide.

## GID Rules

Each tileset owns a half-open gid range:

```text
[firstGid, firstGid + count)
```

For a placed gid:

```text
localTileId = gid - firstGid
atlasColumn = localTileId % columns
atlasRow = localTileId / columns
```

The placed gid in `Layer::tiles` is stable map data. Rendering may resolve that
gid through `TileVisual` and `TileAnimation`, but gameplay and editors should
still treat the stored gid as the canonical cell value.

## Visual Rules

Tile visuals are tileset metadata, not per-cell overrides.

- Missing visual definition means `static`.
- `flipbook` and `waterFlipbook` use `TileAnimation`.
- `wind`, `water`, `emissive`, and `autotile` currently serialize and preview
  as first-class metadata, even if a renderer backend does not yet apply every
  material effect.
- `TileVisual.phase`, `speed`, and `randomStart` only affect rendering.

## Collision Rules

Collision is split into two independent parts:

- `Tileset::collisions[]`: what physical shape a gid contributes.
- `Layer::collidable`: whether this layer participates in TileMap physics.

Runtime rules:

- Missing collision definition means `none`.
- `full` blocks with the entire cell AABB.
- `rect` uses local tile-space `[x, y, w, h]`.
- `polygon` and `oneWay` currently fall back to their authored bounds in the
  current PhysicsSystem implementation.
- `trigger` reports overlap events but does not separate rigid bodies.
- `Layer::visible` affects rendering only.

## Runtime Query Helpers

`engine::TileMap` exposes helpers so render, physics, and editor tooling do not
duplicate gid lookup rules:

```cpp
const Tileset* tilesetForGid(int gid) const;
int localTileId(int gid) const;

const TileVisual* visualForGid(int gid) const;
const TileAnimation* animationForGid(int gid) const;
const TileCollision* collisionForGid(int gid) const;

int resolveVisualGid(int gid, float timeSeconds, int x, int y, int layer) const;

bool inBounds(int x, int y) const;
size_t cellIndex(int x, int y) const;
int tileAt(int layer, int x, int y) const;

bool tileBlocks(int gid) const;
bool tileTriggers(int gid) const;
TileCollision collisionAt(int layer, int x, int y) const;
```

## Engine TileMap JSON

This is the current runtime serialization shape. Scene serialization stores this
under a `"TileMap"` component. The standalone editor also uses the same object
inside its engine package export.

```json
{
  "type": "qgame.tilemap.v2",
  "version": 2,
  "w": 20,
  "h": 15,
  "ts": 32,
  "tilesets": [
    {
      "id": "builtin",
      "name": "Built-in",
      "firstGid": 0,
      "count": 16,
      "cols": 4,
      "tex": "tiles.png",
      "assetId": "optional.asset.id",
      "sourceKind": "builtin",
      "sourceDataUrl": "",
      "visuals": [
        {
          "gid": 12,
          "kind": "flipbook",
          "animation": 0,
          "speed": 1.0,
          "strength": 0.0,
          "phase": 0.0,
          "flags": 0
        }
      ],
      "animations": [
        {
          "baseGid": 12,
          "randomStart": true,
          "speed": 1.0,
          "frames": [
            { "gid": 12, "duration": 0.12 },
            { "gid": 13, "duration": 0.12 }
          ]
        }
      ],
      "collisions": [
        { "gid": 11, "shape": "full" },
        { "gid": 15, "shape": "rect", "points": [0, 8, 16, 8] }
      ]
    }
  ],
  "layers": [
    {
      "name": "ground",
      "visible": true,
      "collidable": true,
      "renderLayer": 0,
      "tiles": [11, -1, -1]
    }
  ]
}
```

Required fields for runtime loading:

- `w`, `h`, `ts`
- `tilesets[].firstGid`
- `tilesets[].count`
- `tilesets[].cols`
- `layers[].tiles`

Optional tileset metadata:

- `id`, `name`
- `tex`, `assetId`
- `sourceKind`, `sourceDataUrl`
- `visuals`, `animations`, `collisions`

When both `assetId` and `tex` exist, runtime loading tries `assetId` first and
falls back to `tex`.

## Engine Package JSON

The editor export format wraps the raw TileMap with source image resources:

```json
{
  "type": "qgame.tilemap.engine-package",
  "version": 2,
  "tileMap": {
    "type": "qgame.tilemap.v2",
    "version": 2,
    "w": 20,
    "h": 15,
    "ts": 32,
    "tilesets": [],
    "layers": []
  },
  "resources": [
    {
      "id": "tileset-id",
      "name": "tiles.png",
      "kind": "tileset",
      "dataUrl": "data:image/png;base64,...",
      "tileSize": 32,
      "cols": 4,
      "firstGid": 16,
      "count": 64
    }
  ]
}
```

## v1 Compatibility

The reader still accepts `qgame.tilemap.v1`.

Migration rules:

- `collision[localTileId] == 0` becomes no explicit collision entry.
- `collision[localTileId] != 0` becomes `{ "gid": gid, "shape": "full" }`.
- Missing `visuals` means every tile is `static`.
- Missing `animations` means no animated tiles.

Serialization now exports v2 only.

## Editor Checklist

An editor should expose these controls explicitly:

- Map: `w`, `h`, `ts`, preview time, export schema
- Tileset: `id`, `name`, `firstGid`, `count`, `cols`, `tex`, `assetId`
- Layer: `name`, `visible`, `collidable`, `renderLayer`, `tiles[]`
- Visual: `kind`, `animation`, `speed`, `strength`, `phase`, `flags`
- Animation: frames, per-frame duration, `randomStart`, `speed`
- Collision: `shape`, `points`
- Export: include `collidable`; never infer collision from `visible`
