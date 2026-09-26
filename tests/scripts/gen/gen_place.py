#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Golden render scripts for docs/math/60-editing-ops.md §14 (place_image, layer_via_copy, clear).

Writes tests/scripts/place/*.json: the goldens of §14.8 (PL-*, LVC-*, CLR-*, their *R equal_to
targets and FIX-K) and one err_*.json per rule §14 makes a script error.

Written from docs/math/ only (00, 10, 20, 30, 60). The payload PNGs are produced by this file's own
writer, built from §14.2 (not from any library), and deliberately exercise the whole accepted
subset: every filter type 0-4 in rotation across rows (so Sub, Up, Average and Paeth all have to be
undone correctly), IDAT split into several chunks, ancillary chunks (some with a wrong CRC, which
§14.2 says is never examined), an optional PLTE, a zero-length IDAT and bytes after IEND.
Deterministic: re-running produces byte-identical files.
"""
import base64
import json
import os
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.normpath(os.path.join(HERE, "..", "place"))
FILL_BLUE = "#1060e0ff"


# ------------------------------------------------------------------------------------ output
def dump_script(script):
    def j(v):
        return json.dumps(v, ensure_ascii=True)
    lines = ["{", '  "canvas": ' + j(script["canvas"]) + ",", '  "ops": [']
    ops = script["ops"]
    for i, op in enumerate(ops):
        lines.append("    " + j(op) + ("," if i + 1 < len(ops) else ""))
    lines.append("  ],")
    tail = [("out", script["out"])]
    for k in ("expect", "equal_to"):
        if k in script:
            tail.append((k, script[k]))
    for i, (k, v) in enumerate(tail):
        lines.append("  " + j(k) + ": " + j(v) + ("," if i + 1 < len(tail) else ""))
    lines.append("}")
    return "\n".join(lines) + "\n"


def script(ops, w=64, h=64, bg="#00000000", **extra):
    s = {"canvas": {"w": w, "h": h, "bg": bg}, "ops": ops, "out": "png8"}
    s.update(extra)
    return s


# ------------------------------------------------------------------ images (lists of rows)
def img_bands(w, h, salt=0):
    """Colour ramps with alpha bands that include 0 (with non-zero colour), 1, 128 and 255."""
    alphas = [0, 1, 64, 127, 128, 200, 254, 255]
    rows = []
    for j in range(h):
        row = []
        for i in range(w):
            r = (i * 13 + j * 7 + salt) % 256
            g = (i * 29 + j * 3 + 2 * salt) % 256
            b = (j * 31 + i + 5 * salt) % 256
            a = alphas[(i + 2 * j + salt) % len(alphas)]
            row.append((r, g, b, a))
        rows.append(row)
    return rows


def img_solid(w, h, rgba):
    return [[rgba] * w for _ in range(h)]


# ----------------------------------------------------------------------- PNG writer (§14.2)
def _paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def _filter_row(t, cur, prev):
    out = bytearray()
    for k in range(len(cur)):
        a = cur[k - 4] if k >= 4 else 0
        b = prev[k] if prev is not None else 0
        c = prev[k - 4] if (prev is not None and k >= 4) else 0
        pred = [0, a, b, (a + b) // 2, _paeth(a, b, c)][t]
        out.append((cur[k] - pred) & 0xFF)
    return bytes(out)


def raw_scanlines(rows, bad_filter_row=None):
    """Filtered scanlines, filter type (row index mod 5): every type 0..4 is used."""
    out = bytearray()
    prev = None
    for y, row in enumerate(rows):
        cur = bytes(v for px in row for v in px)
        t = y % 5
        out.append(5 if y == bad_filter_row else t)
        out += _filter_row(t, cur, prev)
        prev = cur
    return bytes(out)


def chunk(ctype, data, bad_crc=False):
    crc = zlib.crc32(ctype + data) & 0xFFFFFFFF
    if bad_crc:
        crc ^= 0x5A5A5A5A
    return struct.pack(">I", len(data)) + ctype + data + struct.pack(">I", crc)


def png(rows, *, depth=8, color=6, interlace=0, w=None, h=None, idat_parts=3, ancillary=True,
        plte=False, zero_idat_first=False, after_iend=b"", idat_bad_crc=False, no_iend=False,
        unknown_critical=False, split_idat_by_text=False, plte_after_idat=False, data_delta=0,
        bad_filter_row=None):
    h_ = len(rows) if h is None else h
    w_ = len(rows[0]) if w is None else w
    raw = raw_scanlines(rows, bad_filter_row)
    if data_delta < 0:
        raw = raw[:data_delta]
    elif data_delta > 0:
        raw = raw + b"\x00" * data_delta
    z = zlib.compress(raw, 9)
    n = max(1, idat_parts)
    cut = [len(z) * k // n for k in range(n + 1)]
    idats = [z[cut[k]:cut[k + 1]] for k in range(n)]
    out = b"\x89PNG\r\n\x1a\n"
    out += chunk(b"IHDR", struct.pack(">IIBBBBB", w_, h_, depth, color, 0, 0, interlace))
    if ancillary:
        out += chunk(b"gAMA", struct.pack(">I", 45455))
        out += chunk(b"tEXt", b"Comment\x00ignored by section 14.2", bad_crc=True)
    if plte:
        out += chunk(b"PLTE", bytes([0, 0, 0, 255, 255, 255, 16, 96, 224]))
    if unknown_critical:
        out += chunk(b"ABCD", b"xyz")
    if zero_idat_first:
        out += chunk(b"IDAT", b"")
    for k, part in enumerate(idats):
        out += chunk(b"IDAT", part, bad_crc=idat_bad_crc and k == 0)
        if split_idat_by_text and k == 0:
            out += chunk(b"tEXt", b"Comment\x00between IDATs")
    if plte_after_idat:
        out += chunk(b"PLTE", bytes([0, 0, 0]))
    if ancillary:
        out += chunk(b"zTXt", b"k\x00\x00" + zlib.compress(b"after IDAT"))
    if not no_iend:
        out += chunk(b"IEND", b"")
    return out + after_iend


def b64(data):
    return base64.b64encode(data).decode("ascii")


# ------------------------------------------------------------------------------ op builders
def layer(id_, parent=None, **kw):
    op = {"op": "add_layer", "id": id_}
    if parent is not None:
        op["parent"] = parent
    op.update(kw)
    return op


def solid(id_, color, rect=None, parent=None):
    kw = {"fill": "solid", "color": color}
    if rect is not None:
        kw["rect"] = list(rect)
    return layer(id_, parent, **kw)


def noise(id_, seed, parent=None):
    return layer(id_, parent, fill="noise", seed=seed, alpha="random")


def place(id_, data, **kw):
    op = {"op": "place_image", "id": id_, "png": data if isinstance(data, str) else b64(data)}
    op.update(kw)
    return op


def lvc(id_, **kw):
    op = {"op": "layer_via_copy", "id": id_}
    op.update(kw)
    return op


def clear(l):
    return {"op": "clear", "layer": l}


def setp(op, layer_id, value):
    return {"op": op, "layer": layer_id, "value": value}


def hide(l):
    return setp("set_visible", l, False)


def blend(l, mode):
    return {"op": "set_blend", "layer": l, "mode": mode}


def undo(steps=None):
    return {"op": "undo"} if steps is None else {"op": "undo", "steps": steps}


def sel_rect(x, y, w, h):
    return {"op": "select_rect", "x": float(x), "y": float(y), "w": float(w), "h": float(h)}


def sel_ellipse(x, y, w, h):
    return {"op": "select_ellipse", "x": float(x), "y": float(y), "w": float(w), "h": float(h),
            "antialias": True}


def feather(r):
    return {"op": "feather", "radius": float(r)}


def K():
    """Doc 60 fixture K on 64x64."""
    return [layer("k", fill="gradient", **{"from": "#2040C0FF", "to": "#F0C020FF"}, dir="h")]


def RAMP(id_="u", rect=(8, 8, 48, 48)):
    return layer(id_, fill="gradient", **{"from": "#E0302000", "to": "#E03020FF"}, dir="v",
                 rect=list(rect))


# ---------------------------------------------------------------------------------- goldens
def build():
    S = {}
    A = img_bands(20, 12)
    B = img_bands(67, 69, salt=3)
    PA = png(A)
    S["FIX-K"] = script(K())

    # ---- place_image
    S["PL-01"] = script(K() + [place("p", PA)])
    S["PL-02"] = script(K() + [place("p", PA, x=-5, y=50)])
    S["PL-03"] = script(K() + [place("p", png(B, idat_parts=5))])
    S["PL-04"] = script(K() + [solid("u", "#10E080FF", (8, 8, 32, 32)),
                               solid("t", "#E03020FF", (20, 20, 36, 36)), blend("t", "mul"),
                               place("p", PA, above="u", x=14, y=14)])
    S["PL-05"] = script(K() + [{"op": "add_group", "id": "g", "mode": "scrn"},
                               solid("c", "#303030FF", (4, 4, 40, 40), parent="g"),
                               place("p", PA, parent="g", x=10, y=12),
                               setp("set_opacity", "g", 0.75)])
    S["PL-06"] = script(K() + [place("p", png(img_solid(32, 16, (0x10, 0xE0, 0x80, 0xFF))),
                                     x=8, y=8)], equal_to="PL-06R")
    S["PL-06R"] = script(K() + [solid("p", "#10E080FF", (8, 8, 32, 16))])
    S["PL-07"] = script(K() + [place("p", PA, name="photo"), setp("set_opacity", "p", 0.5),
                               blend("p", "diff")])
    S["PL-08"] = script(K() + [place("p", PA), undo()], equal_to="FIX-K")
    S["PL-09"] = script(K() + [place("p", png(A, plte=True, zero_idat_first=True,
                                              after_iend=b"trailing bytes after IEND"),
                                     x=30, y=3)])

    # ---- layer_via_copy
    S["LVC-01"] = script(K() + [noise("u", 7), sel_ellipse(6, 10, 40, 30), lvc("c", layer="u"),
                                hide("u")])
    S["LVC-02"] = script([layer("k", fill="gradient", **{"from": "#2040C0FF", "to": "#F0C020FF"},
                                dir="h", rect=[0, 0, 40, 64]),
                          solid("u", "#C08040C0", (10, 4, 40, 50)), blend("u", "mul"),
                          {"op": "add_adjustment", "id": "a", "type": "invert", "params": {}},
                          setp("set_fill", "a", 0.3), sel_rect(4, 6, 56, 30),
                          lvc("m", merged=True), hide("k"), hide("u"), hide("a")],
                         bg="#336699FF")
    cut_setup = K() + [RAMP(), sel_rect(12, 16, 30, 24), feather(3)]
    S["LVC-03"] = script(cut_setup + [lvc("c", layer="u", cut=True), hide("c")])
    S["LVC-04"] = script(cut_setup + [lvc("c", layer="u", cut=True), hide("u")])
    S["LVC-05"] = script(K() + [solid("u", "#10E080FF", (8, 8, 32, 32)),
                                solid("t", "#E03020FF", (20, 20, 36, 36)), blend("t", "mul"),
                                sel_rect(4, 4, 30, 30), lvc("c", layer="u"), blend("c", "diff")])
    S["LVC-06"] = script(K() + [noise("u", 11), lvc("c", layer="u"),
                                {"op": "delete_layer", "layer": "u"}], equal_to="LVC-06R")
    S["LVC-06R"] = script(K() + [noise("c", 11)])
    S["LVC-07"] = script(K() + [noise("u", 5),
                                {"op": "add_mask", "layer": "u", "fill": "gradient", "from": 0,
                                 "to": 255},
                                setp("set_opacity", "u", 0.3), hide("u"),
                                sel_rect(10, 10, 30, 20), lvc("c", layer="u", name="copy")])
    S["LVC-08"] = script(cut_setup + [lvc("c", layer="u", cut=True), undo()],
                         equal_to="LVC-08R")
    S["LVC-08R"] = script(cut_setup)

    # ---- clear
    S["CLR-01"] = script(K() + [noise("u", 9), sel_ellipse(6, 10, 44, 34), clear("u")])
    S["CLR-02"] = script(K() + [noise("u", 9), clear("u")], equal_to="FIX-K")
    S["CLR-03"] = script(K() + [noise("u", 9), {"op": "lock_transparency", "layer": "u"},
                                sel_rect(4, 4, 30, 30), clear("u")], equal_to="CLR-03R")
    S["CLR-03R"] = script(K() + [noise("u", 9), {"op": "lock_transparency", "layer": "u"},
                                 sel_rect(4, 4, 30, 30)])
    S["CLR-04"] = script(K() + [RAMP(), sel_rect(12, 16, 30, 24), feather(3), clear("u"),
                                undo(), sel_ellipse(20, 4, 40, 40), clear("u")])

    # ---- errors (each breaks exactly one rule of §14)
    def err(ops, **kw):
        return script(K() + ops, expect="error", **kw)

    good = b64(PA)
    S["err_place_b64_space"] = err([place("p", good[:40] + " " + good[41:])])
    S["err_place_b64_urlsafe"] = err([place("p", good[:40] + "-" + good[41:])])
    S["err_place_b64_length"] = err([place("p", good[:-1])])
    S["err_place_b64_inner_pad"] = err([place("p", "QQ==" + good)])
    S["err_place_not_png"] = err([place("p", b"GIF89a not a png at all")])
    S["err_place_rgb"] = err([place("p", png(A, color=2))])
    S["err_place_16bit"] = err([place("p", png(A, depth=16))])
    S["err_place_interlaced"] = err([place("p", png(A, interlace=1))])
    S["err_place_idat_crc"] = err([place("p", png(A, idat_bad_crc=True))])
    S["err_place_no_iend"] = err([place("p", png(A, no_iend=True))])
    S["err_place_unknown_critical"] = err([place("p", png(A, unknown_critical=True))])
    S["err_place_idat_split"] = err([place("p", png(A, split_idat_by_text=True))])
    S["err_place_plte_after_idat"] = err([place("p", png(A, plte_after_idat=True))])
    S["err_place_data_short"] = err([place("p", png(A, data_delta=-1))])
    S["err_place_data_long"] = err([place("p", png(A, data_delta=1))])
    S["err_place_filter5"] = err([place("p", png(A, bad_filter_row=3))])
    S["err_place_too_wide"] = err([place("p", png([[(1, 2, 3, 255)]], w=16385, h=1))])
    S["err_place_x_only"] = err([place("p", PA, x=3)])
    S["err_place_x_range"] = err([place("p", PA, x=40000, y=0)])
    S["err_place_above_and_parent"] = err([place("p", PA, above="k", parent="root")])
    S["err_place_above_root"] = err([place("p", PA, above="root")])
    S["err_place_id_taken"] = err([place("k", PA)])
    S["err_place_name_control"] = err([place("p", PA, name="line\nbreak")])
    S["err_lvc_group"] = err([{"op": "add_group", "id": "g"}, lvc("c", layer="g")])
    S["err_lvc_adjustment"] = err([{"op": "add_adjustment", "id": "a", "type": "invert",
                                    "params": {}}, lvc("c", layer="a")])
    S["err_lvc_merged_with_layer"] = err([lvc("c", layer="k", merged=True)])
    S["err_lvc_merged_cut"] = err([lvc("c", merged=True, cut=True)])
    S["err_lvc_no_layer"] = err([lvc("c")])
    S["err_lvc_empty"] = err([layer("e"), sel_rect(4, 4, 10, 10), lvc("c", layer="e")])
    S["err_clr_group"] = err([{"op": "add_group", "id": "g"}, clear("g")])
    S["err_clr_root"] = err([clear("root")])
    return S


def main(argv):
    if argv[1:]:
        print(f"usage: {os.path.basename(argv[0])}", file=sys.stderr)
        return 2
    scripts = build()
    for name, s in scripts.items():
        tgt = s.get("equal_to")
        if tgt is not None and tgt not in scripts:
            raise SystemExit(f"{name}: equal_to target {tgt} is not generated")
    os.makedirs(OUT, exist_ok=True)
    for f in os.listdir(OUT):
        if f.endswith(".json"):
            os.remove(os.path.join(OUT, f))
    for name in sorted(scripts):
        with open(os.path.join(OUT, name + ".json"), "w", encoding="utf-8", newline="\n") as fh:
            fh.write(dump_script(scripts[name]))
    n_err = sum(1 for s in scripts.values() if s.get("expect") == "error")
    print(f"place: {len(scripts) - n_err} goldens + {n_err} error cases -> {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
