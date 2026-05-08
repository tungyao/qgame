import { BUILTIN_COLORS, DEFAULT_TILE_SIZE, EMPTY_GID } from "./constants";
import {
  bindCollisionPanel,
  createCollisionPanelRefs,
  readCollisionDraft,
  renderCollisionOverlay,
  syncCollisionPanel,
  toggleFullCollision as toggleFullCollisionInPanel
} from "./collision_panel";
import { renderLayerPanel } from "./layer_panel";
import {
  blankTiles,
  clampInt,
  computeNextGid,
  createBuiltinTileset,
  createDefaultState,
  createLayer,
  findTilesetForGid,
  getTileAnimation,
  getTileCollision,
  getTileVisual,
  localTileId,
  resolvePreviewGid,
  tileIndex,
  toEditorProject,
  toEnginePackage,
  toEngineTileMap
} from "./schema";
import type {
  EditorLayer,
  EditorProjectDocument,
  EditorState,
  EditorTileset,
  EnginePackageDocument,
  EngineTileMapDocument,
  TileAnimation,
  TileCollision,
  TileVisual,
  TileVisualKind
} from "./types";

/**
 * TilemapEditorApp owns DOM wiring and editor state mutations.
 *
 * Source-of-truth rules:
 * - state.tilesets[*].visuals / animations / collisions are the canonical v2 metadata.
 * - engine/package/project JSON is always derived from current state, never edited in place.
 * - import paths normalize v1, v2, engine-package, and editor-project shapes into state.
 */
class TilemapEditorApp {
  constructor() {
    this.canvas = this.requireElement<HTMLCanvasElement>("mapCanvas");
    const context = this.canvas.getContext("2d");
    if (!context) {
      throw new Error("2D canvas context not available");
    }
    this.context = context;
    this.paletteElement = this.requireElement<HTMLDivElement>("palette");
    this.tilesetsElement = this.requireElement<HTMLDivElement>("tilesets");
    this.layersElement = this.requireElement<HTMLDivElement>("layers");
    this.animationFramesElement = this.requireElement<HTMLDivElement>("animationFrames");
    this.jsonBox = this.requireElement<HTMLTextAreaElement>("jsonBox");
    this.statusElement = this.requireElement<HTMLDivElement>("status");
    this.loadFileInput = this.requireElement<HTMLInputElement>("loadFile");
    this.selectionSummaryElement = this.requireElement<HTMLDivElement>("selectionSummary");
    this.timeOutputElement = this.requireElement<HTMLOutputElement>("timeOutput");
    this.collisionPanelRefs = createCollisionPanelRefs(document);
    this.state = createDefaultState();
    this.layerEditingIndex = -1;
    this.layerEditingDraft = "";
    this.lastAnimationTick = performance.now();
    this.painting = false;
    this.context.imageSmoothingEnabled = false;
    this.bindEvents();
    this.syncInputs();
    this.renderAll();
    requestAnimationFrame((timestamp) => this.onAnimationFrame(timestamp));
  }

  /**
   * DOM lookup is centralized so the constructor fails fast if the HTML shell
   * and TS code drift out of sync.
   */
  private requireElement<T extends HTMLElement>(id: string): T {
    const element = document.getElementById(id);
    if (!element) {
      throw new Error(`Missing required element #${id}`);
    }
    return element as T;
  }

  /**
   * Generated ids should not rely on crypto.randomUUID existing. Some embedded
   * WebView builds are missing it, and that would otherwise break tileset import.
   */
  private makeId(): string {
    const randomId = globalThis.crypto?.randomUUID?.();
    if (randomId) return randomId;
    return `tilemap-${Date.now()}-${Math.floor(Math.random() * 1_000_000)}`;
  }

  private onAnimationFrame(timestamp: number): void {
    const deltaSeconds = Math.max(0, (timestamp - this.lastAnimationTick) / 1000);
    this.lastAnimationTick = timestamp;
    if (this.state.previewPlaying) {
      this.state.previewTime += deltaSeconds;
      this.syncPreviewInputs();
      this.renderMap();
    }
    requestAnimationFrame((nextTimestamp) => this.onAnimationFrame(nextTimestamp));
  }

