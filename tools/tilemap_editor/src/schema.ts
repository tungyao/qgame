import { BUILTIN_COLORS, DEFAULT_MAP_HEIGHT, DEFAULT_MAP_WIDTH, DEFAULT_TILE_SIZE, EMPTY_GID } from "./constants";
import type {
  EditorLayer,
  EditorProjectDocument,
  EditorState,
  EditorTileset,
  EnginePackageDocument,
  EngineTileMapDocument,
  TileAnimation,
  TileCollision,
  TileVisual
} from "./types";

/**
 * Utilities in this module are intentionally DOM-free. They are shared by
 * import/export, preview rendering, and state mutation code.
 */

export function clampInt(value: unknown, min: number, max: number): number {
  const parsed = Number.parseInt(String(value ?? min), 10);
  if (!Number.isFinite(parsed)) return min;
  return Math.max(min, Math.min(max, parsed));
}

export function blankTiles(width: number, height: number): number[] {
  return Array(width * height).fill(EMPTY_GID);
}

export function tileIndex(width: number, x: number, y: number): number {
  return y * width + x;
}

export function createLayer(
  name: string,
  width: number,
  height: number,
  collidable = true,
  visible = true,
  renderLayer = 0
): EditorLayer {
  return {
    name,
    visible,
    collidable,
    renderLayer,
    tiles: blankTiles(width, height)
  };
}

export function createBuiltinTileset(): EditorTileset {
  return {
    id: "builtin",
    name: "内置色块",
    firstGid: 0,
    count: BUILTIN_COLORS.length,
    cols: 4,
    tileSize: DEFAULT_TILE_SIZE,
    builtin: true,
    dataUrl: "",
    image: null,
    assetId: "",
    texturePath: "",
    sourceKind: "builtin",
    visuals: [],
    animations: [],
    collisions: []
  };
}

export function createDefaultState(): EditorState {
  return {
    width: DEFAULT_MAP_WIDTH,
    height: DEFAULT_MAP_HEIGHT,
    tileSize: DEFAULT_TILE_SIZE,
    zoom: 2,
    tool: "brush",
    activeLayer: 0,
    selectedTile: 0,
    nextGid: BUILTIN_COLORS.length,
    hiddenGids: new Set<number>(),
    previewTime: 0,
    previewPlaying: false,
    showCollisionOverlay: true,
    tilesets: [createBuiltinTileset()],
    layers: [
      createLayer("Ground", DEFAULT_MAP_WIDTH, DEFAULT_MAP_HEIGHT, false, true, 0),
      createLayer("Objects", DEFAULT_MAP_WIDTH, DEFAULT_MAP_HEIGHT, true, true, 10)
    ]
  };
}

export function computeNextGid(tilesets: EditorTileset[]): number {
  return tilesets.reduce((next, tileset) => Math.max(next, tileset.firstGid + tileset.count), BUILTIN_COLORS.length);
}

export function findTilesetForGid(tilesets: EditorTileset[], gid: number): EditorTileset | null {
  return tilesets.find((tileset) => gid >= tileset.firstGid && gid < tileset.firstGid + tileset.count) ?? null;
}

export function localTileId(tileset: EditorTileset, gid: number): number {
  return gid - tileset.firstGid;
}

export function getTileVisual(tileset: EditorTileset, gid: number): TileVisual | null {
  return tileset.visuals.find((visual) => visual.gid === gid) ?? null;
}

export function getTileAnimation(tileset: EditorTileset, gid: number): TileAnimation | null {
  const visual = getTileVisual(tileset, gid);
  if (visual && visual.animation >= 0 && visual.animation < tileset.animations.length) {
    return tileset.animations[visual.animation] ?? null;
  }
  return tileset.animations.find((animation) => animation.baseGid === gid) ?? null;
}

export function getTileCollision(tileset: EditorTileset, gid: number): TileCollision | null {
  return tileset.collisions.find((collision) => collision.gid === gid) ?? null;
}

/**
 * The editor mirrors the engine's v2 frame-resolution rules closely enough to
 * preview flipbook timing and randomStart behavior without calling into C++.
 */
