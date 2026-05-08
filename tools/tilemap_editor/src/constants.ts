/**
 * Built-in palette used when the editor starts without importing an external
 * atlas. Keeping this in a dedicated module makes it easy to reuse from schema
 * and preview code without coupling those paths to DOM state.
 */
export const BUILTIN_COLORS = [
  "#5a9f4d", "#6fb85f", "#a47b48", "#7f5f3b",
  "#4a8cbf", "#3d76a3", "#c9b36a", "#d9cf8f",
  "#6d737a", "#89919a", "#9c5d4e", "#bd7a69",
  "#2f6f43", "#255838", "#b7b7b7", "#e3e3e3"
] as const;

export const EMPTY_GID = -1;
export const DEFAULT_MAP_WIDTH = 20;
export const DEFAULT_MAP_HEIGHT = 15;
export const DEFAULT_TILE_SIZE = 32;