  private bindEvents(): void {
    this.requireElement<HTMLButtonElement>("newBtn").addEventListener("click", () => this.onNewMap());
    this.requireElement<HTMLButtonElement>("brushBtn").addEventListener("click", () => this.setTool("brush"));
    this.requireElement<HTMLButtonElement>("eraserBtn").addEventListener("click", () => this.setTool("eraser"));
    this.requireElement<HTMLButtonElement>("playBtn").addEventListener("click", () => this.togglePreviewPlayback());
    this.requireElement<HTMLButtonElement>("overlayBtn").addEventListener("click", () => this.toggleCollisionOverlay());
    this.requireElement<HTMLButtonElement>("saveBtn").addEventListener("click", () => this.downloadJson("tilemap_project.json", toEditorProject(this.state)));
    this.requireElement<HTMLButtonElement>("loadBtn").addEventListener("click", () => this.loadFileInput.click());
    this.requireElement<HTMLButtonElement>("exportBtn").addEventListener("click", () => this.downloadJson("tilemap_engine_package.json", toEnginePackage(this.state)));
    this.requireElement<HTMLButtonElement>("resizeBtn").addEventListener("click", () => this.onResizeMap());
    this.requireElement<HTMLButtonElement>("clearLayerBtn").addEventListener("click", () => this.onClearActiveLayer());
    this.requireElement<HTMLButtonElement>("addLayerBtn").addEventListener("click", () => this.onAddLayer());
    this.requireElement<HTMLButtonElement>("copyJsonBtn").addEventListener("click", () => void this.copyJsonToClipboard());
    this.requireElement<HTMLButtonElement>("importJsonBtn").addEventListener("click", () => void this.importFromJsonTextbox());
    this.requireElement<HTMLButtonElement>("addFrameBtn").addEventListener("click", () => this.onAddAnimationFrame());
    this.requireElement<HTMLButtonElement>("removeAnimationBtn").addEventListener("click", () => this.onRemoveAnimation());

    this.requireElement<HTMLInputElement>("mapWInput").addEventListener("change", () => this.syncMapDimensionsFromInputs());
    this.requireElement<HTMLInputElement>("mapHInput").addEventListener("change", () => this.syncMapDimensionsFromInputs());
    this.requireElement<HTMLInputElement>("tileSizeInput").addEventListener("change", () => this.onTileSizeChanged());
    this.requireElement<HTMLInputElement>("tilesetColsInput").addEventListener("change", () => this.updateJsonOnly());
    this.requireElement<HTMLSelectElement>("zoomInput").addEventListener("change", () => this.onZoomChanged());
    this.requireElement<HTMLInputElement>("tilesetFile").addEventListener("change", (event) => void this.onImportTilesets(event));
    this.requireElement<HTMLInputElement>("timeInput").addEventListener("input", () => this.onPreviewTimeChanged());
    this.loadFileInput.addEventListener("change", (event) => void this.onLoadFilePicked(event));

    this.requireElement<HTMLSelectElement>("visualKindSelect").addEventListener("change", () => this.onVisualChanged());
    this.requireElement<HTMLInputElement>("visualSpeedInput").addEventListener("change", () => this.onVisualChanged());
    this.requireElement<HTMLInputElement>("visualStrengthInput").addEventListener("change", () => this.onVisualChanged());
    this.requireElement<HTMLInputElement>("visualPhaseInput").addEventListener("change", () => this.onVisualChanged());
    this.requireElement<HTMLInputElement>("visualFlagsInput").addEventListener("change", () => this.onVisualChanged());
    this.requireElement<HTMLInputElement>("animationSpeedInput").addEventListener("change", () => this.onAnimationChanged());
    this.requireElement<HTMLInputElement>("animationRandomStartInput").addEventListener("change", () => this.onAnimationChanged());
    bindCollisionPanel(this.collisionPanelRefs, () => this.onCollisionChanged());

    this.canvas.addEventListener("pointerdown", (event) => {
      this.painting = true;
      this.canvas.setPointerCapture(event.pointerId);
      this.paintFromEvent(event);
    });
    this.canvas.addEventListener("pointermove", (event) => {
      if (this.painting) {
        this.paintFromEvent(event);
      }
    });
    this.canvas.addEventListener("pointerup", () => { this.painting = false; });
    this.canvas.addEventListener("pointercancel", () => { this.painting = false; });
  }

  private onNewMap(): void {
    this.state = createDefaultState();
    this.cancelLayerRename();
    this.state.width = clampInt(this.requireElement<HTMLInputElement>("mapWInput").value, 1, 512);
    this.state.height = clampInt(this.requireElement<HTMLInputElement>("mapHInput").value, 1, 512);
    this.state.tileSize = clampInt(this.requireElement<HTMLInputElement>("tileSizeInput").value, 4, 256);
    this.state.zoom = clampInt(this.requireElement<HTMLSelectElement>("zoomInput").value, 1, 4);
    this.state.layers = [
      createLayer("Ground", this.state.width, this.state.height, false, true, 0),
      createLayer("Objects", this.state.width, this.state.height, true, true, 10)
    ];
    this.syncInputs();
    this.renderAll();
    this.setStatus("新建地图完成");
  }

  private onResizeMap(): void {
    const newWidth = clampInt(this.requireElement<HTMLInputElement>("mapWInput").value, 1, 512);
    const newHeight = clampInt(this.requireElement<HTMLInputElement>("mapHInput").value, 1, 512);
    const oldWidth = this.state.width;
    const oldHeight = this.state.height;
    const previousLayers = this.state.layers.map((layer) => [...layer.tiles]);
    this.state.width = newWidth;
    this.state.height = newHeight;
    this.state.layers.forEach((layer, layerIndex) => {
      layer.tiles = blankTiles(newWidth, newHeight);
      for (let y = 0; y < Math.min(oldHeight, newHeight); y += 1) {
        for (let x = 0; x < Math.min(oldWidth, newWidth); x += 1) {
          layer.tiles[tileIndex(newWidth, x, y)] = previousLayers[layerIndex][tileIndex(oldWidth, x, y)];
        }
      }
    });
    this.renderAll();
    this.setStatus("地图尺寸已应用");
  }

  private onClearActiveLayer(): void {
    this.activeLayer().tiles.fill(EMPTY_GID);
    this.renderMap();
    this.setStatus(`已清空 ${this.activeLayer().name}`);
  }

