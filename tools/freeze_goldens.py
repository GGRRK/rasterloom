#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""freeze_goldens.py - freeze the golden corpus for the embedded self-test (BUILD-SPEC req 14,
<verification>).

    tools/freeze_goldens.py [--cli build/src/rasterloom-cli] [--scripts tests/scripts]
                            [--out tests/goldens] [--jobs K] [--python PY]
    tools/freeze_goldens.py --check [--scripts tests/scripts] [--out tests/goldens]

Freeze (default):
  1. Gate: every golden under tests/scripts (discovery, harness keys, equal_to pairs and tile-stat
     bounds exactly as tests/tools/run_goldens.py defines them) is run through run_goldens with
     the given CLI and the NumPy reference. If ANY golden is not PASS the tool refuses to freeze
     (exit 1) and lists the failures: a frozen expectation must never enshrine a disagreement.
  2. Every golden that must render is rendered by the REFERENCE (tests/reference/refcomp.py, run
     as a black box through run_goldens' cache); the decoded RGBA is re-encoded by this tool's own
     PNG writer (fixed filter heuristic, zlib level 9, no ancillary chunks) into
     <out>/<domain>/<name>.png, so the bytes depend only on the pixels.
  3. The harness-stripped script (run_goldens.strip_harness_keys: "expect"/"equal_to" removed,
     every other token verbatim) is written to <out>/<domain>/<name>.json.
  4. <out>/manifest.json lists every case (sorted by name): kind render|error, equal_to target,
     tile-stat bounds, image size, and sha256 of the source script, the stripped script and the
     PNG. Stale files under <out> are removed. Running the tool twice gives identical bytes.

Check (--check, no CLI and no reference needed; CTest `goldens_frozen_in_sync`): the frozen set
matches tests/scripts right now (same case names, same source hashes, stripped scripts equal,
PNG hashes equal, bounds equal) - i.e. nobody edited a golden without re-freezing.

The C++ build embeds <out>/ (cmake/EmbedFiles.cmake) into the rasterloomselftest library;
`rasterloom --selftest` / `rasterloom-cli --selftest` render every case in-process.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import sys
import tempfile
import zlib
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tests" / "tools"))
import pixcmp  # noqa: E402
import run_goldens as rg  # noqa: E402

MANIFEST_VERSION = 1


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


# ---- deterministic PNG writer (RGBA8, non-interlaced) --------------------------------------------
def _paeth(a: int, b: int, c: int) -> int:
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def _filter_rows(rgba) -> bytes:
    """Per row, the filter (0-4) with the smallest sum of |signed bytes| (the libpng heuristic);
    ties pick the lowest filter number. Pure function of the pixels."""
    import numpy as np

    h, w, _ = rgba.shape
    raw = rgba.reshape(h, w * 4).astype(np.int16)
    zero = np.zeros(w * 4, dtype=np.int16)
    out = bytearray()
    for y in range(h):
        cur = raw[y]
        up = raw[y - 1] if y > 0 else zero
        left = np.concatenate([np.zeros(4, np.int16), cur[:-4]])
        ul = np.concatenate([np.zeros(4, np.int16), up[:-4]])
        cands = [cur, cur - left, cur - up, cur - ((left + up) // 2)]
        # Paeth, vectorised.
        p = left + up - ul
        pa, pb, pc = np.abs(p - left), np.abs(p - up), np.abs(p - ul)
        pred = np.where((pa <= pb) & (pa <= pc), left, np.where(pb <= pc, up, ul))
        cands.append(cur - pred)
        best, best_cost = 0, None
        for f, c in enumerate(cands):
            b = (c & 0xFF).astype(np.uint8)
            cost = int(np.abs(b.astype(np.int8).astype(np.int32)).sum())
            if best_cost is None or cost < best_cost:
                best, best_cost = f, cost
        out.append(best)
        out += (cands[best] & 0xFF).astype(np.uint8).tobytes()
    return bytes(out)


def encode_png(rgba) -> bytes:
    h, w, c = rgba.shape
    assert c == 4

    def chunk(t: bytes, d: bytes) -> bytes:
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)
    idat = zlib.compress(_filter_rows(rgba), 9)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", idat) + chunk(b"IEND", b"")


# ---- helpers ---------------------------------------------------------------------------------------
def build_cases(scripts: Path) -> list[dict]:
    items = rg.discover(scripts, None, [])
    bad = [i for i in items if i["harness_error"]]
    if bad:
        raise SystemExit("freeze_goldens: harness errors: " +
                         "; ".join(f"{i['name']}: {i['harness_error']}" for i in bad))
    cases = []
    for it in items:
        src = Path(it["path"]).read_bytes()
        stripped = rg.strip_harness_keys(src.decode("utf-8")).encode("utf-8") \
            if it["has_expect_key"] else src
        stats = None
        if it["stats"]:
            stats = {k: {"max_allocated": v["max_allocated"], "total": v["total"]}
                     for k, v in sorted(it["stats"].items())}
        cases.append({
            "name": it["name"],
            "kind": "error" if it["expect_error"] else "render",
            "equal_to": it["equal_to"],
            "stats": stats,
            "source_sha256": sha256(src),
            "script_sha256": sha256(stripped),
            "_stripped": stripped,
        })
    return cases


def write_if_changed(p: Path, data: bytes) -> None:
    if p.exists() and p.read_bytes() == data:
        return
    p.parent.mkdir(parents=True, exist_ok=True)
    tmp = p.with_name(p.name + ".tmp")
    tmp.write_bytes(data)
    os.replace(tmp, p)


def manifest_bytes(cases: list[dict]) -> bytes:
    pub = [{k: v for k, v in c.items() if not k.startswith("_")} for c in cases]
    return (json.dumps({"version": MANIFEST_VERSION,
                        "about": "Frozen golden corpus for the embedded self-test; generated by "
                                 "tools/freeze_goldens.py from tests/scripts + the NumPy "
                                 "reference. Do not edit by hand.",
                        "cases": pub}, indent=1, sort_keys=True) + "\n").encode()


# ---- freeze ----------------------------------------------------------------------------------------
def freeze(args) -> int:
    scripts, out = Path(args.scripts).resolve(), Path(args.out).resolve()
    cases = build_cases(scripts)
    cfg = rg.make_cfg(args.cli, args.ref, args.work, jobs=args.jobs, python=args.python,
                      determinism=not args.single)
    items = rg.discover(scripts, None, [])
    print(f"freeze_goldens: gate - {len(items)} goldens, CLI vs reference ...", flush=True)
    report = rg.run_suite(cfg, items)
    print(rg.summary_line(report), flush=True)
    bad = [r for r in report["results"] if r["status"] != "PASS"]
    if bad or report["counts"]["PASS"] != len(items):
        print("freeze_goldens: REFUSING TO FREEZE - the CLI disagrees with the reference on:",
              file=sys.stderr)
        for r in bad:
            print(f"  {r['status']:5} {r['name']}: {r.get('reason', '')}", file=sys.stderr)
        return 1

    expected_files = {out / "manifest.json"}
    with tempfile.TemporaryDirectory(prefix="rl-freeze-") as td:
        td = Path(td)
        for c in cases:
            stem = out / c["name"]
            write_if_changed(stem.with_suffix(".json"), c["_stripped"])
            expected_files.add(stem.with_suffix(".json"))
            if c["kind"] == "error":
                c["png_sha256"], c["w"], c["h"] = None, None, None
                continue
            wd = td / c["name"].replace("/", "__")
            wd.mkdir(parents=True)
            sp = wd / "script.json"
            sp.write_bytes(c["_stripped"])
            r = rg.run_ref_cached(cfg, sp, c["_stripped"], wd)
            if r["rc"] != 0 or not r["png"]:
                print(f"freeze_goldens: reference failed on {c['name']}: {rg._describe(r)}",
                      file=sys.stderr)
                return 1
            rgba = pixcmp.load_rgba(str(wd / "ref.png"))
            png = encode_png(rgba)
            write_if_changed(stem.with_suffix(".png"), png)
            expected_files.add(stem.with_suffix(".png"))
            c["png_sha256"] = sha256(png)
            c["h"], c["w"] = int(rgba.shape[0]), int(rgba.shape[1])
    write_if_changed(out / "manifest.json", manifest_bytes(cases))
    stale = [p for p in out.rglob("*") if p.is_file() and p not in expected_files]
    for p in stale:
        p.unlink()
    for d in sorted((p for p in out.rglob("*") if p.is_dir()), reverse=True):
        if not any(d.iterdir()):
            d.rmdir()
    n_png = sum(1 for c in cases if c["kind"] == "render")
    size = sum(p.stat().st_size for p in out.rglob("*") if p.is_file())
    print(f"freeze_goldens: froze {len(cases)} cases ({n_png} images, "
          f"{len(cases) - n_png} expect-error) into {out} ({size / 1e6:.2f} MB); "
          f"{len(stale)} stale file(s) removed")
    return 0


# ---- check -----------------------------------------------------------------------------------------
def check(args) -> int:
    scripts, out = Path(args.scripts).resolve(), Path(args.out).resolve()
    mpath = out / "manifest.json"
    if not mpath.exists():
        print(f"goldens_frozen_in_sync: {mpath} missing - run tools/freeze_goldens.py",
              file=sys.stderr)
        return 1
    frozen = {c["name"]: c for c in json.loads(mpath.read_text())["cases"]}
    cases = {c["name"]: c for c in build_cases(scripts)}
    problems = []
    for n in sorted(set(cases) - set(frozen)):
        problems.append(f"{n}: new golden, not frozen")
    for n in sorted(set(frozen) - set(cases)):
        problems.append(f"{n}: frozen but no longer in tests/scripts")
    for n in sorted(set(cases) & set(frozen)):
        c, f = cases[n], frozen[n]
        for k in ("kind", "equal_to", "stats", "source_sha256", "script_sha256"):
            if c[k] != f.get(k):
                problems.append(f"{n}: {k} changed since the freeze")
        js = out / (n + ".json")
        if not js.exists() or sha256(js.read_bytes()) != f["script_sha256"]:
            problems.append(f"{n}: frozen script missing or edited")
        if f["kind"] == "render":
            png = out / (n + ".png")
            if not png.exists() or sha256(png.read_bytes()) != f.get("png_sha256"):
                problems.append(f"{n}: frozen PNG missing or edited")
    if problems:
        print("goldens_frozen_in_sync: tests/goldens is stale - re-run tools/freeze_goldens.py:",
              file=sys.stderr)
        for p in problems[:50]:
            print("  " + p, file=sys.stderr)
        if len(problems) > 50:
            print(f"  ... {len(problems) - 50} more", file=sys.stderr)
        return 1
    print(f"goldens_frozen_in_sync: {len(frozen)} frozen cases match tests/scripts")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__)
    ap.add_argument("--cli", default=str(REPO / "build/src/rasterloom-cli"))
    ap.add_argument("--ref", default=str(REPO / "tests/reference/refcomp.py"))
    ap.add_argument("--scripts", default=str(REPO / "tests/scripts"))
    ap.add_argument("--out", default=str(REPO / "tests/goldens"))
    ap.add_argument("--work", default=str(REPO / "tests/output/freeze"),
                    help="run_goldens output dir for the gate (reference cache lives there)")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    ap.add_argument("--python", default=sys.executable, help="interpreter for the reference")
    ap.add_argument("--single", action="store_true", help="gate without the determinism re-render")
    ap.add_argument("--check", action="store_true", help="only verify tests/goldens is in sync")
    args = ap.parse_args(argv)
    if args.check:
        return check(args)
    return freeze(args)


if __name__ == "__main__":
    sys.exit(main())
