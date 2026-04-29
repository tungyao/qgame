#!/usr/bin/env python3
"""
bake_region_id.py — 把"鲜艳标记图"转成引擎使用的 R8 region ID 图。

工作流：
  1. 美术在 Aseprite/Photoshop 用饱和原色画一张标记图（每个部位一种颜色）
     例：上衣纯红 #FF0000、裤子纯绿 #00FF00、头发纯蓝 #0000FF...
  2. 写一份 mapping.json 描述颜色→ID 对应关系
  3. 跑这个脚本，输出 <name>.id.png（R8 单通道，引擎自动加载 sibling）

mapping.json 格式：
  {
    "FF0000": 1,    // 皮肤
    "00FF00": 2,    // 头发
    "0000FF": 3,    // 上衣
    "FFFF00": 4     // 裤子
  }
  (键是 6 位十六进制 RGB；忽略大小写和 "#"。透明像素 / 未列出的颜色 = 0 = 背景)

用法：
  python tools/bake_region_id.py <marker.png> <mapping.json> [<out.id.png>]
若省略 out 路径，默认 <marker 去后缀>.id.png

引擎约定：ID 0 = 不染色，ID 1..15 = 可染色部位（Tinting::MAX_REGIONS = 16）。
"""
import json
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    print("error: 需要 Pillow。pip install pillow", file=sys.stderr)
    sys.exit(1)


def parse_hex(key: str) -> tuple[int, int, int]:
    s = key.strip().lstrip("#").lower()
    if len(s) != 6:
        raise ValueError(f"invalid color key: {key!r}")
    return int(s[0:2], 16), int(s[2:4], 16), int(s[4:6], 16)


def main(argv: list[str]) -> int:
    if len(argv) < 3 or len(argv) > 4:
        print(__doc__, file=sys.stderr)
        return 2

    marker_path = Path(argv[1])
    map_path    = Path(argv[2])
    if len(argv) == 4:
        out_path = Path(argv[3])
    else:
        # marker.png → marker.id.png  (保留原后缀前的部分)
        out_path = marker_path.with_suffix("").with_suffix(".id.png")

    raw = json.loads(map_path.read_text())
    color_to_id: dict[tuple[int, int, int], int] = {}
    for k, v in raw.items():
        rgb = parse_hex(k)
        if not (0 <= int(v) <= 255):
            raise ValueError(f"id {v} out of range 0..255 (key {k!r})")
        if int(v) >= 16:
            print(f"warn: id {v} >= MAX_REGIONS(16)，引擎 shader 会忽略", file=sys.stderr)
        color_to_id[rgb] = int(v)

    img = Image.open(marker_path).convert("RGBA")
    w, h = img.size
    src = img.load()
    out = Image.new("L", (w, h))
    dst = out.load()

    unmapped: dict[tuple[int, int, int], int] = {}
    for y in range(h):
        for x in range(w):
            r, g, b, a = src[x, y]
            if a == 0:
                dst[x, y] = 0
                continue
            key = (r, g, b)
            if key in color_to_id:
                dst[x, y] = color_to_id[key]
            else:
                dst[x, y] = 0
                unmapped[key] = unmapped.get(key, 0) + 1

    out.save(out_path)
    print(f"wrote {out_path}  ({w}x{h}, {len(color_to_id)} regions)")
    if unmapped:
        print(f"warn: {sum(unmapped.values())} 像素颜色未映射（前 5 种）：", file=sys.stderr)
        for k, n in sorted(unmapped.items(), key=lambda kv: -kv[1])[:5]:
            print(f"  #{k[0]:02X}{k[1]:02X}{k[2]:02X}  {n} px", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