  private onAddLayer(): void {
    this.commitLayerRename();
    this.state.layers.push(
      createLayer(
        `Layer ${this.state.layers.length + 1}`,
        this.state.width,
        this.state.height,
        false,
        true,
        this.state.layers.length * 10
      )
    );
    this.state.activeLayer = this.state.layers.length - 1;
    this.renderAll();
    this.setStatus("已添加图层");
  }

  private onZoomChanged(): void {
    this.state.zoom = clampInt(this.requireElement<HTMLSelectElement>("zoomInput").value, 1, 4);
    this.renderMap();
  }

  private onTileSizeChanged(): void {
    this.state.tileSize = clampInt(this.requireElement<HTMLInputElement>("tileSizeInput").value, 4, 256);
    this.renderAll();
  }

  private onPreviewTimeChanged(): void {
    const slider = this.requireElement<HTMLInputElement>("timeInput");
    this.state.previewTime = Number.parseFloat(slider.value) || 0;
    this.syncPreviewInputs();
    this.renderMap();
  }

  private async onImportTilesets(event: Event): Promise<void> {
    const input = event.currentTarget as HTMLInputElement;
    const files = Array.from(input.files ?? []);
    for (const file of files) {
      const dataUrl = await this.readFileAsDataUrl(file);
      const image = await this.loadImage(dataUrl);
      const cols = Math.max(1, Math.floor(image.width / this.state.tileSize));
      const rows = Math.max(1, Math.floor(image.height / this.state.tileSize));
      const count = cols * rows;
      this.state.tilesets.push({
        id: this.makeId(),
        name: file.name,
        firstGid: this.state.nextGid,
        count,
        cols,
        tileSize: this.state.tileSize,
        builtin: false,
        dataUrl,
        image,
        assetId: "",
        texturePath: file.name,
        sourceKind: "image",
        visuals: [],
        animations: [],
        collisions: []
      });
      this.state.nextGid += count;
    }
    input.value = "";
    this.syncInputs();
    this.renderAll();
    this.setStatus(`已导入 ${files.length} 个 tileset`);
  }

  private async onLoadFilePicked(event: Event): Promise<void> {
    const file = (event.currentTarget as HTMLInputElement).files?.[0];
    if (!file) return;
    const text = await file.text();
    const payload = JSON.parse(text) as unknown;
    await this.loadDocument(payload);
    (event.currentTarget as HTMLInputElement).value = "";
    this.setStatus(`已读取 ${file.name}`);
  }

  private onVisualChanged(): void {
    const tileset = this.selectedTileset();
    const gid = this.state.selectedTile;
    if (!tileset || gid === EMPTY_GID) return;

    const kind = this.requireElement<HTMLSelectElement>("visualKindSelect").value as TileVisualKind;
    const speed = Number.parseFloat(this.requireElement<HTMLInputElement>("visualSpeedInput").value) || 1;
    const strength = Number.parseFloat(this.requireElement<HTMLInputElement>("visualStrengthInput").value) || 0;
    const phase = Number.parseFloat(this.requireElement<HTMLInputElement>("visualPhaseInput").value) || 0;
    const flags = clampInt(this.requireElement<HTMLInputElement>("visualFlagsInput").value, 0, 0xffffffff);
    const animation = tileset.animations.findIndex((entry) => entry.baseGid === gid);

    this.upsertVisual(tileset, {
      gid,
      kind,
      animation,
      speed,
      strength,
      phase,
      flags
    });
    this.renderAll();
  }

  private onAnimationChanged(): void {
    const tileset = this.selectedTileset();
    const gid = this.state.selectedTile;
    if (!tileset || gid === EMPTY_GID) return;

    const animation = this.ensureAnimation(tileset, gid);
    animation.speed = Number.parseFloat(this.requireElement<HTMLInputElement>("animationSpeedInput").value) || 1;
    animation.randomStart = this.requireElement<HTMLInputElement>("animationRandomStartInput").checked;
    this.rebindVisualAnimationIndex(tileset, gid);
    this.renderAll();
  }

  private onAddAnimationFrame(): void {
    const tileset = this.selectedTileset();
    const gid = this.state.selectedTile;
    if (!tileset || gid === EMPTY_GID) return;
    const animation = this.ensureAnimation(tileset, gid);
    animation.frames.push({ gid, duration: 0.12 });
    this.rebindVisualAnimationIndex(tileset, gid);
    this.renderAll();
  }

  private onRemoveAnimation(): void {
    const tileset = this.selectedTileset();
    const gid = this.state.selectedTile;
    if (!tileset || gid === EMPTY_GID) return;
    tileset.animations = tileset.animations.filter((animation) => animation.baseGid !== gid);
    const visual = getTileVisual(tileset, gid);
    if (visual) {
      visual.animation = -1;
      if (visual.kind === "flipbook" || visual.kind === "waterFlipbook") {
        visual.kind = "static";
      }
    }
    this.renderAll();
  }

  private onCollisionChanged(): void {
    const tileset = this.selectedTileset();
    const gid = this.state.selectedTile;
    if (!tileset || gid === EMPTY_GID) return;

    const collisionDraft = readCollisionDraft(
      this.collisionPanelRefs,
      gid,
      (text) => this.parseNumericList(text)
    );
    this.upsertCollision(tileset, collisionDraft);
    this.renderAll();
  }

  private async copyJsonToClipboard(): Promise<void> {
    try {
      await navigator.clipboard.writeText(this.jsonBox.value);
      this.setStatus("JSON 已复制");
    } catch {
      this.jsonBox.focus();
      this.jsonBox.select();
      this.setStatus("已选中 JSON 文本");
    }
  }

