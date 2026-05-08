/**
 * Small Bun dev server for the standalone editor.
 *
 * The editor can still be opened directly from the filesystem after build, but
 * a local server avoids browser quirks around local assets and keeps one
 * predictable way to test the modular project.
 */
const projectRoot = new URL(".", import.meta.url);
const fallbackPath = "index.html";

function contentType(pathname: string): string {
  if (pathname.endsWith(".html")) return "text/html; charset=utf-8";
  if (pathname.endsWith(".css")) return "text/css; charset=utf-8";
  if (pathname.endsWith(".js")) return "application/javascript; charset=utf-8";
  if (pathname.endsWith(".map")) return "application/json; charset=utf-8";
  return "application/octet-stream";
}

Bun.serve({
  port: 3000,
  development: true,
  async fetch(request) {
    const url = new URL(request.url);
    const normalizedPath = url.pathname === "/" ? fallbackPath : url.pathname.replace(/^\/+/, "");
    const file = Bun.file(new URL(normalizedPath, projectRoot));
    if (await file.exists()) {
      return new Response(file, {
        headers: {
          "content-type": contentType(normalizedPath)
        }
      });
    }
    const fallbackFile = Bun.file(new URL(fallbackPath, projectRoot));
    return new Response(fallbackFile, {
      headers: {
        "content-type": "text/html; charset=utf-8"
      }
    });
  }
});

console.log("[tilemap-editor] dev server on http://127.0.0.1:3000");
