#!/usr/bin/env python3
"""contact_sheet.py - tile labelled thumbnails of rendered goldens into contact sheets.

    contact_sheet.py [--scripts tests/scripts] [--out tests/output] [--source auto|cpp|ref]
                     [--ref tests/reference/refcomp.py] [--python python3] [--tile 128]
                     [--cols N]

Writes into <out>/sheets/:
  blend_modes.png   the 27 blend modes (rows, doc-10 table order) x 4 alpha configs
                    (columns opq, op50, bda, rnd), from goldens named blend_<mode>_<cfg>
  <domain>.png      one sheet per top-level directory of --scripts (e.g. compositing.png)

Image source per golden (expect-error goldens are skipped):
  cpp   <out>/<name>/cpp.png (kept by run_goldens --keep-all or on failure)
  ref   the reference cache <out>/.refcache/ that run_goldens fills (same key)
  auto  cpp if present, else ref (default)
A golden with no image gets a grey "missing" tile. Transparent pixels are shown over a
checkerboard. Tiles are fitted into --tile px (box filter down, nearest up).
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

TOOLS_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS_DIR))
import run_goldens as rg  # noqa: E402

BLEND_MODES = ["norm", "diss", "dark", "mul", "idiv", "lbrn", "dkCl", "lite", "scrn", "div",
               "lddg", "lgCl", "over", "sLit", "hLit", "vLit", "lLit", "pLit", "hMix", "diff",
               "smud", "fsub", "fdiv", "hue", "sat", "colr", "lum"]
BLEND_CFGS = ["opq", "op50", "bda", "rnd"]
BLEND_RE = re.compile(r"(?:^|/)blend_([A-Za-z]+)_(opq|op50|bda|rnd)$")
LABEL_H = 14
PAD = 4


def _font(size: int = 11):
    try:
        return ImageFont.load_default(size=size)
    except TypeError:
        return ImageFont.load_default()


def checker(w: int, h: int, cell: int = 8) -> Image.Image:
    im = Image.new("RGBA", (w, h), (255, 255, 255, 255))
    d = ImageDraw.Draw(im)
    for y in range(0, h, cell):
        for x in range(0, w, cell):
            if ((x // cell) + (y // cell)) % 2:
                d.rectangle([x, y, x + cell - 1, y + cell - 1], fill=(204, 204, 204, 255))
    return im


def thumb(path: Path | None, tile: int) -> Image.Image:
    bg = checker(tile, tile)
    if path is None:
        im = Image.new("RGBA", (tile, tile), (90, 90, 90, 255))
        ImageDraw.Draw(im).text((6, tile // 2 - 6), "missing", fill=(255, 255, 255, 255),
                                font=_font())
        return im
    with Image.open(path) as src:
        src = src.convert("RGBA")
        w, h = src.size
        s = min(tile / w, tile / h)
        nw, nh = max(1, round(w * s)), max(1, round(h * s))
        flt = Image.Resampling.NEAREST if s >= 1 else Image.Resampling.BOX
        src = src.resize((nw, nh), flt)
    ox, oy = (tile - nw) // 2, (tile - nh) // 2
    region = bg.crop((ox, oy, ox + nw, oy + nh))
    bg.paste(Image.alpha_composite(region, src), (ox, oy))
    return bg


def find_image(name: str, item: dict, args, code_hash: str) -> Path | None:
    out = Path(args.out)
    cpp = out / name / "cpp.png"
    if args.source in ("auto", "cpp") and cpp.exists():
        return cpp
    if args.source in ("auto", "ref"):
        key = rg.cache_key(Path(item["path"]).read_bytes(), code_hash, args.python)
        p = out / ".refcache" / f"{key}.png"
        if p.exists():
            return p
        kept = out / name / "ref.png"
        if kept.exists():
            return kept
    return None


def draw_grid(cells: list[tuple[str, Path | None]], cols: int, tile: int, title: str,
              row_labels: list[str] | None = None, col_labels: list[str] | None = None
              ) -> Image.Image:
    font = _font()
    rows = (len(cells) + cols - 1) // cols
    left = 44 if row_labels else 0
    top = 22 + (LABEL_H if col_labels else 0)
    cw, ch = tile + PAD, tile + LABEL_H + PAD
    sheet = Image.new("RGBA", (left + cols * cw + PAD, top + rows * ch + PAD),
                      (40, 40, 40, 255))
    d = ImageDraw.Draw(sheet)
    d.text((PAD, 4), title, fill=(255, 255, 255, 255), font=font)
    if col_labels:
        for c, lab in enumerate(col_labels):
            d.text((left + PAD + c * cw, 22), lab, fill=(230, 230, 150, 255), font=font)
    for i, (label, path) in enumerate(cells):
        r, c = divmod(i, cols)
        x, y = left + PAD + c * cw, top + PAD + r * ch
        sheet.paste(thumb(path, tile), (x, y))
        text = label
        while len(text) > 3 and d.textlength(text, font=font) > tile:
            text = text[:-2] + "~"
        d.text((x, y + tile + 1), text, fill=(220, 220, 220, 255), font=font)
        if row_labels and c == 0:
            d.text((4, y + tile // 2 - 6), row_labels[r], fill=(230, 230, 150, 255), font=font)
    return sheet


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__.split("\n", 1)[1])
    ap.add_argument("--scripts", default=str(rg.REPO / "tests/scripts"))
    ap.add_argument("--out", default=str(rg.REPO / "tests/output"))
    ap.add_argument("--source", choices=["auto", "cpp", "ref"], default="auto")
    ap.add_argument("--ref", default=str(rg.REPO / "tests/reference/refcomp.py"))
    ap.add_argument("--python", default=sys.executable)
    ap.add_argument("--tile", type=int, default=128)
    ap.add_argument("--cols", type=int, default=10)
    args = ap.parse_args(argv)

    items = [i for i in rg.discover(Path(args.scripts), None, []) if not i["expect_error"]]
    ref_p = Path(args.ref)
    code_hash = rg.ref_code_hash(ref_p) if ref_p.exists() else "missing"
    sheets = Path(args.out) / "sheets"
    sheets.mkdir(parents=True, exist_ok=True)
    found = {i["name"]: find_image(i["name"], i, args, code_hash) for i in items}
    written, missing = [], sum(1 for v in found.values() if v is None)

    blend = {}
    for i in items:
        m = BLEND_RE.search(i["name"])
        if m:
            blend[(m.group(1), m.group(2))] = i["name"]
    if blend:
        cells = []
        for mode in BLEND_MODES:
            for cfg in BLEND_CFGS:
                n = blend.get((mode, cfg))
                cells.append((f"{mode}_{cfg}", found.get(n) if n else None))
        im = draw_grid(cells, 4, args.tile, "27 blend modes x 4 alpha configs (doc 10 §12.1)",
                       row_labels=BLEND_MODES, col_labels=BLEND_CFGS)
        p = sheets / "blend_modes.png"
        im.save(p)
        written.append(p)

    domains: dict[str, list[dict]] = {}
    for i in items:
        dom = i["name"].split("/", 1)[0] if "/" in i["name"] else "_top"
        domains.setdefault(dom, []).append(i)
    for dom, its in sorted(domains.items()):
        cells = [(i["name"].rsplit("/", 1)[-1], found[i["name"]]) for i in its]
        tile = min(args.tile, 96) if len(cells) > 40 else args.tile
        cols = max(args.cols, 12) if len(cells) > 40 else args.cols
        im = draw_grid(cells, cols, tile, f"{dom}: {len(cells)} goldens")
        p = sheets / f"{dom}.png"
        im.save(p)
        written.append(p)

    for p in written:
        print(p)
    print(f"{len(items)} goldens, {len(items) - missing} with images, {missing} missing")
    return 0


if __name__ == "__main__":
    sys.exit(main())
