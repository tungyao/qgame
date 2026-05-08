import { tileIndex } from "./schema";
import type { EditorState, EditorTileset, TileCollision, TileCollisionShape } from "./types";

/**
 * CollisionPanelRefs caches the DOM nodes that belong to the dedicated
 * collision panel. Keeping them in one module makes the panel easier to evolve
 * without growing TilemapEditorApp into another monolith.
 */
export interface CollisionPanelRefs {
  shapeSelect: HTMLSelectElement;
  pointsInput: HTMLTextAreaElement;
}

/**
 * resolveTileCollision is passed in from the editor shell so this module does
 * not need to know how tilesets are searched or how metadata is stored.
 */
export interface CollisionOverlayResolver {
  findCollision(gid: number): TileCollision | null;
}

/**
 * Build the CollisionPanelRefs struct once from the page shell.
 */
export function createCollisionPanelRefs(root: ParentNode = document): CollisionPanelRefs {
  const shapeSelect = root.querySelector<HTMLSelectElement>("#collisionShapeSelect");
  const pointsInput = root.querySelector<HTMLTextAreaElement>("#collisionPointsInput");
  if (!shapeSelect || !pointsInput) {
    throw new Error("Collision panel elements are missing from the editor shell");
  }
  return { shapeSelect, pointsInput };
}

/**
 * Keep collision-panel event wiring local to the panel module so the editor
 * shell only needs to provide the state-mutation callback.
 */
export function bindCollisionPanel(refs: CollisionPanelRefs, onChange: () => void): void {
  refs.shapeSelect.addEventListener("change", onChange);
  refs.pointsInput.addEventListener("input", onChange);
}

/**
 * Push the current selected tile collision into the panel controls.
 */
export function syncCollisionPanel(refs: CollisionPanelRefs, collision: TileCollision | null): void {
  refs.shapeSelect.value = collision?.shape ?? "none";
  refs.pointsInput.value = collision?.points.join(", ") ?? "";
}

/**
 * Read the current collision panel controls back into a TileCollision draft.
 */
export function readCollisionDraft(
  refs: CollisionPanelRefs,
  gid: number,
  parseNumberList: (text: string) => number[]
): TileCollision {
  return {
    gid,
    shape: refs.shapeSelect.value as TileCollisionShape,
    points: parseNumberList(refs.pointsInput.value)
  };
}

/**
 * Toggle a simple blocking collision profile for quick authoring from the
 * palette. Complex shapes still go through the collision panel.
 */
export function toggleFullCollision(tileset: EditorTileset, gid: number): void {
  const existingIndex = tileset.collisions.findIndex((collision) => collision.gid === gid);
  const existing = existingIndex >= 0 ? tileset.collisions[existingIndex] : null;
  if (existing?.shape === "full") {
    tileset.collisions.splice(existingIndex, 1);
    return;
  }

  const nextCollision: TileCollision = {
    gid,
    shape: "full",
    points: []
  };

  if (existingIndex >= 0) {
    tileset.collisions[existingIndex] = nextCollision;
  } else {
    tileset.collisions.push(nextCollision);
  }
}

/**
 * Render the collision overlay for every visible, collidable layer. The module
 * only needs state, zoom, and a gid->collision resolver; everything else stays
 * in the main editor shell.
 */
export function renderCollisionOverlay(
  context: CanvasRenderingContext2D,
  state: EditorState,
  resolver: CollisionOverlayResolver
): void {
  const cellSize = state.tileSize * state.zoom;

  for (let layerIndex = 0; layerIndex < state.layers.length; layerIndex += 1) {
    const layer = state.layers[layerIndex];
    if (!layer.visible || !layer.collidable) continue;

    for (let y = 0; y < state.height; y += 1) {
      for (let x = 0; x < state.width; x += 1) {
        const gid = layer.tiles[tileIndex(state.width, x, y)];
        const collision = resolver.findCollision(gid);
        if (!collision || collision.shape === "none") continue;

        context.strokeStyle = collision.shape === "trigger"
          ? "rgba(240,180,41,0.85)"
          : "rgba(228,93,93,0.85)";
        context.fillStyle = collision.shape === "trigger"
          ? "rgba(240,180,41,0.16)"
          : "rgba(228,93,93,0.18)";

        if (collision.shape === "rect" && collision.points.length >= 4) {
          const [px, py, width, height] = collision.points;
          context.fillRect(
            x * cellSize + px * state.zoom,
            y * cellSize + py * state.zoom,
            width * state.zoom,
            height * state.zoom
          );
          context.strokeRect(
            x * cellSize + px * state.zoom,
            y * cellSize + py * state.zoom,
            width * state.zoom,
            height * state.zoom
          );
          continue;
        }

        if ((collision.shape === "polygon" || collision.shape === "oneWay") &&
            collision.points.length >= 4) {
          context.beginPath();
          for (let pointIndex = 0; pointIndex + 1 < collision.points.length; pointIndex += 2) {
            const px = x * cellSize + collision.points[pointIndex] * state.zoom;
            const py = y * cellSize + collision.points[pointIndex + 1] * state.zoom;
            if (pointIndex === 0) {
              context.moveTo(px, py);
            } else {
              context.lineTo(px, py);
            }
          }
          context.closePath();
          context.fill();
          context.stroke();
          continue;
        }

        context.fillRect(x * cellSize, y * cellSize, cellSize, cellSize);
        context.strokeRect(x * cellSize + 1, y * cellSize + 1, cellSize - 2, cellSize - 2);
      }
    }
  }
}