  private async importFromJsonTextbox(): Promise<void> {
    const payload = JSON.parse(this.jsonBox.value) as unknown;
    await this.loadDocument(payload);
    this.setStatus("已从文本读取");
  }

  private togglePreviewPlayback(): void {
    this.state.previewPlaying = !this.state.previewPlaying;
    this.requireElement<HTMLButtonElement>("playBtn").textContent = this.state.previewPlaying ? "暂停预览" : "播放预览";
  }

  private toggleCollisionOverlay(): void {
    this.state.showCollisionOverlay = !this.state.showCollisionOverlay;
    this.requireElement<HTMLButtonElement>("overlayBtn").classList.toggle("active", this.state.showCollisionOverlay);
    this.renderMap();
  }

  private syncInputs(): void {
    this.requireElement<HTMLInputElement>("mapWInput").value = String(this.state.width);
    this.requireElement<HTMLInputElement>("mapHInput").value = String(this.state.height);
    this.requireElement<HTMLInputElement>("tileSizeInput").value = String(this.state.tileSize);
    this.requireElement<HTMLSelectElement>("zoomInput").value = String(this.state.zoom);
    this.requireElement<HTMLInputElement>("tilesetColsInput").value = String(this.selectedTileset()?.cols ?? 4);
    this.syncPreviewInputs();
  }

  private syncPreviewInputs(): void {
    const timeInput = this.requireElement<HTMLInputElement>("timeInput");
    const clampedTime = Math.max(0, this.state.previewTime);
    timeInput.value = String(Math.min(clampedTime, Number.parseFloat(timeInput.max)));
    this.timeOutputElement.textContent = `${clampedTime.toFixed(2)}s`;
  }

  private syncMapDimensionsFromInputs(): void {
    this.state.width = clampInt(this.requireElement<HTMLInputElement>("mapWInput").value, 1, 512);
    this.state.height = clampInt(this.requireElement<HTMLInputElement>("mapHInput").value, 1, 512);
    this.updateJsonOnly();
  }

  private activeLayer(): EditorLayer {
    return this.state.layers[this.state.activeLayer];
  }

  private selectedTileset(): EditorTileset | null {
    return findTilesetForGid(this.state.tilesets, this.state.selectedTile);
  }

  private paintFromEvent(event: PointerEvent): void {
    const rect = this.canvas.getBoundingClientRect();
    const cellSize = this.state.tileSize * this.state.zoom;
    const tileX = Math.floor((event.clientX - rect.left) / cellSize);
    const tileY = Math.floor((event.clientY - rect.top) / cellSize);
    if (tileX < 0 || tileX >= this.state.width || tileY < 0 || tileY >= this.state.height) {
      return;
    }
    this.activeLayer().tiles[tileIndex(this.state.width, tileX, tileY)] =
      this.state.tool === "eraser" ? EMPTY_GID : this.state.selectedTile;
    this.renderMap();
  }

  private setTool(tool: "brush" | "eraser"): void {
    this.state.tool = tool;
    this.requireElement<HTMLButtonElement>("brushBtn").classList.toggle("active", tool === "brush");
    this.requireElement<HTMLButtonElement>("eraserBtn").classList.toggle("active", tool === "eraser");
  }

  private updateJsonOnly(): void {
    this.jsonBox.value = JSON.stringify(toEngineTileMap(this.state), null, 2);
  }

  private renderAll(): void {
    this.renderTilesets();
    this.renderPalette();
    this.renderLayers();
    this.renderInspector();
    this.renderMap();
  }

  private renderTilesets(): void {
    this.tilesetsElement.innerHTML = "";
    for (const tileset of this.state.tilesets) {
      const row = document.createElement("div");
      row.className = "tileset-row";

      const label = document.createElement("div");
      label.className = "name-cell";
      label.textContent = `${tileset.name}  #${tileset.firstGid}-${tileset.firstGid + tileset.count - 1}`;

      const remove = document.createElement("button");
      remove.className = "mini danger";
      remove.textContent = tileset.builtin ? "保留" : "删除";
      remove.disabled = tileset.builtin;
      remove.addEventListener("click", () => this.removeTileset(tileset.id));

      row.append(label, remove);
      this.tilesetsElement.appendChild(row);
    }
  }

