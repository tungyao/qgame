export type ToolMode = "brush" | "eraser";

export type TileVisualKind =
  | "static"
  | "flipbook"
  | "wind"
  | "water"
  | "waterFlipbook"
  | "emissive"
  | "autotile";

export type TileCollisionShape =
  | "none"
  | "full"
  | "rect"
  | "polygon"
  | "oneWay"
  | "trigger";

export interface TileAnimationFrame {
  gid: number;
  duration: number;
}

export interface TileAnimation {
  baseGid: number;
  frames: TileAnimationFrame[];
  randomStart: boolean;
  speed: number;
}

export interface TileVisual {
  gid: number;
  kind: TileVisualKind;
  animation: number;
  speed: number;
  strength: number;
  phase: number;
  flags: number;
}

export interface TileCollision {
  gid: number;
  shape: TileCollisionShape;
  points: number[];
}

export interface EditorTileset {
  id: string;
  name: string;
  firstGid: number;
  count: number;
  cols: number;
  tileSize: number;
  builtin: boolean;
  dataUrl: string;
  image: HTMLImageElement | null;
  assetId: string;
  texturePath: string;
  sourceKind: string;
  visuals: TileVisual[];
  animations: TileAnimation[];
  collisions: TileCollision[];
}

export interface EditorLayer {
  name: string;
  visible: boolean;
  collidable: boolean;
  renderLayer: number;
  tiles: number[];
}

export interface EditorState {
  width: number;
  height: number;
  tileSize: number;
  zoom: number;
  tool: ToolMode;
  activeLayer: number;
  selectedTile: number;
  nextGid: number;
  hiddenGids: Set<number>;
  previewTime: number;
  previewPlaying: boolean;
  showCollisionOverlay: boolean;
  tilesets: EditorTileset[];
  layers: EditorLayer[];
}

export interface EngineTileMapDocument {
  type: string;
  version: number;
  w: number;
  h: number;
  ts: number;
  tilesets: Array<{
    id?: string;
    name?: string;
    firstGid: number;
    count: number;
    cols: number;
    tex?: string;
    assetId?: string;
    sourceKind?: string;
    sourceDataUrl?: string;
    animations?: TileAnimation[];
    visuals?: TileVisual[];
    collisions?: TileCollision[];
    collision?: number[];
  }>;
  layers: Array<{
    name: string;
    visible: boolean;
    collidable: boolean;
    renderLayer: number;
    tiles: number[];
  }>;
}

export interface EnginePackageDocument {
  type: string;
  version: number;
  tileMap: EngineTileMapDocument;
  resources?: Array<{
    id?: string;
    name?: string;
    kind?: string;
    dataUrl?: string;
    tileSize?: number;
    cols?: number;
    firstGid?: number;
    count?: number;
  }>;
}

export interface EditorProjectDocument {
  type: string;
  version: number;
  width: number;
  height: number;
  tileSize: number;
  zoom: number;
  activeLayer: number;
  selectedTile: number;
  nextGid: number;
  hiddenGids: number[];
  previewTime: number;
  previewPlaying: boolean;
  showCollisionOverlay: boolean;
  tilesets: Array<{
    id: string;
    name: string;
    firstGid: number;
    count: number;
    cols: number;
    tileSize: number;
    builtin: boolean;
    dataUrl: string;
    assetId: string;
    texturePath: string;
    sourceKind: string;
    visuals: TileVisual[];
    animations: TileAnimation[];
    collisions: TileCollision[];
  }>;
  layers: EditorLayer[];
  engineTileMap: EngineTileMapDocument;
}