export function resolvePreviewGid(
  tileset: EditorTileset,
  gid: number,
  timeSeconds: number,
  cellX: number,
  cellY: number,
  layerIndex: number
): number {
  const visual = getTileVisual(tileset, gid);
  if (!visual) return gid;
  if (visual.kind !== "flipbook" && visual.kind !== "waterFlipbook") return gid;

  const animation = getTileAnimation(tileset, gid);
  if (!animation || animation.frames.length === 0) return gid;

  const frameDurations = animation.frames.map((frame) => Math.max(frame.duration, 0.0001));
  const totalDuration = frameDurations.reduce((sum, duration) => sum + duration, 0);
  if (totalDuration <= 0) return gid;

  let phaseSeconds = visual.phase;
  if (animation.randomStart) {
    let hash = 2166136261;
    for (const value of [gid, cellX, cellY, layerIndex]) {
      hash ^= value >>> 0;
      hash = Math.imul(hash, 16777619);
    }
    phaseSeconds += ((hash >>> 0) & 0x00ffffff) / 0x01000000 * totalDuration;
  }

  const playbackSpeed = Math.max(visual.speed || 1, 0.0001) * Math.max(animation.speed || 1, 0.0001);
  let localTime = (timeSeconds * playbackSpeed + phaseSeconds) % totalDuration;
  if (localTime < 0) localTime += totalDuration;

  for (let index = 0; index < animation.frames.length; index += 1) {
    localTime -= frameDurations[index];
    if (localTime < 0) {
      return animation.frames[index].gid ?? gid;
    }
  }

  return animation.frames[animation.frames.length - 1]?.gid ?? gid;
}

export function toEngineTileMap(state: EditorState): EngineTileMapDocument {
  return {
    type: "qgame.tilemap.v2",
    version: 2,
    w: state.width,
    h: state.height,
    ts: state.tileSize,
    tilesets: state.tilesets.map((tileset) => ({
      id: tileset.id,
      name: tileset.name,
      firstGid: tileset.firstGid,
      count: tileset.count,
      cols: tileset.cols,
      tex: tileset.texturePath || tileset.name,
      assetId: tileset.assetId || undefined,
      sourceKind: tileset.sourceKind || (tileset.builtin ? "builtin" : "image"),
      sourceDataUrl: tileset.dataUrl || undefined,
      animations: tileset.animations.map((animation) => ({
        baseGid: animation.baseGid,
        randomStart: !!animation.randomStart,
        speed: animation.speed,
        frames: animation.frames.map((frame) => ({
          gid: frame.gid,
          duration: frame.duration
        }))
      })),
      visuals: tileset.visuals.map((visual) => ({
        gid: visual.gid,
        kind: visual.kind,
        animation: visual.animation,
        speed: visual.speed,
        strength: visual.strength,
        phase: visual.phase,
        flags: visual.flags
      })),
      collisions: tileset.collisions.map((collision) => ({
        gid: collision.gid,
        shape: collision.shape,
        points: [...collision.points]
      }))
    })),
    layers: state.layers.map((layer) => ({
      name: layer.name,
      visible: !!layer.visible,
      collidable: !!layer.collidable,
      renderLayer: layer.renderLayer,
      tiles: [...layer.tiles]
    }))
  };
}

export function toEnginePackage(state: EditorState): EnginePackageDocument {
  return {
    type: "qgame.tilemap.engine-package",
    version: 2,
    tileMap: toEngineTileMap(state),
    resources: state.tilesets
      .filter((tileset) => !tileset.builtin)
      .map((tileset) => ({
        id: tileset.id,
        name: tileset.name,
        kind: "tileset",
        dataUrl: tileset.dataUrl,
        tileSize: tileset.tileSize,
        cols: tileset.cols,
        firstGid: tileset.firstGid,
        count: tileset.count
      }))
  };
}

export function toEditorProject(state: EditorState): EditorProjectDocument {
  return {
    type: "qgame.tilemap.editor",
    version: 3,
    width: state.width,
    height: state.height,
    tileSize: state.tileSize,
    zoom: state.zoom,
    activeLayer: state.activeLayer,
    selectedTile: state.selectedTile,
    nextGid: state.nextGid,
    hiddenGids: Array.from(state.hiddenGids),
    previewTime: state.previewTime,
    previewPlaying: state.previewPlaying,
    showCollisionOverlay: state.showCollisionOverlay,
    tilesets: state.tilesets.map((tileset) => ({
      id: tileset.id,
      name: tileset.name,
      firstGid: tileset.firstGid,
      count: tileset.count,
      cols: tileset.cols,
      tileSize: tileset.tileSize,
      builtin: tileset.builtin,
      dataUrl: tileset.dataUrl,
      assetId: tileset.assetId,
      texturePath: tileset.texturePath,
      sourceKind: tileset.sourceKind,
      visuals: tileset.visuals.map((visual) => ({ ...visual })),
      animations: tileset.animations.map((animation) => ({
        baseGid: animation.baseGid,
        randomStart: animation.randomStart,
        speed: animation.speed,
        frames: animation.frames.map((frame) => ({ ...frame }))
      })),
      collisions: tileset.collisions.map((collision) => ({
        gid: collision.gid,
        shape: collision.shape,
        points: [...collision.points]
      }))
    })),
    layers: state.layers.map((layer) => ({
      name: layer.name,
      visible: layer.visible,
      collidable: layer.collidable,
      renderLayer: layer.renderLayer,
      tiles: [...layer.tiles]
    })),
    engineTileMap: toEngineTileMap(state)
  };
}