  private renderPalette(): void {
    this.paletteElement.innerHTML = "";
    for (const tileset of this.state.tilesets) {
      for (let gid = tileset.firstGid; gid < tileset.firstGid + tileset.count; gid += 1) {
        if (this.state.hiddenGids.has(gid)) continue;

        const card = document.createElement("div");
        card.className = "tile-card";

        const button = document.createElement("button");
        button.className = `tile-swatch${gid === this.state.selectedTile ? " selected" : ""}`;
        button.title = `${tileset.name} / Tile ${gid}`;
        button.addEventListener("click", () => {
          this.state.selectedTile = gid;
          this.renderAll();
          this.setStatus(`Tile ${gid}`);
        });

        const swatch = document.createElement("canvas");
        swatch.width = this.state.tileSize;
        swatch.height = this.state.tileSize;
        const swatchContext = swatch.getContext("2d");
        if (swatchContext) {
          swatchContext.imageSmoothingEnabled = false;
          this.drawTile(swatchContext, gid, 0, 0, this.state.tileSize, 0, 0, 0);
        }
        button.appendChild(swatch);

        const collisionToggle = document.createElement("button");
        collisionToggle.type = "button";
        collisionToggle.className = `tile-action tile-action-left${getTileCollision(tileset, gid)?.shape === "full" ? " active" : ""}`;
        collisionToggle.textContent = "C";
        collisionToggle.title = "切换 full collision";
        collisionToggle.addEventListener("click", (event) => {
          event.stopPropagation();
          this.toggleFullCollision(tileset, gid);
        });

        const badges = document.createElement("div");
        badges.className = "tile-badges";
        if (getTileVisual(tileset, gid)) badges.appendChild(this.makeBadge("V", "visual"));
        if (getTileAnimation(tileset, gid)) badges.appendChild(this.makeBadge("A", "anim"));
        if (getTileCollision(tileset, gid)) badges.appendChild(this.makeBadge("C", "collision"));

        card.append(button, collisionToggle, badges);
        this.paletteElement.appendChild(card);
      }
    }
  }

  private makeBadge(text: string, className: string): HTMLSpanElement {
    const badge = document.createElement("span");
    badge.className = `badge ${className}`;
    badge.textContent = text;
    return badge;
  }

  private renderLayers(): void {
    renderLayerPanel(this.layersElement, this.state.layers, {
      activeLayer: this.state.activeLayer,
      editingLayer: this.layerEditingIndex,
      draftName: this.layerEditingDraft
    }, {
      onActivate: (index) => {
        this.commitLayerRename();
        this.state.activeLayer = index;
        this.renderAll();
      },
      onBeginRename: (index) => {
        this.layerEditingIndex = index;
        this.layerEditingDraft = this.state.layers[index]?.name ?? "";
        this.renderLayers();
        this.focusEditingLayerInput();
      },
      onDraftRename: (index, value) => {
        if (this.layerEditingIndex !== index) return;
        this.layerEditingDraft = value;
      },
      onCommitRename: (index) => {
        if (this.layerEditingIndex !== index) return;
        this.commitLayerRename();
        this.renderLayers();
      },
      onCancelRename: (index) => {
        if (this.layerEditingIndex !== index) return;
        this.cancelLayerRename();
        this.renderLayers();
      },
      onToggleVisible: (index) => {
        this.commitLayerRename();
        this.state.layers[index].visible = !this.state.layers[index].visible;
        this.renderAll();
      },
      onToggleCollidable: (index) => {
        this.commitLayerRename();
        this.state.layers[index].collidable = !this.state.layers[index].collidable;
        this.renderAll();
      },
      onRenderLayerChange: (index, value) => {
        this.state.layers[index].renderLayer = Number.parseInt(value, 10) || 0;
        this.updateJsonOnly();
      },
      onDelete: (index) => {
        this.commitLayerRename();
        this.state.layers.splice(index, 1);
        this.state.activeLayer = Math.min(this.state.activeLayer, this.state.layers.length - 1);
        if (this.layerEditingIndex >= this.state.layers.length) {
          this.cancelLayerRename();
        }
        this.renderAll();
      }
    });
  }

  private renderInspector(): void {
    const gid = this.state.selectedTile;
    const tileset = this.selectedTileset();
    if (!tileset || gid === EMPTY_GID) {
      this.selectionSummaryElement.textContent = "未选中";
      return;
    }

    const visual = getTileVisual(tileset, gid) ?? {
      gid,
      kind: "static",
      animation: -1,
      speed: 1,
      strength: 0,
      phase: 0,
      flags: 0
    } satisfies TileVisual;

    const animation = getTileAnimation(tileset, gid) ?? {
      baseGid: gid,
      frames: [],
      randomStart: false,
      speed: 1
    } satisfies TileAnimation;

    const collision = getTileCollision(tileset, gid) ?? {
      gid,
      shape: "none",
      points: []
    } satisfies TileCollision;

    this.selectionSummaryElement.textContent = `${tileset.name} / gid ${gid} / local ${localTileId(tileset, gid)}`;
    this.requireElement<HTMLSelectElement>("visualKindSelect").value = visual.kind;
    this.requireElement<HTMLInputElement>("visualSpeedInput").value = String(visual.speed);
    this.requireElement<HTMLInputElement>("visualStrengthInput").value = String(visual.strength);
    this.requireElement<HTMLInputElement>("visualPhaseInput").value = String(visual.phase);
    this.requireElement<HTMLInputElement>("visualFlagsInput").value = String(visual.flags);
    this.requireElement<HTMLInputElement>("animationSpeedInput").value = String(animation.speed);
    this.requireElement<HTMLInputElement>("animationRandomStartInput").checked = !!animation.randomStart;
    syncCollisionPanel(this.collisionPanelRefs, collision);

    this.animationFramesElement.innerHTML = "";
    animation.frames.forEach((frame, index) => {
      const row = document.createElement("div");
      row.className = "frame-row";

      const label = document.createElement("div");
      label.textContent = `#${index + 1}`;

      const gidInput = document.createElement("input");
      gidInput.type = "number";
      gidInput.value = String(frame.gid);
      gidInput.addEventListener("change", () => {
        frame.gid = Number.parseInt(gidInput.value, 10) || gid;
        this.renderMap();
        this.updateJsonOnly();
      });

      const durationInput = document.createElement("input");
      durationInput.type = "number";
      durationInput.step = "0.01";
      durationInput.value = String(frame.duration);
      durationInput.addEventListener("change", () => {
        frame.duration = Math.max(Number.parseFloat(durationInput.value) || 0.12, 0.01);
        this.renderMap();
        this.updateJsonOnly();
      });

      const removeButton = document.createElement("button");
      removeButton.className = "mini danger";
      removeButton.textContent = "删除";
      removeButton.addEventListener("click", () => {
        animation.frames.splice(index, 1);
        this.renderInspector();
        this.renderMap();
      });

      row.append(label, gidInput, durationInput, removeButton);
      this.animationFramesElement.appendChild(row);
    });
  }

