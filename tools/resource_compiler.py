#!/usr/bin/env python3
"""
QGame resource compiler.

Phase 2/4 goals:
  - Read the source manifest maintained by developers.
  - Produce a baked manifest that runtime code can load directly.
  - Copy or bake assets into a build-owned asset root.
  - Keep a content-hash cache so unchanged assets are skipped.

This tool intentionally keeps the output layout close to the source layout.
That makes current runtime code simple, while leaving clean extension points:
  - Phase 5 can replace copy_file() with bundle writing and keep the manifest IDs.
  - Phase 6 can watch the same dependency list and re-run one asset at a time.
  - Phase 7 can surface cache records in an editor diagnostics panel.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import struct
from pathlib import Path
from typing import Any


TOOL_VERSION = 2
PACK_MAGIC = b"QPAK"
PACK_VERSION = 1


def read_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as f:
        return json.load(f)


def write_json(path: Path, data: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as f:
        json.dump(data, f, indent=2, sort_keys=True)
        f.write("\n")


def file_sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def normalize_rel(path: Path) -> str:
    return path.as_posix()


def resolve_source(manifest_dir: Path, raw: str) -> Path:
    p = Path(raw)
    if p.is_absolute():
        return p
    return (manifest_dir / p).resolve()


def rel_to_manifest(manifest_dir: Path, source: Path) -> Path:
    try:
        return source.resolve().relative_to(manifest_dir.resolve())
    except ValueError:
        # External source files are mirrored by basename. This keeps the baked
        # manifest relocatable and avoids leaking absolute paths into runtime data.
        return Path("_external") / source.name


def cache_key(record: dict[str, Any], rel_source: Path) -> str:
    return f"{record.get('type', 'unknown')}:{record.get('id', '')}:{normalize_rel(rel_source)}"


def load_cache(path: Path) -> dict[str, Any]:
    if not path.exists():
        return {"version": TOOL_VERSION, "assets": {}}
    try:
        data = read_json(path)
    except Exception:
        return {"version": TOOL_VERSION, "assets": {}}
    if data.get("version") != TOOL_VERSION or not isinstance(data.get("assets"), dict):
        return {"version": TOOL_VERSION, "assets": {}}
    return data


def dependency_hash(paths: list[Path], extra: dict[str, Any]) -> str:
    h = hashlib.sha256()
    h.update(json.dumps(extra, sort_keys=True).encode("utf-8"))
    for path in paths:
        h.update(str(path).encode("utf-8"))
        if path.exists():
            h.update(file_sha256(path).encode("ascii"))
        else:
            h.update(b"<missing>")
    return h.hexdigest()


def copy_if_needed(src: Path, dst: Path) -> None:
    dst.parent.mkdir(parents=True, exist_ok=True)
    if dst.exists() and src.exists() and file_sha256(src) == file_sha256(dst):
        return
    shutil.copy2(src, dst)


def region_id_sibling_path(path: Path) -> Path:
    return path.with_suffix("").with_name(path.with_suffix("").name + ".id.png")


def pack_path_uri(pack_id: str, rel_path: str) -> str:
    return f"pak://{pack_id}/{rel_path}"


def write_pack(pack_path: Path, out_dir: Path, rel_paths: list[str]) -> None:
    pack_path.parent.mkdir(parents=True, exist_ok=True)
    entries: list[dict[str, Any]] = []
    offset = 0
    unique_paths = sorted(set(rel_paths))

    with pack_path.open("wb") as f:
        for rel in unique_paths:
            src = out_dir / Path(rel)
            if not src.exists():
                raise RuntimeError(f"pack input missing: {src}")
            data = src.read_bytes()
            f.write(data)
            entries.append({
                "path": rel,
                "offset": offset,
                "size": len(data),
                "sha256": hashlib.sha256(data).hexdigest(),
            })
            offset += len(data)

        index = {
            "version": PACK_VERSION,
            "files": entries,
        }
        index_bytes = json.dumps(index, separators=(",", ":"), sort_keys=True).encode("utf-8")
        f.write(index_bytes)
        f.write(struct.pack("<Q", len(index_bytes)))
        f.write(PACK_MAGIC)


def copy_region_id_sibling(src: Path, dst: Path) -> None:
    # AssetManager auto-loads "<texture>.id.png"; keep that convention intact
    # in the baked output so region tinting works without extra manifest syntax.
    src_id = region_id_sibling_path(src)
    if not src_id.exists():
        return
    dst_id = region_id_sibling_path(dst)
    copy_if_needed(src_id, dst_id)


def maybe_copy_animation_image(src_json: Path, dst_json: Path) -> list[Path]:
    deps = [src_json]
    try:
        data = read_json(src_json)
    except Exception:
        return deps
    image = data.get("meta", {}).get("image")
    if not image:
        return deps
    src_img = (src_json.parent / image).resolve()
    dst_img = dst_json.parent / image
    if src_img.exists():
        copy_if_needed(src_img, dst_img)
        copy_region_id_sibling(src_img, dst_img)
        deps.append(src_img)
    return deps


def animation_output_paths(src_json: Path, dst_json: Path) -> list[Path]:
    outputs = [dst_json]
    try:
        data = read_json(src_json)
    except Exception:
        return outputs
    image = data.get("meta", {}).get("image")
    if not image:
        return outputs
    src_img = (src_json.parent / image).resolve()
    dst_img = dst_json.parent / image
    if src_img.exists():
        outputs.append(dst_img)
        src_id = region_id_sibling_path(src_img)
        if src_id.exists():
            outputs.append(region_id_sibling_path(dst_img))
    return outputs


def run_font_bake(args: argparse.Namespace, src_font: Path, dst_font_source: Path, deps: list[Path]) -> None:
    dst_font_source.parent.mkdir(parents=True, exist_ok=True)
    dst_bin = Path(str(dst_font_source) + ".font")

    source_bin = Path(str(src_font) + ".font")
    source_png = src_font.with_suffix(".png")
    source_json = src_font.with_suffix(".json")

    if source_bin.exists():
        # Fast path for repositories that already commit baked fonts. The
        # compiler still copies it into the build asset root and records hashes,
        # so Phase 4 incremental behavior is identical to generated output.
        copy_if_needed(source_bin, dst_bin)
        deps.append(source_bin)
        return

    if source_png.exists() and source_json.exists():
        bake_script = Path(__file__).with_name("bake_font.py")
        subprocess.check_call([sys.executable, str(bake_script), str(source_png), str(source_json), str(dst_bin)])
        deps.extend([source_png, source_json])
        return

    if not args.msdf_atlas_gen:
        raise RuntimeError(
            f"font {src_font} has no .font or .png/.json siblings; pass --msdf-atlas-gen to bake from TTF"
        )

    # Full auto-bake path. Output png/json are intermediate files kept beside
    # the baked font for diagnostics and future hot-reload tooling.
    dst_png = dst_font_source.with_suffix(".png")
    dst_json = dst_font_source.with_suffix(".json")
    subprocess.check_call([
        args.msdf_atlas_gen,
        "-font", str(src_font),
        "-type", "msdf",
        "-format", "png",
        "-imageout", str(dst_png),
        "-json", str(dst_json),
        "-size", str(args.font_size),
        "-pxrange", str(args.font_pxrange),
        "-potr",
    ])
    bake_script = Path(__file__).with_name("bake_font.py")
    subprocess.check_call([sys.executable, str(bake_script), str(dst_png), str(dst_json), str(dst_bin)])
    deps.extend([dst_png, dst_json])


def compile_asset(
    args: argparse.Namespace,
    manifest_dir: Path,
    out_dir: Path,
    record: dict[str, Any],
    cache: dict[str, Any],
) -> dict[str, Any]:
    asset_id = record.get("id", "")
    asset_type = record.get("type", "")
    source_raw = record.get("source", "")
    if not asset_id or not asset_type or not source_raw:
        raise RuntimeError(f"invalid asset record: {record}")

    src = resolve_source(manifest_dir, source_raw)
    rel_src = rel_to_manifest(manifest_dir, src)
    dst = out_dir / rel_src
    key = cache_key(record, rel_src)
    deps = [src]

    extra = {
        "tool": TOOL_VERSION,
        "type": asset_type,
        "id": asset_id,
        "fontSize": args.font_size,
        "fontPxRange": args.font_pxrange,
    }

    # Discover dependency set before cache comparison where possible.
    if asset_type == "texture":
        sibling = region_id_sibling_path(src)
        if sibling.exists():
            deps.append(sibling)
    elif asset_type == "animation":
        deps = maybe_copy_animation_image(src, dst)
    elif asset_type == "font":
        for p in [Path(str(src) + ".font"), src.with_suffix(".png"), src.with_suffix(".json")]:
            if p.exists():
                deps.append(p)

    digest = dependency_hash(deps, extra)
    previous = cache.get("assets", {}).get(key, {})
    outputs = [dst]
    if asset_type == "texture":
        sibling = region_id_sibling_path(src)
        if sibling.exists():
            outputs.append(region_id_sibling_path(dst))
    elif asset_type == "animation":
        outputs = animation_output_paths(src, dst)
    elif asset_type == "font":
        outputs.append(Path(str(dst) + ".font"))

    cache_hit = previous.get("hash") == digest and all(p.exists() for p in outputs)

    if not cache_hit:
        if asset_type == "texture":
            copy_if_needed(src, dst)
            copy_region_id_sibling(src, dst)
        elif asset_type == "sound":
            copy_if_needed(src, dst)
        elif asset_type == "animation":
            copy_if_needed(src, dst)
            maybe_copy_animation_image(src, dst)
        elif asset_type == "font":
            # Runtime loadFont("x.ttf") expects "x.ttf.font" beside it, so the
            # baked manifest points at the copied TTF path and the compiler
            # ensures the binary sibling exists.
            copy_if_needed(src, dst)
            run_font_bake(args, src, dst, deps)
        else:
            raise RuntimeError(f"unknown asset type for {asset_id}: {asset_type}")

    cache["assets"][key] = {
        "hash": digest,
        "outputs": [normalize_rel(p.relative_to(out_dir)) for p in outputs],
        "dependencies": [str(p) for p in deps],
    }

    baked = dict(record)
    baked_rel = normalize_rel(rel_src)
    baked["baked"] = pack_path_uri(args.pack_id, baked_rel) if args.pack else baked_rel
    baked["_packOutputs"] = cache["assets"][key]["outputs"]
    return baked


def collect_pack_outputs(records: list[dict[str, Any]]) -> list[str]:
    paths: list[str] = []
    for record in records:
        for rel in record.get("_packOutputs", []):
            paths.append(str(rel))
    return paths


def strip_internal_fields(records: list[dict[str, Any]]) -> list[dict[str, Any]]:
    result: list[dict[str, Any]] = []
    for record in records:
        clean = dict(record)
        clean.pop("_packOutputs", None)
        result.append(clean)
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description="Compile QGame assets into a baked manifest.")
    parser.add_argument("--manifest", required=True, help="Source manifest.json")
    parser.add_argument("--out-dir", required=True, help="Baked asset root")
    parser.add_argument("--cache", default="", help="Incremental cache path")
    parser.add_argument("--msdf-atlas-gen", default="", help="Optional msdf-atlas-gen executable")
    parser.add_argument("--font-size", type=int, default=32)
    parser.add_argument("--font-pxrange", type=int, default=4)
    parser.add_argument("--pack", default="", help="Optional output .qpak path")
    parser.add_argument("--pack-id", default="main", help="Pack id used in pak:// URIs")
    args = parser.parse_args()

    manifest = Path(args.manifest).resolve()
    manifest_dir = manifest.parent
    out_dir = Path(args.out_dir).resolve()
    cache_path = Path(args.cache).resolve() if args.cache else out_dir / ".assetcache.json"

    source = read_json(manifest)
    if not isinstance(source.get("assets"), list):
        raise RuntimeError(f"manifest missing assets array: {manifest}")

    cache = load_cache(cache_path)
    baked_assets = [
        compile_asset(args, manifest_dir, out_dir, record, cache)
        for record in source["assets"]
    ]

    if args.pack:
        pack_path = Path(args.pack).resolve()
        write_pack(pack_path, out_dir, collect_pack_outputs(baked_assets))

    baked_manifest = {
        "version": source.get("version", 1),
        "pipelineVersion": TOOL_VERSION,
        "assets": strip_internal_fields(baked_assets),
    }
    if args.pack:
        try:
            pack_rel = pack_path.relative_to(out_dir)
        except ValueError:
            pack_rel = pack_path
        baked_manifest["packs"] = [{
            "id": args.pack_id,
            "path": normalize_rel(pack_rel),
        }]

    write_json(out_dir / "manifest.baked.json", baked_manifest)
    write_json(cache_path, cache)
    print(f"[resource_compiler] wrote {out_dir / 'manifest.baked.json'} ({len(baked_assets)} assets)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
