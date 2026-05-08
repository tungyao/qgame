/**
 * Bun build entry for the standalone TileMap editor.
 *
 * The editor intentionally avoids framework dependencies. Bun is only used as
 * the project build tool so the source can stay split across small TS/CSS files
 * while still producing a browser-ready bundle in ./dist.
 */
const watchMode = Bun.argv.includes("--watch");

const result = await Bun.build({
  entrypoints: ["./src/main.ts", "./src/styles.css"],
  outdir: "./dist",
  target: "browser",
  format: "iife",
  sourcemap: "linked",
  naming: {
    entry: "[name].[ext]"
  },
  minify: false,
  watch: watchMode
});

if (!result.success) {
  for (const log of result.logs) {
    console.error(log);
  }
  process.exit(1);
}

console.log(`[tilemap-editor] built ${result.outputs.length} artifact(s)${watchMode ? " in watch mode" : ""}`);
