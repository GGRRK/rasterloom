#!/usr/bin/env python3
"""Golden render scripts for docs/math/30-geometry-selection.md (section 20).

Emits tests/scripts/geometry/*.json, one file per golden id (SEL-01..SEL-24, TR-01..TR-14,
RA-01..RA-10, GR-01..GR-04, BK-01..BK-04), plus err_*.json scripts the doc defines as script
errors ("expect": "error").

Written from docs/math/ only (00, 10, 30). Deterministic: re-running produces byte-identical files.

Translation rules for the section-20 prose sketches:
  - "A" = canvas 64x64, bg #00000000, add_layer L (empty).
  - "select_rect x,y,w,h" -> {"op":"select_rect","x":..,"y":..,"w":..,"h":..}; "aa" -> antialias true;
    "(non-AA)" / "non-AA" -> antialias false (written explicitly even where it is the default).
  - SEL-* goldens end with fill_selection L "#1060e0ff" unless the sketch already ends with a
    fill_selection (the doc's "Unless stated, each selection golden ends with ...").
  - TR/RA/GR/BK goldens get no trailing fill (they are not selection goldens).
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.normpath(os.path.join(HERE, "..", "geometry"))

FILL_BLUE = "#1060e0ff"


# ---------------------------------------------------------------- helpers
def dump_script(script):
    lines = ["{"]
    lines.append('  "canvas": ' + json.dumps(script["canvas"]) + ",")
    lines.append('  "ops": [')
    ops = script["ops"]
    for i, op in enumerate(ops):
        lines.append("    " + json.dumps(op) + ("," if i + 1 < len(ops) else ""))
    lines.append("  ],")
    tail = [("out", script["out"])]
    for k in ("expect", "equal_to"):
        if k in script:
            tail.append((k, script[k]))
    for i, (k, v) in enumerate(tail):
        lines.append("  " + json.dumps(k) + ": " + json.dumps(v) + ("," if i + 1 < len(tail) else ""))
    lines.append("}")
    return "\n".join(lines) + "\n"


def script(w, h, bg, ops, **extra):
    s = {"canvas": {"w": w, "h": h, "bg": bg}, "ops": ops, "out": "png8"}
    s.update(extra)
    return s


def num(v):
    """Doc 30 'num' fields: emit as JSON numbers with a fractional part for readability."""
    return float(v)


def rect(x, y, w, h, aa=None, mode=None):
    op = {"op": "select_rect", "x": num(x), "y": num(y), "w": num(w), "h": num(h)}
    if aa is not None:
        op["antialias"] = aa
    if mode is not None:
        op["mode"] = mode
    return op


def ellipse(x, y, w, h, aa=None, mode=None):
    op = {"op": "select_ellipse", "x": num(x), "y": num(y), "w": num(w), "h": num(h)}
    if aa is not None:
        op["antialias"] = aa
    if mode is not None:
        op["mode"] = mode
    return op


def polygon(pts, aa=None, mode=None):
    op = {"op": "select_polygon", "points": [[num(a), num(b)] for a, b in pts]}
    if aa is not None:
        op["antialias"] = aa
    if mode is not None:
        op["mode"] = mode
    return op


def wand(layer, x, y, tol=None, contiguous=None, aa=None, mode=None):
    op = {"op": "select_wand", "layer": layer, "x": x, "y": y}
    if tol is not None:
        op["tolerance"] = tol
    if contiguous is not None:
        op["contiguous"] = contiguous
    if aa is not None:
        op["antialias"] = aa
    if mode is not None:
        op["mode"] = mode
    return op


def fill(layer, color, opacity=None):
    op = {"op": "fill_selection", "layer": layer, "color": color}
    if opacity is not None:
        op["opacity"] = num(opacity)
    return op


def gradient(layer, type_, p0, p1, c0, c1, opacity=None, reverse=None):
    op = {"op": "gradient", "layer": layer, "type": type_,
          "p0": [num(p0[0]), num(p0[1])], "p1": [num(p1[0]), num(p1[1])], "c0": c0, "c1": c1}
    if opacity is not None:
        op["opacity"] = num(opacity)
    if reverse is not None:
        op["reverse"] = reverse
    return op


def transform(layer, **kw):
    op = {"op": "transform", "layer": layer}
    for k, v in kw.items():
        if k == "interp":
            op[k] = v
        elif k in ("matrix",):
            op[k] = [num(x) for x in v]
        elif k in ("translate", "scale", "skew", "pivot"):
            op[k] = [num(v[0]), num(v[1])]
        elif k == "rotate":
            op[k] = num(v)
        elif k == "quad":
            op[k] = [[num(a), num(b)] for a, b in v]
        elif k == "rect":
            op[k] = [num(x) for x in v]
        else:
            raise KeyError(k)
    return op


def bucket(layer, x, y, color, tol=None, contiguous=None, aa=None, opacity=None):
    op = {"op": "bucket_fill", "layer": layer, "x": x, "y": y, "color": color}
    if opacity is not None:
        op["opacity"] = num(opacity)
    if tol is not None:
        op["tolerance"] = tol
    if contiguous is not None:
        op["contiguous"] = contiguous
    if aa is not None:
        op["antialias"] = aa
    return op


def simple(name, **kw):
    op = {"op": name}
    op.update(kw)
    return op


DESELECT = simple("deselect")
ADD_L = {"op": "add_layer", "id": "L"}


# ---------------------------------------------------------------- content fixtures
def A(ops):
    return script(64, 64, "#00000000", [ADD_L] + ops)


def H_ops():
    return [ADD_L, ellipse(16, 16, 32, 32, aa=True), simple("feather", radius=num(4)),
            fill("L", "#ff2000ff"), DESELECT]


def H(ops):
    return script(64, 64, "#ffffffff", H_ops() + ops)


def K_ops():
    ops = [ADD_L]
    for by in range(8):
        for bx in range(8):
            if (bx + by) % 2 == 0:
                ops.append(rect(8 * bx, 8 * by, 8, 8, mode="add"))
    assert len(ops) == 1 + 32
    ops += [fill("L", "#000000ff"), DESELECT]
    return ops


def K(ops):
    return script(64, 64, "#00000000", K_ops() + ops)


def T(ops):
    base = [ADD_L,
            polygon([[12, 10], [50, 14], [44, 52], [16, 40]], aa=False),
            gradient("L", "linear", [8, 8], [56, 56], "#ff0000ff", "#0000ffff"),
            DESELECT]
    return script(64, 64, "#ffffffff", base + ops)


def sel20_content():
    return [gradient("L", "linear", [0, 0], [64, 0], "#000000ff", "#ffffffff")]


# ---------------------------------------------------------------- 20.1 selections
def sel(ops, end_fill=True):
    return A(ops + ([fill("L", FILL_BLUE)] if end_fill else []))


SEL = {
    "SEL-01": sel([rect(8, 8, 40, 24)]),
    "SEL-02": sel([rect(8.25, 8.5, 40.3, 23.75, aa=True)]),
    "SEL-03": sel([rect(10.5, 6.4, 20.2, 30.6, aa=False)]),
    "SEL-04": sel([ellipse(4, 6, 56, 40, aa=True)]),
    "SEL-05": sel([ellipse(5, 5, 33, 21, aa=False)]),
    "SEL-06": sel([ellipse(20.3, 20.7, 2.5, 3, aa=True)]),
    "SEL-07": sel([polygon([[4, 60], [32, 3.5], [61, 57]], aa=True)]),
    "SEL-08": sel([polygon([[32, 4], [48, 58], [5, 24], [59, 24], [16, 58]], aa=True)]),
    "SEL-09": sel([polygon([[8.5, 8.5], [40, 8.5], [40, 20.5], [20, 20.5], [20, 40], [8.5, 40]], aa=False)]),
    "SEL-10": sel([polygon([[-10, 10], [70, 20], [30, 30], [70, 50], [-5, 60]], aa=False)]),
    "SEL-11": sel([ellipse(4, 4, 36, 36, aa=True), ellipse(24, 20, 36, 36, aa=True, mode="add")]),
    "SEL-12": sel([ellipse(4, 4, 40, 40, aa=True), ellipse(20.5, 18.25, 40, 40, aa=True, mode="subtract")]),
    "SEL-13": sel([ellipse(10, 10, 40, 30, aa=True), ellipse(10, 10, 40, 30, aa=True, mode="subtract")]),
    "SEL-14": sel([rect(4.5, 4.5, 40, 40, aa=True), ellipse(16, 16, 44, 44, aa=True, mode="intersect")]),
    "SEL-15": sel([rect(8, 8, 20, 20), DESELECT, ellipse(30, 30, 20, 20, aa=True, mode="add"), DESELECT,
                   simple("reselect")]),
    "SEL-16": sel([simple("select_all"), simple("contract", by=3), rect(20, 20, 24, 24, mode="subtract"),
                   simple("contract", by=2)]),
    "SEL-17": sel([ellipse(8, 8, 48, 40, aa=True), simple("select_inverse")]),
    "SEL-18": script(64, 64, "#00000000",
                     K_ops() + [wand("L", 2, 2, tol=0, contiguous=True, aa=False), fill("L", FILL_BLUE)]),
    "SEL-19": sel([rect(4, 4, 16, 16), rect(40, 40, 16, 16, mode="add"), fill("L", "#000000ff"),
                   rect(40, 4, 16, 16), fill("L", "#141414ff"), DESELECT,
                   wand("L", 8, 8, tol=20, contiguous=False, aa=False)]),
    "SEL-20": sel(sel20_content() + [wand("L", 10, 32, tol=32, aa=False)]),
    "SEL-21": sel(sel20_content() + [wand("L", 10, 32, tol=40, aa=True)]),
    "SEL-22": script(64, 64, "#00000000", [
        {"op": "add_layer", "id": "M"},
        rect(0, 0, 32, 64), fill("M", "#ff0000ff"),
        simple("select_inverse"), fill("M", "#ff000080"),
        DESELECT,
        {"op": "add_layer", "id": "L"},
        wand("M", 10, 10, tol=100, aa=False),
        fill("L", FILL_BLUE)]),
    "SEL-23": sel([rect(16, 16, 32, 32), simple("feather", radius=num(8))]),
    "SEL-24": sel([rect(20, 20, 10, 10), ellipse(40, 8, 12, 12, aa=True, mode="add"), simple("expand", by=4)]),
}

# ---------------------------------------------------------------- 20.2 transform
TR = {
    "TR-01": T([transform("L", matrix=[1, 0, 0, 0, 1, 0, 0, 0, 1])]),
    "TR-02": T([transform("L", translate=[5, -3])]),
    "TR-03": T([transform("L", translate=[0.5, 0.25])]),
    "TR-04": T([transform("L", scale=[2, 2], pivot=[32, 32])]),
    "TR-05": T([transform("L", scale=[0.5, 0.5], pivot=[0, 0])]),
    "TR-06": T([transform("L", rotate=30)]),
    "TR-07": T([transform("L", rotate=90)]),
    "TR-08": T([transform("L", skew=[20, 0])]),
    "TR-09": T([transform("L", matrix=[1, 0, 0, 0, 1, 0, 0.004, 0, 1])]),
    "TR-10": T([transform("L", quad=[[4, 4], [52, 10], [60, 58], [12, 52]])]),
    "TR-11": T([transform("L", quad=[[16, 4], [48, 4], [62, 60], [2, 60]])]),
    "TR-12": T([transform("L", matrix=[1, 0, 0, 0, 1, 0, 0, 0.03, 1])]),
    "TR-13": T([transform("L", rotate=45, interp="nearest")]),
    "TR-14": T([transform("L", matrix=[-1, 0, 64, 0, 1, 0, 0, 0, 1])]),
}

# ---------------------------------------------------------------- 20.3 resample / alpha
RA = {
    "RA-01": H([transform("L", rotate=30)]),
    "RA-02": H([transform("L", scale=[1.7, 1.7], translate=[0.3, 0.3])]),
    "RA-03": H([transform("L", scale=[0.6, 0.6])]),
    "RA-04": H([simple("image_size", w=100, h=100)]),
    "RA-05": script(128, 128, "#ffffffff", [
        ADD_L, ellipse(32, 32, 64, 64, aa=True), simple("feather", radius=num(6)),
        fill("L", "#ff2000ff"), DESELECT, simple("image_size", w=48, h=48)]),
    "RA-06": H([simple("image_size", w=96, h=40)]),
    "RA-07": H([simple("rotate_canvas", angle=num(30))]),
    "RA-08": script(64, 48, "#ffffffff", [
        ADD_L, ellipse(6, 20, 30, 20, aa=True), simple("feather", radius=num(3)),
        fill("L", "#ff2000ff"), DESELECT,
        simple("rotate_canvas", angle=num(90)), simple("rotate_canvas", angle=num(180)),
        simple("rotate_canvas", angle=num(-90)), simple("flip", axis="v")]),
    "RA-09": H([simple("canvas_size", w=81, h=51, anchor="c"), simple("crop", x=-3, y=5, w=60, h=70)]),
    "RA-10": script(32, 32, "#808080ff", [
        ADD_L, rect(4, 4, 8, 24), fill("L", "#ffffffff"),
        rect(12, 4, 8, 24), fill("L", "#000000ff"), DESELECT,
        transform("L", scale=[2.3, 2.3], pivot=[0, 0])]),
}

# ---------------------------------------------------------------- 20.4 tools
TOOLS = {
    "GR-01": A([gradient("L", "linear", [5.5, 10], [58, 50], "#ff0000ff", "#0000ffff")]),
    "GR-02": A([gradient("L", "radial", [32, 32], [60, 40], "#00ff00ff", "#0000ff00", reverse=True)]),
    "GR-03": A([ellipse(8, 8, 48, 48, aa=True),
                gradient("L", "linear", [0, 0], [64, 0], "#ff0000ff", "#ffff00ff", opacity=0.5)]),
    # GR-04's colours are "..." in the doc; any valid pair works because a zero-length drag is a no-op.
    "GR-04": A([fill("L", "#808080ff"),
                gradient("L", "linear", [10, 10], [10, 10], "#ff0000ff", "#0000ffff")]),
    "BK-01": K([bucket("L", 2, 2, "#00ff00ff", tol=0, aa=False)]),
    "BK-02": A(sel20_content() + [bucket("L", 10, 32, "#ff00ffff", tol=16, contiguous=False, aa=False)]),
    "BK-03": A(sel20_content() + [rect(0, 0, 40, 64),
                                  bucket("L", 10, 32, "#00ffffff", tol=24, opacity=0.6)]),
    # BK-04's rect position/colour are not given by the doc; chosen: 20x16 at (22,24), #2040a0ff.
    "BK-04": A([rect(22, 24, 20, 16), fill("L", "#2040a0ff"), DESELECT,
                bucket("L", 0, 0, "#ff8000ff", tol=0)]),
}

# ---------------------------------------------------------------- script-error cases
ERR = {
    # 12.2 quad form: den == 0 is an invalid script (sx != 0 so the projective branch runs).
    "err_quad_den_zero": T([transform("L", quad=[[0, 0], [10, 0], [10, 0], [0, 10]])]),
    # 12.3: |det| < 1e-12 is invalid (singular matrix, also 00 C9).
    "err_matrix_singular": T([transform("L", matrix=[1, 0, 0, 1, 0, 0, 0, 0, 1])]),
    # 19: mixing fields of two forms is invalid.
    "err_transform_mixed_forms": T([transform("L", matrix=[1, 0, 0, 0, 1, 0, 0, 0, 1], rotate=10)]),
    # 19: scale components must be nonzero.
    "err_transform_scale_zero": T([transform("L", scale=[0, 1])]),
    # 19: skew each in [-89, 89].
    "err_transform_skew_range": T([transform("L", skew=[90, 0])]),
    # 19: select_rect w in (0, 65536].
    "err_select_rect_w_zero": A([rect(4, 4, 0, 8)]),
    # 19: select_polygon needs 3..4096 points.
    "err_polygon_two_points": A([polygon([[4, 4], [40, 40]])]),
    # 19: expand by int in [1, 100].
    "err_expand_zero": A([ellipse(8, 8, 32, 32, aa=True), simple("expand", by=0)]),
    # 19: tolerance int in [0, 255].
    "err_wand_tolerance_range": A(sel20_content() + [wand("L", 10, 32, tol=256)]),
    # 19: a reference to a non-raster layer where a raster layer is required.
    "err_wand_on_group": script(64, 64, "#00000000", [
        ADD_L, {"op": "add_group", "id": "G"}, wand("G", 4, 4)]),
    # 19: mode enum.
    "err_select_mode_unknown": A([rect(4, 4, 8, 8, mode="xor")]),
    # 19: feather radius num in [0, 250].
    "err_feather_negative": A([rect(4, 4, 8, 8), simple("feather", radius=num(-1))]),
    # 19: rotate_canvas angle in [-3600, 3600]. (The "> 16384 result" error is not scripted: it
    # needs a huge canvas that a renderer may allocate before it can reject the op.)
    "err_rotate_canvas_angle_range": H([simple("rotate_canvas", angle=num(3601))]),
    # 19: canvas_size anchor enum.
    "err_canvas_size_anchor": H([simple("canvas_size", w=80, h=80, anchor="center")]),
    # 19: crop x is int.
    "err_crop_x_not_int": H([simple("crop", x=1.5, y=0, w=10, h=10)]),
    # 19: gradient type enum.
    "err_gradient_type": A([gradient("L", "angle", [0, 0], [10, 10], "#000000ff", "#ffffffff")]),
}


def build():
    out = {}
    for d in (SEL, TR, RA, TOOLS):
        out.update(d)
    for k, v in ERR.items():
        v = dict(v)
        v["expect"] = "error"
        out[k] = v
    return out


def main():
    os.makedirs(OUT, exist_ok=True)
    for f in os.listdir(OUT):
        if f.endswith(".json"):
            os.remove(os.path.join(OUT, f))
    scripts = build()
    for name in sorted(scripts):
        with open(os.path.join(OUT, name + ".json"), "w", newline="\n") as fh:
            fh.write(dump_script(scripts[name]))
    n_err = sum(1 for s in scripts.values() if s.get("expect") == "error")
    print(f"geometry: {len(scripts) - n_err} goldens + {n_err} error cases -> {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
