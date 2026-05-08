import type { EditorLayer } from "./types";

/**
 * LayerPanelState keeps transient rename state outside the persistent tilemap
 * data. This prevents unrelated re-renders from stomping on in-progress text.
 */
export interface LayerPanelState {
  activeLayer: number;
  editingLayer: number;
  draftName: string;
}

/**
 * LayerPanelCallbacks keeps all mutations in the editor shell while this module
 * focuses on DOM composition for the layer panel.
 */
export interface LayerPanelCallbacks {
  onActivate(index: number): void;
  onBeginRename(index: number): void;
  onDraftRename(index: number, value: string): void;
  onCommitRename(index: number): void;
  onCancelRename(index: number): void;
  onToggleVisible(index: number): void;
  onToggleCollidable(index: number): void;
  onRenderLayerChange(index: number, value: string): void;
  onDelete(index: number): void;
}

/**
 * renderLayerPanel rebuilds the entire layer list from state. The active rename
 * draft is preserved through LayerPanelState so list refreshes stay stable.
 */
export function renderLayerPanel(
  container: HTMLElement,
  layers: EditorLayer[],
  panelState: LayerPanelState,
  callbacks: LayerPanelCallbacks
): void {
  container.innerHTML = "";

  layers.forEach((layer, index) => {
    const row = document.createElement("div");
    row.className = `layer-row${index === panelState.activeLayer ? " active" : ""}`;

    row.appendChild(
      index === panelState.editingLayer
        ? createEditingNameInput(index, panelState.draftName, callbacks)
        : createStaticNameLabel(layer.name)
    );

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

    controls.append(
      activateButton,
      renameButton,
      visibleButton,
      collidableButton,
      renderLayerInput,
      deleteButton
    );
    row.appendChild(controls);
    container.appendChild(row);
  });
}

function createStaticNameLabel(name: string): HTMLDivElement {
  const label = document.createElement("div");
  label.className = "layer-name-label";
  label.textContent = name;
  return label;
}

function createEditingNameInput(
  index: number,
  value: string,
  callbacks: LayerPanelCallbacks
): HTMLInputElement {
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
