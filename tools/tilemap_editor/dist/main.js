(() => {
  // src/constants.ts
  var BUILTIN_COLORS = [
    "#5a9f4d",
    "#6fb85f",
    "#a47b48",
    "#7f5f3b",
    "#4a8cbf",
    "#3d76a3",
    "#c9b36a",
    "#d9cf8f",
    "#6d737a",
    "#89919a",
    "#9c5d4e",
    "#bd7a69",
    "#2f6f43",
    "#255838",
    "#b7b7b7",
    "#e3e3e3"
  ];
  var EMPTY_GID = -1;
  var DEFAULT_MAP_WIDTH = 20;
  var DEFAULT_MAP_HEIGHT = 15;
  var DEFAULT_TILE_SIZE = 32;

  // src/schema.ts
  function clampInt(value, min, max) {
    const parsed = Number.parseInt(String(value ?? min), 10);
    if (!Number.isFinite(parsed))
      return min;
    return Math.max(min, Math.min(max, parsed));
  }
  function blankTiles(width, height) {
    return Array(width * height).fill(EMPTY_GID);
  }
  function tileIndex(width, x, y) {
    return y * width + x;
  }
  function createLayer(name, width, height, collidable = true, visible = true, renderLayer = 0) {
    return {
      name,
      visible,
      collidable,
      renderLayer,
      tiles: blankTiles(width, height)
    };
  }
  function createBuiltinTileset() {
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
  function createDefaultState() {
    return {
      width: DEFAULT_MAP_WIDTH,
      height: DEFAULT_MAP_HEIGHT,
      tileSize: DEFAULT_TILE_SIZE,
      zoom: 2,
      tool: "brush",
      activeLayer: 0,
      selectedTile: 0,
      nextGid: BUILTIN_COLORS.length,
      hiddenGids: new Set,
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
  function computeNextGid(tilesets) {
    return tilesets.reduce((next, tileset) => Math.max(next, tileset.firstGid + tileset.count), BUILTIN_COLORS.length);
  }
  function findTilesetForGid(tilesets, gid) {
    return tilesets.find((tileset) => gid >= tileset.firstGid && gid < tileset.firstGid + tileset.count) ?? null;
  }
  function localTileId(tileset, gid) {
    return gid - tileset.firstGid;
  }
  function getTileVisual(tileset, gid) {
    return tileset.visuals.find((visual) => visual.gid === gid) ?? null;
  }
  function getTileAnimation(tileset, gid) {
    const visual = getTileVisual(tileset, gid);
    if (visual && visual.animation >= 0 && visual.animation < tileset.animations.length) {
      return tileset.animations[visual.animation] ?? null;
    }
    return tileset.animations.find((animation) => animation.baseGid === gid) ?? null;
  }
  function getTileCollision(tileset, gid) {
    return tileset.collisions.find((collision) => collision.gid === gid) ?? null;
  }
  function resolvePreviewGid(tileset, gid, timeSeconds, cellX, cellY, layerIndex) {
    const visual = getTileVisual(tileset, gid);
    if (!visual)
      return gid;
    if (visual.kind !== "flipbook" && visual.kind !== "waterFlipbook")
      return gid;
    const animation = getTileAnimation(tileset, gid);
    if (!animation || animation.frames.length === 0)
      return gid;
    const frameDurations = animation.frames.map((frame) => Math.max(frame.duration, 0.0001));
    const totalDuration = frameDurations.reduce((sum, duration) => sum + duration, 0);
    if (totalDuration <= 0)
      return gid;
    let phaseSeconds = visual.phase;
    if (animation.randomStart) {
      let hash = 2166136261;
      for (const value of [gid, cellX, cellY, layerIndex]) {
        hash ^= value >>> 0;
        hash = Math.imul(hash, 16777619);
      }
      phaseSeconds += (hash >>> 0 & 16777215) / 16777216 * totalDuration;
    }
    const playbackSpeed = Math.max(visual.speed || 1, 0.0001) * Math.max(animation.speed || 1, 0.0001);
    let localTime = (timeSeconds * playbackSpeed + phaseSeconds) % totalDuration;
    if (localTime < 0)
      localTime += totalDuration;
    for (let index = 0;index < animation.frames.length; index += 1) {
      localTime -= frameDurations[index];
      if (localTime < 0) {
        return animation.frames[index].gid ?? gid;
      }
    }
    return animation.frames[animation.frames.length - 1]?.gid ?? gid;
  }
  function toEngineTileMap(state) {
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
  function toEnginePackage(state) {
    return {
      type: "qgame.tilemap.engine-package",
      version: 2,
      tileMap: toEngineTileMap(state),
      resources: state.tilesets.filter((tileset) => !tileset.builtin).map((tileset) => ({
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
  function toEditorProject(state) {
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

  // src/collision_panel.ts
  function createCollisionPanelRefs(root = document) {
    const shapeSelect = root.querySelector("#collisionShapeSelect");
    const pointsInput = root.querySelector("#collisionPointsInput");
    if (!shapeSelect || !pointsInput) {
      throw new Error("Collision panel elements are missing from the editor shell");
    }
    return { shapeSelect, pointsInput };
  }
  function bindCollisionPanel(refs, onChange) {
    refs.shapeSelect.addEventListener("change", onChange);
    refs.pointsInput.addEventListener("input", onChange);
  }
  function syncCollisionPanel(refs, collision) {
    refs.shapeSelect.value = collision?.shape ?? "none";
    refs.pointsInput.value = collision?.points.join(", ") ?? "";
  }
  function readCollisionDraft(refs, gid, parseNumberList) {
    return {
      gid,
      shape: refs.shapeSelect.value,
      points: parseNumberList(refs.pointsInput.value)
    };
  }
  function toggleFullCollision(tileset, gid) {
    const existingIndex = tileset.collisions.findIndex((collision) => collision.gid === gid);
    const existing = existingIndex >= 0 ? tileset.collisions[existingIndex] : null;
    if (existing?.shape === "full") {
      tileset.collisions.splice(existingIndex, 1);
      return;
    }
    const nextCollision = {
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
  function renderCollisionOverlay(context, state, resolver) {
    const cellSize = state.tileSize * state.zoom;
    for (let layerIndex = 0;layerIndex < state.layers.length; layerIndex += 1) {
      const layer = state.layers[layerIndex];
      if (!layer.visible || !layer.collidable)
        continue;
      for (let y = 0;y < state.height; y += 1) {
        for (let x = 0;x < state.width; x += 1) {
          const gid = layer.tiles[tileIndex(state.width, x, y)];
          const collision = resolver.findCollision(gid);
          if (!collision || collision.shape === "none")
            continue;
          context.strokeStyle = collision.shape === "trigger" ? "rgba(240,180,41,0.85)" : "rgba(228,93,93,0.85)";
          context.fillStyle = collision.shape === "trigger" ? "rgba(240,180,41,0.16)" : "rgba(228,93,93,0.18)";
          if (collision.shape === "rect" && collision.points.length >= 4) {
            const [px, py, width, height] = collision.points;
            context.fillRect(x * cellSize + px * state.zoom, y * cellSize + py * state.zoom, width * state.zoom, height * state.zoom);
            context.strokeRect(x * cellSize + px * state.zoom, y * cellSize + py * state.zoom, width * state.zoom, height * state.zoom);
            continue;
          }
          if ((collision.shape === "polygon" || collision.shape === "oneWay") && collision.points.length >= 4) {
            context.beginPath();
            for (let pointIndex = 0;pointIndex + 1 < collision.points.length; pointIndex += 2) {
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

  // src/layer_panel.ts
  function renderLayerPanel(container, layers, panelState, callbacks) {
    container.innerHTML = "";
    layers.forEach((layer, index) => {
      const row = document.createElement("div");
      row.className = `layer-row${index === panelState.activeLayer ? " active" : ""}`;
      row.appendChild(index === panelState.editingLayer ? createEditingNameInput(index, panelState.draftName, callbacks) : createStaticNameLabel(layer.name));
      const controls = document.createElement("div");
      controls.className = "layer-controls";
      const activateButton = document.createElement("button");
      activateButton.type = "button";
      activateButton.className = "mini";
      activateButton.textContent = index === panelState.activeLayer ? "当前层" : "设为当前";
      activateButton.addEventListener("click", () => callbacks.onActivate(index));
      const renameButton = document.createElement("button");
      renameButton.type = "button";
      renameButton.className = "mini";
      renameButton.textContent = index === panelState.editingLayer ? "完成" : "重命名";
      renameButton.addEventListener("click", () => {
        if (index === panelState.editingLayer) {
          callbacks.onCommitRename(index);
        } else {
          callbacks.onBeginRename(index);
        }
      });
      const visibleButton = document.createElement("button");
      visibleButton.type = "button";
      visibleButton.className = `mini${layer.visible ? " active" : ""}`;
      visibleButton.textContent = layer.visible ? "可见" : "隐藏";
      visibleButton.addEventListener("click", () => callbacks.onToggleVisible(index));
      const collidableButton = document.createElement("button");
      collidableButton.type = "button";
      collidableButton.className = `mini${layer.collidable ? " active" : ""}`;
      collidableButton.textContent = layer.collidable ? "碰撞" : "无碰撞";
      collidableButton.addEventListener("click", () => callbacks.onToggleCollidable(index));
      const renderLayerInput = document.createElement("input");
      renderLayerInput.className = "inline-number";
      renderLayerInput.type = "number";
      renderLayerInput.value = String(layer.renderLayer);
      renderLayerInput.title = "render layer";
      renderLayerInput.addEventListener("change", () => callbacks.onRenderLayerChange(index, renderLayerInput.value));
      const deleteButton = document.createElement("button");
      deleteButton.type = "button";
      deleteButton.className = "mini danger";
      deleteButton.textContent = "删除";
      deleteButton.disabled = layers.length <= 1;
      deleteButton.addEventListener("click", () => callbacks.onDelete(index));
      controls.append(activateButton, renameButton, visibleButton, collidableButton, renderLayerInput, deleteButton);
      row.appendChild(controls);
      container.appendChild(row);
    });
  }
  function createStaticNameLabel(name) {
    const label = document.createElement("div");
    label.className = "layer-name-label";
    label.textContent = name;
    return label;
  }
  function createEditingNameInput(index, value, callbacks) {
    const input = document.createElement("input");
    input.type = "text";
    input.className = "layer-name-input";
    input.value = value;
    input.autocomplete = "off";
    input.spellcheck = false;
    input.dataset.layerIndex = String(index);
    input.addEventListener("input", () => callbacks.onDraftRename(index, input.value));
    input.addEventListener("blur", () => callbacks.onCommitRename(index));
    input.addEventListener("keydown", (event) => {
      if (event.key === "Enter") {
        event.preventDefault();
        callbacks.onCommitRename(index);
      } else if (event.key === "Escape") {
        event.preventDefault();
        callbacks.onCancelRename(index);
      }
    });
    return input;
  }

  // src/main.ts
  class TilemapEditorApp {
    constructor() {
      this.canvas = this.requireElement("mapCanvas");
      const context = this.canvas.getContext("2d");
      if (!context) {
        throw new Error("2D canvas context not available");
      }
      this.context = context;
      this.paletteElement = this.requireElement("palette");
      this.tilesetsElement = this.requireElement("tilesets");
      this.layersElement = this.requireElement("layers");
      this.animationFramesElement = this.requireElement("animationFrames");
      this.jsonBox = this.requireElement("jsonBox");
      this.statusElement = this.requireElement("status");
      this.loadFileInput = this.requireElement("loadFile");
      this.selectionSummaryElement = this.requireElement("selectionSummary");
      this.timeOutputElement = this.requireElement("timeOutput");
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
    requireElement(id) {
      const element = document.getElementById(id);
      if (!element) {
        throw new Error(`Missing required element #${id}`);
      }
      return element;
    }
    makeId() {
      const randomId = globalThis.crypto?.randomUUID?.();
      if (randomId)
        return randomId;
      return `tilemap-${Date.now()}-${Math.floor(Math.random() * 1e6)}`;
    }
    onAnimationFrame(timestamp) {
      const deltaSeconds = Math.max(0, (timestamp - this.lastAnimationTick) / 1000);
      this.lastAnimationTick = timestamp;
      if (this.state.previewPlaying) {
        this.state.previewTime += deltaSeconds;
        this.syncPreviewInputs();
        this.renderMap();
      }
      requestAnimationFrame((nextTimestamp) => this.onAnimationFrame(nextTimestamp));
    }
    bindEvents() {
      this.requireElement("newBtn").addEventListener("click", () => this.onNewMap());
      this.requireElement("brushBtn").addEventListener("click", () => this.setTool("brush"));
      this.requireElement("eraserBtn").addEventListener("click", () => this.setTool("eraser"));
      this.requireElement("playBtn").addEventListener("click", () => this.togglePreviewPlayback());
      this.requireElement("overlayBtn").addEventListener("click", () => this.toggleCollisionOverlay());
      this.requireElement("saveBtn").addEventListener("click", () => this.downloadJson("tilemap_project.json", toEditorProject(this.state)));
      this.requireElement("loadBtn").addEventListener("click", () => this.loadFileInput.click());
      this.requireElement("exportBtn").addEventListener("click", () => this.downloadJson("tilemap_engine_package.json", toEnginePackage(this.state)));
      this.requireElement("resizeBtn").addEventListener("click", () => this.onResizeMap());
      this.requireElement("clearLayerBtn").addEventListener("click", () => this.onClearActiveLayer());
      this.requireElement("addLayerBtn").addEventListener("click", () => this.onAddLayer());
      this.requireElement("copyJsonBtn").addEventListener("click", () => void this.copyJsonToClipboard());
      this.requireElement("importJsonBtn").addEventListener("click", () => void this.importFromJsonTextbox());
      this.requireElement("addFrameBtn").addEventListener("click", () => this.onAddAnimationFrame());
      this.requireElement("removeAnimationBtn").addEventListener("click", () => this.onRemoveAnimation());
      this.requireElement("mapWInput").addEventListener("change", () => this.syncMapDimensionsFromInputs());
      this.requireElement("mapHInput").addEventListener("change", () => this.syncMapDimensionsFromInputs());
      this.requireElement("tileSizeInput").addEventListener("change", () => this.onTileSizeChanged());
      this.requireElement("tilesetColsInput").addEventListener("change", () => this.updateJsonOnly());
      this.requireElement("zoomInput").addEventListener("change", () => this.onZoomChanged());
      this.requireElement("tilesetFile").addEventListener("change", (event) => void this.onImportTilesets(event));
      this.requireElement("timeInput").addEventListener("input", () => this.onPreviewTimeChanged());
      this.loadFileInput.addEventListener("change", (event) => void this.onLoadFilePicked(event));
      this.requireElement("visualKindSelect").addEventListener("change", () => this.onVisualChanged());
      this.requireElement("visualSpeedInput").addEventListener("change", () => this.onVisualChanged());
      this.requireElement("visualStrengthInput").addEventListener("change", () => this.onVisualChanged());
      this.requireElement("visualPhaseInput").addEventListener("change", () => this.onVisualChanged());
      this.requireElement("visualFlagsInput").addEventListener("change", () => this.onVisualChanged());
      this.requireElement("animationSpeedInput").addEventListener("change", () => this.onAnimationChanged());
      this.requireElement("animationRandomStartInput").addEventListener("change", () => this.onAnimationChanged());
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
      this.canvas.addEventListener("pointerup", () => {
        this.painting = false;
      });
      this.canvas.addEventListener("pointercancel", () => {
        this.painting = false;
      });
    }
    onNewMap() {
      this.state = createDefaultState();
      this.cancelLayerRename();
      this.state.width = clampInt(this.requireElement("mapWInput").value, 1, 512);
      this.state.height = clampInt(this.requireElement("mapHInput").value, 1, 512);
      this.state.tileSize = clampInt(this.requireElement("tileSizeInput").value, 4, 256);
      this.state.zoom = clampInt(this.requireElement("zoomInput").value, 1, 4);
      this.state.layers = [
        createLayer("Ground", this.state.width, this.state.height, false, true, 0),
        createLayer("Objects", this.state.width, this.state.height, true, true, 10)
      ];
      this.syncInputs();
      this.renderAll();
      this.setStatus("新建地图完成");
    }
    onResizeMap() {
      const newWidth = clampInt(this.requireElement("mapWInput").value, 1, 512);
      const newHeight = clampInt(this.requireElement("mapHInput").value, 1, 512);
      const oldWidth = this.state.width;
      const oldHeight = this.state.height;
      const previousLayers = this.state.layers.map((layer) => [...layer.tiles]);
      this.state.width = newWidth;
      this.state.height = newHeight;
      this.state.layers.forEach((layer, layerIndex) => {
        layer.tiles = blankTiles(newWidth, newHeight);
        for (let y = 0;y < Math.min(oldHeight, newHeight); y += 1) {
          for (let x = 0;x < Math.min(oldWidth, newWidth); x += 1) {
            layer.tiles[tileIndex(newWidth, x, y)] = previousLayers[layerIndex][tileIndex(oldWidth, x, y)];
          }
        }
      });
      this.renderAll();
      this.setStatus("地图尺寸已应用");
    }
    onClearActiveLayer() {
      this.activeLayer().tiles.fill(EMPTY_GID);
      this.renderMap();
      this.setStatus(`已清空 ${this.activeLayer().name}`);
    }
    onAddLayer() {
      this.commitLayerRename();
      this.state.layers.push(createLayer(`Layer ${this.state.layers.length + 1}`, this.state.width, this.state.height, false, true, this.state.layers.length * 10));
      this.state.activeLayer = this.state.layers.length - 1;
      this.renderAll();
      this.setStatus("已添加图层");
    }
    onZoomChanged() {
      this.state.zoom = clampInt(this.requireElement("zoomInput").value, 1, 4);
      this.renderMap();
    }
    onTileSizeChanged() {
      this.state.tileSize = clampInt(this.requireElement("tileSizeInput").value, 4, 256);
      this.renderAll();
    }
    onPreviewTimeChanged() {
      const slider = this.requireElement("timeInput");
      this.state.previewTime = Number.parseFloat(slider.value) || 0;
      this.syncPreviewInputs();
      this.renderMap();
    }
    async onImportTilesets(event) {
      const input = event.currentTarget;
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
    async onLoadFilePicked(event) {
      const file = event.currentTarget.files?.[0];
      if (!file)
        return;
      const text = await file.text();
      const payload = JSON.parse(text);
      await this.loadDocument(payload);
      event.currentTarget.value = "";
      this.setStatus(`已读取 ${file.name}`);
    }
    onVisualChanged() {
      const tileset = this.selectedTileset();
      const gid = this.state.selectedTile;
      if (!tileset || gid === EMPTY_GID)
        return;
      const kind = this.requireElement("visualKindSelect").value;
      const speed = Number.parseFloat(this.requireElement("visualSpeedInput").value) || 1;
      const strength = Number.parseFloat(this.requireElement("visualStrengthInput").value) || 0;
      const phase = Number.parseFloat(this.requireElement("visualPhaseInput").value) || 0;
      const flags = clampInt(this.requireElement("visualFlagsInput").value, 0, 4294967295);
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
    onAnimationChanged() {
      const tileset = this.selectedTileset();
      const gid = this.state.selectedTile;
      if (!tileset || gid === EMPTY_GID)
        return;
      const animation = this.ensureAnimation(tileset, gid);
      animation.speed = Number.parseFloat(this.requireElement("animationSpeedInput").value) || 1;
      animation.randomStart = this.requireElement("animationRandomStartInput").checked;
      this.rebindVisualAnimationIndex(tileset, gid);
      this.renderAll();
    }
    onAddAnimationFrame() {
      const tileset = this.selectedTileset();
      const gid = this.state.selectedTile;
      if (!tileset || gid === EMPTY_GID)
        return;
      const animation = this.ensureAnimation(tileset, gid);
      animation.frames.push({ gid, duration: 0.12 });
      this.rebindVisualAnimationIndex(tileset, gid);
      this.renderAll();
    }
    onRemoveAnimation() {
      const tileset = this.selectedTileset();
      const gid = this.state.selectedTile;
      if (!tileset || gid === EMPTY_GID)
        return;
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
    onCollisionChanged() {
      const tileset = this.selectedTileset();
      const gid = this.state.selectedTile;
      if (!tileset || gid === EMPTY_GID)
        return;
      const collisionDraft = readCollisionDraft(this.collisionPanelRefs, gid, (text) => this.parseNumericList(text));
      this.upsertCollision(tileset, collisionDraft);
      this.renderAll();
    }
    async copyJsonToClipboard() {
      try {
        await navigator.clipboard.writeText(this.jsonBox.value);
        this.setStatus("JSON 已复制");
      } catch {
        this.jsonBox.focus();
        this.jsonBox.select();
        this.setStatus("已选中 JSON 文本");
      }
    }
    async importFromJsonTextbox() {
      const payload = JSON.parse(this.jsonBox.value);
      await this.loadDocument(payload);
      this.setStatus("已从文本读取");
    }
    togglePreviewPlayback() {
      this.state.previewPlaying = !this.state.previewPlaying;
      this.requireElement("playBtn").textContent = this.state.previewPlaying ? "暂停预览" : "播放预览";
    }
    toggleCollisionOverlay() {
      this.state.showCollisionOverlay = !this.state.showCollisionOverlay;
      this.requireElement("overlayBtn").classList.toggle("active", this.state.showCollisionOverlay);
      this.renderMap();
    }
    syncInputs() {
      this.requireElement("mapWInput").value = String(this.state.width);
      this.requireElement("mapHInput").value = String(this.state.height);
      this.requireElement("tileSizeInput").value = String(this.state.tileSize);
      this.requireElement("zoomInput").value = String(this.state.zoom);
      this.requireElement("tilesetColsInput").value = String(this.selectedTileset()?.cols ?? 4);
      this.syncPreviewInputs();
    }
    syncPreviewInputs() {
      const timeInput = this.requireElement("timeInput");
      const clampedTime = Math.max(0, this.state.previewTime);
      timeInput.value = String(Math.min(clampedTime, Number.parseFloat(timeInput.max)));
      this.timeOutputElement.textContent = `${clampedTime.toFixed(2)}s`;
    }
    syncMapDimensionsFromInputs() {
      this.state.width = clampInt(this.requireElement("mapWInput").value, 1, 512);
      this.state.height = clampInt(this.requireElement("mapHInput").value, 1, 512);
      this.updateJsonOnly();
    }
    activeLayer() {
      return this.state.layers[this.state.activeLayer];
    }
    selectedTileset() {
      return findTilesetForGid(this.state.tilesets, this.state.selectedTile);
    }
    paintFromEvent(event) {
      const rect = this.canvas.getBoundingClientRect();
      const cellSize = this.state.tileSize * this.state.zoom;
      const tileX = Math.floor((event.clientX - rect.left) / cellSize);
      const tileY = Math.floor((event.clientY - rect.top) / cellSize);
      if (tileX < 0 || tileX >= this.state.width || tileY < 0 || tileY >= this.state.height) {
        return;
      }
      this.activeLayer().tiles[tileIndex(this.state.width, tileX, tileY)] = this.state.tool === "eraser" ? EMPTY_GID : this.state.selectedTile;
      this.renderMap();
    }
    setTool(tool) {
      this.state.tool = tool;
      this.requireElement("brushBtn").classList.toggle("active", tool === "brush");
      this.requireElement("eraserBtn").classList.toggle("active", tool === "eraser");
    }
    updateJsonOnly() {
      this.jsonBox.value = JSON.stringify(toEngineTileMap(this.state), null, 2);
    }
    renderAll() {
      this.renderTilesets();
      this.renderPalette();
      this.renderLayers();
      this.renderInspector();
      this.renderMap();
    }
    renderTilesets() {
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
    renderPalette() {
      this.paletteElement.innerHTML = "";
      for (const tileset of this.state.tilesets) {
        for (let gid = tileset.firstGid;gid < tileset.firstGid + tileset.count; gid += 1) {
          if (this.state.hiddenGids.has(gid))
            continue;
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
          if (getTileVisual(tileset, gid))
            badges.appendChild(this.makeBadge("V", "visual"));
          if (getTileAnimation(tileset, gid))
            badges.appendChild(this.makeBadge("A", "anim"));
          if (getTileCollision(tileset, gid))
            badges.appendChild(this.makeBadge("C", "collision"));
          card.append(button, collisionToggle, badges);
          this.paletteElement.appendChild(card);
        }
      }
    }
    makeBadge(text, className) {
      const badge = document.createElement("span");
      badge.className = `badge ${className}`;
      badge.textContent = text;
      return badge;
    }
    renderLayers() {
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
          if (this.layerEditingIndex !== index)
            return;
          this.layerEditingDraft = value;
        },
        onCommitRename: (index) => {
          if (this.layerEditingIndex !== index)
            return;
          this.commitLayerRename();
          this.renderLayers();
        },
        onCancelRename: (index) => {
          if (this.layerEditingIndex !== index)
            return;
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
    renderInspector() {
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
      };
      const animation = getTileAnimation(tileset, gid) ?? {
        baseGid: gid,
        frames: [],
        randomStart: false,
        speed: 1
      };
      const collision = getTileCollision(tileset, gid) ?? {
        gid,
        shape: "none",
        points: []
      };
      this.selectionSummaryElement.textContent = `${tileset.name} / gid ${gid} / local ${localTileId(tileset, gid)}`;
      this.requireElement("visualKindSelect").value = visual.kind;
      this.requireElement("visualSpeedInput").value = String(visual.speed);
      this.requireElement("visualStrengthInput").value = String(visual.strength);
      this.requireElement("visualPhaseInput").value = String(visual.phase);
      this.requireElement("visualFlagsInput").value = String(visual.flags);
      this.requireElement("animationSpeedInput").value = String(animation.speed);
      this.requireElement("animationRandomStartInput").checked = !!animation.randomStart;
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
    renderMap() {
      const context = this.context;
      const scaledCell = this.state.tileSize * this.state.zoom;
      this.canvas.width = this.state.width * scaledCell;
      this.canvas.height = this.state.height * scaledCell;
      this.canvas.style.width = `${this.canvas.width}px`;
      this.canvas.style.height = `${this.canvas.height}px`;
      context.clearRect(0, 0, this.canvas.width, this.canvas.height);
      const orderedLayers = this.state.layers.map((layer, index) => ({ layer, index })).sort((left, right) => left.layer.renderLayer - right.layer.renderLayer || left.index - right.index);
      for (const { layer, index: layerIndex } of orderedLayers) {
        if (!layer.visible)
          continue;
        context.globalAlpha = layerIndex === this.state.activeLayer ? 1 : 0.58;
        for (let y = 0;y < this.state.height; y += 1) {
          for (let x = 0;x < this.state.width; x += 1) {
            const gid = layer.tiles[tileIndex(this.state.width, x, y)];
            this.drawTile(context, gid, x * scaledCell, y * scaledCell, scaledCell, x, y, layerIndex);
          }
        }
      }
      context.globalAlpha = 1;
      for (let x = 0;x <= this.state.width; x += 1) {
        context.strokeStyle = x % 5 === 0 ? "rgba(255,255,255,0.22)" : "rgba(255,255,255,0.10)";
        context.beginPath();
        context.moveTo(x * scaledCell + 0.5, 0);
        context.lineTo(x * scaledCell + 0.5, this.canvas.height);
        context.stroke();
      }
      for (let y = 0;y <= this.state.height; y += 1) {
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
    drawTile(target, gid, dx, dy, size, cellX, cellY, layerIndex) {
      if (gid === EMPTY_GID || this.state.hiddenGids.has(gid))
        return;
      const tileset = findTilesetForGid(this.state.tilesets, gid);
      if (!tileset)
        return;
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
      if (!previewTileset.image)
        return;
      const sx = localId % previewTileset.cols * previewTileset.tileSize;
      const sy = Math.floor(localId / previewTileset.cols) * previewTileset.tileSize;
      target.drawImage(previewTileset.image, sx, sy, previewTileset.tileSize, previewTileset.tileSize, dx, dy, size, size);
    }
    removeTileset(id) {
      const tileset = this.state.tilesets.find((entry) => entry.id === id);
      if (!tileset || tileset.builtin)
        return;
      this.state.tilesets = this.state.tilesets.filter((entry) => entry.id !== id);
      for (let gid = tileset.firstGid;gid < tileset.firstGid + tileset.count; gid += 1) {
        this.state.hiddenGids.add(gid);
      }
      this.state.layers.forEach((layer) => {
        layer.tiles = layer.tiles.map((gid) => findTilesetForGid(this.state.tilesets, gid) ? gid : EMPTY_GID);
      });
      this.state.selectedTile = findTilesetForGid(this.state.tilesets, this.state.selectedTile)?.firstGid ?? 0;
      this.renderAll();
      this.setStatus(`已删除 ${tileset.name}`);
    }
    upsertVisual(tileset, nextVisual) {
      const index = tileset.visuals.findIndex((visual) => visual.gid === nextVisual.gid);
      if (nextVisual.kind === "static" && nextVisual.animation < 0 && nextVisual.speed === 1 && nextVisual.strength === 0 && nextVisual.phase === 0 && nextVisual.flags === 0) {
        if (index >= 0) {
          tileset.visuals.splice(index, 1);
        }
        return;
      }
      if (index >= 0)
        tileset.visuals[index] = nextVisual;
      else
        tileset.visuals.push(nextVisual);
    }
    ensureAnimation(tileset, gid) {
      let animation = tileset.animations.find((entry) => entry.baseGid === gid);
      if (!animation) {
        animation = { baseGid: gid, frames: [{ gid, duration: 0.12 }], randomStart: false, speed: 1 };
        tileset.animations.push(animation);
      }
      return animation;
    }
    rebindVisualAnimationIndex(tileset, gid) {
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
    upsertCollision(tileset, nextCollision) {
      const index = tileset.collisions.findIndex((collision) => collision.gid === nextCollision.gid);
      if (nextCollision.shape === "none") {
        if (index >= 0)
          tileset.collisions.splice(index, 1);
        return;
      }
      if (index >= 0)
        tileset.collisions[index] = nextCollision;
      else
        tileset.collisions.push(nextCollision);
    }
    toggleFullCollision(tileset, gid) {
      toggleFullCollision(tileset, gid);
      if (this.state.selectedTile === gid) {
        this.renderInspector();
      }
      this.renderMap();
      this.renderPalette();
    }
    parseNumericList(text) {
      return text.split(/[\s,]+/).map((token) => token.trim()).filter(Boolean).map((token) => Number.parseFloat(token)).filter((value) => Number.isFinite(value));
    }
    async loadDocument(payload) {
      const data = payload;
      if (data?.type === "qgame.tilemap.editor") {
        await this.loadEditorProject(data);
        return;
      }
      if (data?.type === "qgame.tilemap.engine-package") {
        await this.loadEnginePackage(data);
        return;
      }
      await this.loadEngineTileMap(data);
    }
    async loadEditorProject(project) {
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
        let image = null;
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
    async loadEnginePackage(pkg) {
      this.cancelLayerRename();
      const tileMap = pkg.tileMap;
      const resourcesByKey = new Map;
      for (const resource of pkg.resources ?? []) {
        const key = `${resource.id ?? ""}::${resource.firstGid ?? ""}`;
        if (resource.dataUrl) {
          resourcesByKey.set(key, resource.dataUrl);
        }
      }
      await this.loadEngineTileMap(tileMap, resourcesByKey);
    }
    async loadEngineTileMap(tileMap, resourcesByKey = new Map) {
      const state = createDefaultState();
      this.cancelLayerRename();
      state.width = clampInt(tileMap.w ?? state.width, 1, 512);
      state.height = clampInt(tileMap.h ?? state.height, 1, 512);
      state.tileSize = clampInt(tileMap.ts ?? state.tileSize, 4, 256);
      state.tilesets = [];
      state.hiddenGids = new Set;
      for (const spec of tileMap.tilesets ?? []) {
        const builtin = spec.sourceKind === "builtin" || spec.id === "builtin" || spec.firstGid === 0;
        const dataUrl = spec.sourceDataUrl || resourcesByKey.get(`${spec.id ?? ""}::${spec.firstGid}`) || "";
        const image = !builtin && dataUrl ? await this.loadImage(dataUrl) : null;
        const collisions = spec.collisions && spec.collisions.length > 0 ? spec.collisions : (spec.collision ?? []).flatMap((solid, localId) => solid ? [{
          gid: spec.firstGid + localId,
          shape: "full",
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
    async loadImage(src) {
      return await new Promise((resolve, reject) => {
        const image = new Image;
        image.onload = () => resolve(image);
        image.onerror = reject;
        image.src = src;
      });
    }
    async readFileAsDataUrl(file) {
      return await new Promise((resolve, reject) => {
        const reader = new FileReader;
        reader.onload = () => resolve(String(reader.result));
        reader.onerror = reject;
        reader.readAsDataURL(file);
      });
    }
    downloadJson(filename, payload) {
      const blob = new Blob([JSON.stringify(payload, null, 2)], { type: "application/json" });
      const url = URL.createObjectURL(blob);
      const link = document.createElement("a");
      link.href = url;
      link.download = filename;
      link.click();
      URL.revokeObjectURL(url);
      this.setStatus(`${filename} 已导出`);
    }
    setStatus(text) {
      this.statusElement.textContent = text;
    }
    commitLayerRename() {
      if (this.layerEditingIndex < 0)
        return;
      const layer = this.state.layers[this.layerEditingIndex];
      if (layer) {
        layer.name = this.layerEditingDraft.trim() || `Layer ${this.layerEditingIndex + 1}`;
      }
      this.layerEditingIndex = -1;
      this.layerEditingDraft = "";
      this.updateJsonOnly();
    }
    cancelLayerRename() {
      this.layerEditingIndex = -1;
      this.layerEditingDraft = "";
    }
    focusEditingLayerInput() {
      if (this.layerEditingIndex < 0)
        return;
      requestAnimationFrame(() => {
        const input = this.layersElement.querySelector(`.layer-name-input[data-layer-index="${this.layerEditingIndex}"]`);
        if (!input)
          return;
        input.focus();
        input.select();
      });
    }
  }
  new TilemapEditorApp;
})();

//# debugId=A86879418224FC6B64756E2164756E21
//# sourceMappingURL=main.js.map
