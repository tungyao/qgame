#!/usr/bin/env python3
"""
Bake msdf-atlas-gen output (PNG + JSON) into the engine's .font binary.

临时工具：本地一次性生成 .font 文件用于运行时验证。
将来由 C++ 编译阶段取代，本脚本不参与 CMake 构建。

用法：
  msdf-atlas-gen -font foo.ttf -charset charset.txt -type msdf -format png \
                 -imageout foo.png -json foo.json -size 32 -pxrange 4 -potr
  python3 bake_font.py foo.png foo.json foo.ttf.font
"""
import json
import struct
import sys
from PIL import Image


def bake(png_path: str, json_path: str, out_path: str) -> None:
    with open(json_path) as f:
        meta = json.load(f)

    atlas = meta["atlas"]
    assert atlas["type"] == "msdf", "expected msdf atlas"
    assert atlas["yOrigin"] in ("bottom", "top")

    atlas_w = int(atlas["width"])
    atlas_h = int(atlas["height"])
    font_size = float(atlas["size"])
    px_range = float(atlas["distanceRange"])
    y_from_bottom = atlas["yOrigin"] == "bottom"

    m = meta["metrics"]
    line_height = float(m["lineHeight"]) * font_size
    baseline = float(m["ascender"]) * font_size

    glyphs = []
    for g in meta["glyphs"]:
        cp = int(g["unicode"])
        advance_px = float(g["advance"]) * font_size
        pb = g.get("planeBounds")
        ab = g.get("atlasBounds")
        if pb and ab:
            width_px = (pb["right"] - pb["left"]) * font_size
            height_px = (pb["top"] - pb["bottom"]) * font_size
            bearing_x_px = pb["left"] * font_size
            bearing_y_px = pb["top"] * font_size  # baseline→顶端，向上为正
            u0 = ab["left"] / atlas_w
            u1 = ab["right"] / atlas_w
            if y_from_bottom:
                v0 = (atlas_h - ab["top"]) / atlas_h
                v1 = (atlas_h - ab["bottom"]) / atlas_h
            else:
                v0 = ab["top"] / atlas_h
                v1 = ab["bottom"] / atlas_h
        else:
            # 无外形（空格等）
            width_px = height_px = bearing_x_px = bearing_y_px = 0.0
            u0 = v0 = u1 = v1 = 0.0
        glyphs.append((cp, u0, v0, u1, v1, width_px, height_px,
                       bearing_x_px, bearing_y_px, advance_px))

    img = Image.open(png_path).convert("RGBA")
    assert img.size == (atlas_w, atlas_h), "png/json size mismatch"
    pixels = img.tobytes()

    with open(out_path, "wb") as out:
        out.write(b"FONT")
        out.write(struct.pack("<I", 1))                # version
        out.write(struct.pack("<II", atlas_w, atlas_h))
        out.write(struct.pack("<ffff", font_size, px_range, line_height, baseline))
        out.write(struct.pack("<I", len(glyphs)))
        for g in glyphs:
            out.write(struct.pack("<I" + "f" * 9, *g))
        out.write(pixels)

    print(f"wrote {out_path}: {atlas_w}x{atlas_h}, {len(glyphs)} glyphs")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(f"usage: {sys.argv[0]} <png> <json> <out.font>")
    bake(sys.argv[1], sys.argv[2], sys.argv[3])