  private renderMap(): void {
    const context = this.context;
    const scaledCell = this.state.tileSize * this.state.zoom;
    this.canvas.width = this.state.width * scaledCell;
    this.canvas.height = this.state.height * scaledCell;
    this.canvas.style.width = `${this.canvas.width}px`;
    this.canvas.style.height = `${this.canvas.height}px`;
    context.clearRect(0, 0, this.canvas.width, this.canvas.height);

    const orderedLayers = this.state.layers
      .map((layer, index) => ({ layer, index }))
      .sort((left, right) => left.layer.renderLayer - right.layer.renderLayer || left.index - right.index);

    for (const { layer, index: layerIndex } of orderedLayers) {
      if (!layer.visible) continue;
      context.globalAlpha = layerIndex === this.state.activeLayer ? 1 : 0.58;
      for (let y = 0; y < this.state.height; y += 1) {
        for (let x = 0; x < this.state.width; x += 1) {
          const gid = layer.tiles[tileIndex(this.state.width, x, y)];
          this.drawTile(context, gid, x * scaledCell, y * scaledCell, scaledCell, x, y, layerIndex);
        }
      }
    }
    context.globalAlpha = 1;

    for (let x = 0; x <= this.state.width; x += 1) {
      context.strokeStyle = x % 5 === 0 ? "rgba(255,255,255,0.22)" : "rgba(255,255,255,0.10)";
      context.beginPath();
      context.moveTo(x * scaledCell + 0.5, 0);
      context.lineTo(x * scaledCell + 0.5, this.canvas.height);
      context.stroke();
    }
    for (let y = 0; y <= this.state.height; y += 1) {
      context.strokeStyle = y % 5 === 0 ? "rgba(255,255,255,0.22)" : "rgba(255,255,255,0.10)";
      context.beginPath();
      context.moveTo(0, y * scaledCell + 0.5);
      context.lineTo(this.canvas.width, y * scaledCell + 0.5);
      context.stroke();
    }

    if (this.state.showCollisionOverlay) {
      renderCollisionOverlay(this.context, this.state, {
        findCollision: (gid) => {
          const tileset = findTilesetForGid(this.state.tilesets, gid);
          return tileset ? getTileCollision(tileset, gid) : null;
        }
      });
    }

    this.updateJsonOnly();
  }

  private drawTile(
    target: CanvasRenderingContext2D,
    gid: number,
    dx: number,
    dy: number,
    size: number,
    cellX: number,
    cellY: number,
    layerIndex: number
  ): void {
    if (gid === EMPTY_GID || this.state.hiddenGids.has(gid)) return;
    const tileset = findTilesetForGid(this.state.tilesets, gid);
    if (!tileset) return;

    const previewGid = resolvePreviewGid(tileset, gid, this.state.previewTime, cellX, cellY, layerIndex);
    const previewTileset = findTilesetForGid(this.state.tilesets, previewGid) ?? tileset;
    const localId = localTileId(previewTileset, previewGid);

    if (previewTileset.builtin) {
      target.fillStyle = BUILTIN_COLORS[localId % BUILTIN_COLORS.length];
      target.fillRect(dx, dy, size, size);
      target.fillStyle = "rgba(255,255,255,0.16)";
      target.fillRect(dx + 3, dy + 3, size - 6, Math.max(2, size * 0.16));
      return;
    }

    if (!previewTileset.image) return;
    const sx = (localId % previewTileset.cols) * previewTileset.tileSize;
    const sy = Math.floor(localId / previewTileset.cols) * previewTileset.tileSize;
    target.drawImage(previewTileset.image, sx, sy, previewTileset.tileSize, previewTileset.tileSize, dx, dy, size, size);
  }

  private removeTileset(id: string): void {
    const tileset = this.state.tilesets.find((entry) => entry.id === id);
    if (!tileset || tileset.builtin) return;
    this.state.tilesets = this.state.tilesets.filter((entry) => entry.id !== id);
    for (let gid = tileset.firstGid; gid < tileset.firstGid + tileset.count; gid += 1) {
      this.state.hiddenGids.add(gid);
    }
    this.state.layers.forEach((layer) => {
      layer.tiles = layer.tiles.map((gid) => (findTilesetForGid(this.state.tilesets, gid) ? gid : EMPTY_GID));
    });
    this.state.selectedTile = findTilesetForGid(this.state.tilesets, this.state.selectedTile)?.firstGid ?? 0;
    this.renderAll();
    this.setStatus(`已删除 ${tileset.name}`);
  }

