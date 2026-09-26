#!/usr/bin/env python3
"""pixcmp.py - exact RGBA8 image comparison (local equivalent of `odiff --threshold=0`).

    pixcmp.py A.png B.png [--diff D.png] [--quiet]

Both images are decoded with Pillow and converted to 8-bit RGBA. Exit codes:
    0  identical size and identical RGBA bytes
    1  different size or at least one differing byte (details printed, diff mask written)
    2  an image could not be read/decoded

The diff mask (only written on a mismatch, and only when --diff is given) is an RGBA PNG the size
of the union of both images: differing pixels are opaque red (255,0,0,255); identical pixels are
image A's pixel as grey at quarter brightness, opaque, so the location of the difference is
visible in context. On a size mismatch the area covered by only one image is magenta.

Also importable: `compare(a_path, b_path, diff_path=None) -> dict`.
"""
from __future__ import annotations

import argparse
import sys

import numpy as np
from PIL import Image


def load_rgba(path: str) -> np.ndarray:
    """Decode an image file to an (H, W, 4) uint8 array."""
    with Image.open(path) as im:
        im.load()
        if im.mode != "RGBA":
            im = im.convert("RGBA")
        return np.asarray(im, dtype=np.uint8).copy()


def _write_diff(a: np.ndarray, b: np.ndarray, diffmask: np.ndarray | None, path: str) -> None:
    h = max(a.shape[0], b.shape[0])
    w = max(a.shape[1], b.shape[1])
    out = np.zeros((h, w, 4), dtype=np.uint8)
    out[..., 0] = 255
    out[..., 2] = 255
    out[..., 3] = 255  # magenta where only one image has pixels
    ch = min(a.shape[0], b.shape[0])
    cw = min(a.shape[1], b.shape[1])
    ac = a[:ch, :cw].astype(np.uint32)
    grey = ((ac[..., 0] * 30 + ac[..., 1] * 59 + ac[..., 2] * 11) // 100) // 4
    region = out[:ch, :cw]
    region[..., 0] = grey
    region[..., 1] = grey
    region[..., 2] = grey
    region[..., 3] = 255
    if diffmask is None:
        diffmask = np.any(a[:ch, :cw] != b[:ch, :cw], axis=-1)
    region[diffmask] = (255, 0, 0, 255)
    Image.fromarray(out, "RGBA").save(path)


def compare(a_path: str, b_path: str, diff_path: str | None = None, max_list: int = 10) -> dict:
    """Compare two images. Returns a dict with keys:
    identical (bool), size_a, size_b (w, h), mismatched_pixels, max_delta, max_delta_per_channel,
    first (list of (x, y, a_rgba, b_rgba)), error (str or None)."""
    res = {
        "identical": False,
        "size_a": None,
        "size_b": None,
        "mismatched_pixels": 0,
        "max_delta": 0,
        "max_delta_per_channel": [0, 0, 0, 0],
        "first": [],
        "error": None,
    }
    try:
        a = load_rgba(a_path)
    except Exception as e:  # noqa: BLE001 - report any decode failure
        res["error"] = f"cannot read {a_path}: {e}"
        return res
    try:
        b = load_rgba(b_path)
    except Exception as e:  # noqa: BLE001
        res["error"] = f"cannot read {b_path}: {e}"
        return res
    res["size_a"] = (int(a.shape[1]), int(a.shape[0]))
    res["size_b"] = (int(b.shape[1]), int(b.shape[0]))
    if a.shape != b.shape:
        ch = min(a.shape[0], b.shape[0])
        cw = min(a.shape[1], b.shape[1])
        overlap = np.any(a[:ch, :cw] != b[:ch, :cw], axis=-1)
        res["mismatched_pixels"] = int(overlap.sum()) + (a.shape[0] * a.shape[1] - ch * cw) \
            + (b.shape[0] * b.shape[1] - ch * cw)
        res["size_mismatch"] = True
        if diff_path:
            _write_diff(a, b, overlap, diff_path)
        return res
    if np.array_equal(a, b):
        res["identical"] = True
        return res
    delta = np.abs(a.astype(np.int16) - b.astype(np.int16))
    per_ch = delta.reshape(-1, 4).max(axis=0)
    mask = np.any(a != b, axis=-1)
    res["mismatched_pixels"] = int(mask.sum())
    res["max_delta_per_channel"] = [int(v) for v in per_ch]
    res["max_delta"] = int(per_ch.max())
    ys, xs = np.nonzero(mask)  # row-major order: first differing pixels by (y, x)
    for y, x in zip(ys[:max_list], xs[:max_list]):
        res["first"].append((int(x), int(y), tuple(int(v) for v in a[y, x]),
                             tuple(int(v) for v in b[y, x])))
    if diff_path:
        _write_diff(a, b, mask, diff_path)
    return res


def format_report(res: dict, a_name: str = "A", b_name: str = "B") -> str:
    if res["error"]:
        return f"ERROR: {res['error']}"
    if res["identical"]:
        w, h = res["size_a"]
        return f"identical ({w}x{h})"
    lines = []
    if res.get("size_mismatch"):
        lines.append(f"SIZE MISMATCH: {a_name} {res['size_a'][0]}x{res['size_a'][1]} vs "
                     f"{b_name} {res['size_b'][0]}x{res['size_b'][1]}; "
                     f"{res['mismatched_pixels']} pixels differ or are unmatched")
        return "\n".join(lines)
    w, h = res["size_a"]
    lines.append(f"MISMATCH: {res['mismatched_pixels']} of {w * h} pixels differ; max per-channel "
                 f"delta {res['max_delta']} (R,G,B,A = {tuple(res['max_delta_per_channel'])})")
    for x, y, pa, pb in res["first"]:
        lines.append(f"  ({x},{y}) {a_name}={pa} {b_name}={pb}")
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Exact RGBA8 image comparison (odiff --threshold=0).")
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--diff", metavar="D.png", help="write a diff mask PNG on mismatch")
    ap.add_argument("--quiet", action="store_true", help="print nothing, exit code only")
    args = ap.parse_args(argv)
    res = compare(args.a, args.b, args.diff)
    if not args.quiet:
        print(format_report(res, "a", "b"))
    if res["error"]:
        return 2
    return 0 if res["identical"] else 1


if __name__ == "__main__":
    sys.exit(main())
