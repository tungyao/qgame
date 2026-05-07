# QGame TileMap API

This document is the editor-facing contract for the current TileMap runtime.
Keep `tools/tilemap_editor.html`, scene serialization, and game-side loaders aligned
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
tileset.texture = textureHandle;
tileset.firstGid = 0;
tileset.count = 16;
tileset.columns = 4;
tileset.collision = {
    0, 0, 0, 0,
    0, 0, 0, 0,
    0, 0, 0, 1,
    0, 0, 0, 0
};
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

- `Transform.x/y` is the top-left corner of the map.
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

The atlas source rectangle is:

```text
x = atlasColumn * tileSize
y = atlasRow * tileSize
w = tileSize
h = tileSize
```

## Collision Rules

Collision is split into two independent parts:

- `Tileset::collision[localTileId]`: whether this tile kind is solid.
- `Layer::collidable`: whether this layer participates in TileMap physics.

A cell is solid if any `collidable` layer contains a gid whose tileset collision
entry is nonzero.

`Layer::visible` affects rendering only. Hidden collision layers are valid.

## Rendering Rules

- `Layer::visible=false`: layer is skipped by RenderSystem.
- `Layer::renderLayer`: participates in the same layer ordering as
  `Sprite::layer`.
- GPU-driven rendering caches TileMap tile instances and only rebuilds the cache
  when map data, map transform, tileset handles, layer visibility, or layer
  render/collision settings change.

Text is still rendered through its MSDF path and is not part of the TileMap GPU
cache.

## Engine TileMap JSON

This is the raw TileMap component shape. Scene serialization stores this under a
`"TileMap"` component. The standalone editor also places the same object at
`enginePackage.tileMap`.

```json
{
  "type": "qgame.tilemap.v1",
  "version": 1,
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
      "collision": [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0]
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
- `tilesets[].collision`
- `layers[].tiles`

Optional editor/package fields:

- `type`
- `version`
- `tilesets[].id`
- `tilesets[].name`
- `tilesets[].sourceKind`
- `tilesets[].sourceDataUrl`

Optional runtime asset fields:

- `tilesets[].assetId`
- `tilesets[].tex`

When both `assetId` and `tex` exist, runtime loading tries `assetId` first and
falls back to `tex`.

## Engine Package JSON

The editor export format wraps the raw TileMap with source image resources:

```json
{
  "type": "qgame.tilemap.engine-package",
  "version": 1,
  "tileMap": {
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

`resources` is for editor/demo import convenience. The runtime component only
needs the `tileMap` object plus valid `TextureHandle`s in each tileset.

## Editor Checklist

An editor should expose these controls explicitly:

- Map: `w`, `h`, `ts`
- Tileset: `firstGid`, `count`, `cols`, `collision[]`
- Layer: `name`, `visible`, `collidable`, `renderLayer`, `tiles[]`
- Cell painting: write `TileMap::EMPTY_GID` for erased cells.
- Export: include `collidable`; do not infer collision from `visible`.