  private upsertVisual(tileset: EditorTileset, nextVisual: TileVisual): void {
    const index = tileset.visuals.findIndex((visual) => visual.gid === nextVisual.gid);
    if (
      nextVisual.kind === "static" &&
      nextVisual.animation < 0 &&
      nextVisual.speed === 1 &&
      nextVisual.strength === 0 &&
      nextVisual.phase === 0 &&
      nextVisual.flags === 0
    ) {
      if (index >= 0) {
        tileset.visuals.splice(index, 1);
      }
      return;
    }
    if (index >= 0) tileset.visuals[index] = nextVisual;
    else tileset.visuals.push(nextVisual);
  }

  private ensureAnimation(tileset: EditorTileset, gid: number): TileAnimation {
    let animation = tileset.animations.find((entry) => entry.baseGid === gid);
    if (!animation) {
      animation = { baseGid: gid, frames: [{ gid, duration: 0.12 }], randomStart: false, speed: 1 };
      tileset.animations.push(animation);
    }
    return animation;
  }

  private rebindVisualAnimationIndex(tileset: EditorTileset, gid: number): void {
    const visual = getTileVisual(tileset, gid) ?? {
      gid,
      kind: "flipbook",
      animation: -1,
      speed: 1,
      strength: 0,
      phase: 0,
      flags: 0
    };
    const animationIndex = tileset.animations.findIndex((entry) => entry.baseGid === gid);
    visual.animation = animationIndex;
    if (visual.kind === "static" && animationIndex >= 0) {
      visual.kind = "flipbook";
    }
    this.upsertVisual(tileset, visual);
  }

  private upsertCollision(tileset: EditorTileset, nextCollision: TileCollision): void {
    const index = tileset.collisions.findIndex((collision) => collision.gid === nextCollision.gid);
    if (nextCollision.shape === "none") {
      if (index >= 0) tileset.collisions.splice(index, 1);
      return;
    }
    if (index >= 0) tileset.collisions[index] = nextCollision;
    else tileset.collisions.push(nextCollision);
  }

  private toggleFullCollision(tileset: EditorTileset, gid: number): void {
    toggleFullCollisionInPanel(tileset, gid);
    if (this.state.selectedTile === gid) {
      this.renderInspector();
    }
    this.renderMap();
    this.renderPalette();
  }

  private parseNumericList(text: string): number[] {
    return text
      .split(/[\s,]+/)
      .map((token) => token.trim())
      .filter(Boolean)
      .map((token) => Number.parseFloat(token))
      .filter((value) => Number.isFinite(value));
  }

  private async loadDocument(payload: unknown): Promise<void> {
    const data = payload as Record<string, unknown>;
    if (data?.type === "qgame.tilemap.editor") {
      await this.loadEditorProject(data as unknown as EditorProjectDocument);
      return;
    }
    if (data?.type === "qgame.tilemap.engine-package") {
      await this.loadEnginePackage(data as unknown as EnginePackageDocument);
      return;
    }
    await this.loadEngineTileMap(data as unknown as EngineTileMapDocument);
  }

  private async loadEditorProject(project: EditorProjectDocument): Promise<void> {
    const state = createDefaultState();
    this.cancelLayerRename();
    state.width = clampInt(project.width, 1, 512);
    state.height = clampInt(project.height, 1, 512);
    state.tileSize = clampInt(project.tileSize, 4, 256);
    state.zoom = clampInt(project.zoom, 1, 4);
    state.activeLayer = clampInt(project.activeLayer, 0, Math.max(0, project.layers.length - 1));
    state.selectedTile = Number(project.selectedTile ?? 0);
    state.nextGid = Math.max(Number(project.nextGid ?? BUILTIN_COLORS.length), BUILTIN_COLORS.length);
    state.hiddenGids = new Set(project.hiddenGids ?? []);
    state.previewTime = Number(project.previewTime ?? 0);
    state.previewPlaying = !!project.previewPlaying;
    state.showCollisionOverlay = project.showCollisionOverlay ?? true;
    state.tilesets = [];

    for (const spec of project.tilesets ?? []) {
      let image: HTMLImageElement | null = null;
      if (!spec.builtin && spec.dataUrl) {
        image = await this.loadImage(spec.dataUrl);
      }
      state.tilesets.push({
        id: spec.id,
        name: spec.name,
        firstGid: spec.firstGid,
        count: spec.count,
        cols: spec.cols,
        tileSize: spec.tileSize,
        builtin: spec.builtin,
        dataUrl: spec.dataUrl,
        image,
        assetId: spec.assetId,
        texturePath: spec.texturePath,
        sourceKind: spec.sourceKind,
        visuals: spec.visuals ?? [],
        animations: spec.animations ?? [],
        collisions: spec.collisions ?? []
      });
    }

    if (!state.tilesets.some((tileset) => tileset.builtin)) {
      state.tilesets.unshift(createBuiltinTileset());
    }

    state.layers = (project.layers ?? []).map((layer) => ({
      name: layer.name,
      visible: layer.visible ?? true,
      collidable: layer.collidable ?? true,
      renderLayer: layer.renderLayer ?? 0,
      tiles: [...layer.tiles]
    }));
    if (state.layers.length === 0) {
      state.layers = [
        createLayer("Ground", state.width, state.height, false, true, 0),
        createLayer("Objects", state.width, state.height, true, true, 10)
      ];
    }

    this.state = state;
    this.state.nextGid = computeNextGid(this.state.tilesets);
    this.syncInputs();
    this.renderAll();
  }

  private async loadEnginePackage(pkg: EnginePackageDocument): Promise<void> {
    this.cancelLayerRename();
    const tileMap = pkg.tileMap;
    const resourcesByKey = new Map<string, string>();
    for (const resource of pkg.resources ?? []) {
      const key = `${resource.id ?? ""}::${resource.firstGid ?? ""}`;
      if (resource.dataUrl) {
        resourcesByKey.set(key, resource.dataUrl);
      }
    }
    await this.loadEngineTileMap(tileMap, resourcesByKey);
  }

  private async loadEngineTileMap(
    tileMap: EngineTileMapDocument,
    resourcesByKey = new Map<string, string>()
  ): Promise<void> {
    const state = createDefaultState();
    this.cancelLayerRename();
    state.width = clampInt(tileMap.w ?? state.width, 1, 512);
    state.height = clampInt(tileMap.h ?? state.height, 1, 512);
    state.tileSize = clampInt(tileMap.ts ?? state.tileSize, 4, 256);
    state.tilesets = [];
    state.hiddenGids = new Set<number>();

    for (const spec of tileMap.tilesets ?? []) {
      const builtin = (spec.sourceKind === "builtin") || spec.id === "builtin" || spec.firstGid === 0;
      const dataUrl = spec.sourceDataUrl || resourcesByKey.get(`${spec.id ?? ""}::${spec.firstGid}`) || "";
      const image = !builtin && dataUrl ? await this.loadImage(dataUrl) : null;
      const collisions = spec.collisions && spec.collisions.length > 0
        ? spec.collisions
        : (spec.collision ?? []).flatMap((solid, localId) => solid ? [{
            gid: spec.firstGid + localId,
            shape: "full" as const,
            points: []
          }] : []);

      state.tilesets.push({
        id: spec.id || `${spec.firstGid}`,
        name: spec.name || spec.tex || `tileset-${spec.firstGid}`,
        firstGid: spec.firstGid,
        count: spec.count,
        cols: spec.cols,
        tileSize: state.tileSize,
        builtin,
        dataUrl,
        image,
        assetId: spec.assetId ?? "",
        texturePath: spec.tex ?? "",
        sourceKind: spec.sourceKind ?? (builtin ? "builtin" : "image"),
        visuals: spec.visuals ?? [],
        animations: spec.animations ?? [],
        collisions
      });
    }

    if (!state.tilesets.some((tileset) => tileset.builtin)) {
      state.tilesets.unshift(createBuiltinTileset());
    }

    state.layers = (tileMap.layers ?? []).map((layer, index) => ({
      name: layer.name || `Layer ${index + 1}`,
      visible: layer.visible ?? true,
      collidable: layer.collidable ?? true,
      renderLayer: layer.renderLayer ?? index * 10,
      tiles: [...layer.tiles]
    }));
    if (state.layers.length === 0) {
      state.layers = [
        createLayer("Ground", state.width, state.height, false, true, 0),
        createLayer("Objects", state.width, state.height, true, true, 10)
      ];
    }

    state.activeLayer = 0;
    state.selectedTile = state.tilesets[0]?.firstGid ?? 0;
    state.nextGid = computeNextGid(state.tilesets);
    this.state = state;
    this.syncInputs();
    this.renderAll();
  }

  private async loadImage(src: string): Promise<HTMLImageElement> {
    return await new Promise((resolve, reject) => {
      const image = new Image();
      image.onload = () => resolve(image);
      image.onerror = reject;
      image.src = src;
    });
  }

  private async readFileAsDataUrl(file: File): Promise<string> {
    return await new Promise((resolve, reject) => {
      const reader = new FileReader();
      reader.onload = () => resolve(String(reader.result));
      reader.onerror = reject;
      reader.readAsDataURL(file);
    });
  }

  private downloadJson(filename: string, payload: unknown): void {
    const blob = new Blob([JSON.stringify(payload, null, 2)], { type: "application/json" });
    const url = URL.createObjectURL(blob);
    const link = document.createElement("a");
    link.href = url;
    link.download = filename;
    link.click();
    URL.revokeObjectURL(url);
    this.setStatus(`${filename} 已导出`);
  }

  private setStatus(text: string): void {
    this.statusElement.textContent = text;
  }

  /**
   * Persist the current rename draft back into the layer list only when the
   * user explicitly finishes renaming or focus leaves the text box.
   */
  private commitLayerRename(): void {
    if (this.layerEditingIndex < 0) return;
    const layer = this.state.layers[this.layerEditingIndex];
    if (layer) {
      layer.name = this.layerEditingDraft.trim() || `Layer ${this.layerEditingIndex + 1}`;
    }
    this.layerEditingIndex = -1;
    this.layerEditingDraft = "";
    this.updateJsonOnly();
  }

  /**
   * Leave rename mode without touching the stored layer name.
   */
  private cancelLayerRename(): void {
    this.layerEditingIndex = -1;
    this.layerEditingDraft = "";
  }

  /**
   * Restore focus after the layer list re-renders into rename mode.
   */
  private focusEditingLayerInput(): void {
    if (this.layerEditingIndex < 0) return;
    requestAnimationFrame(() => {
      const input = this.layersElement.querySelector<HTMLInputElement>(
        `.layer-name-input[data-layer-index="${this.layerEditingIndex}"]`
      );
      if (!input) return;
      input.focus();
      input.select();
    });
  }
}

new TilemapEditorApp();
