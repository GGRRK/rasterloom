#!/usr/bin/env python3
"""Golden render scripts for docs/math/40-brush.md (section 9, B01..B27).

Emits tests/scripts/brush/B01.json .. B27.json plus err_*.json scripts the doc defines as script
errors ("expect": "error").

Written from docs/math/ only (00, 10, 30, 40). Deterministic: every sample stream is computed
here from closed-form paths; coordinates are rounded to 1e-3 px (round() on binary64, then the
shortest repr, which JSON parsers read back to the same double). Re-running produces
byte-identical files.

Where section 9 leaves a parameter unstated (colour, size, path of a "stroke across ..."), this
generator picks a value and says so in the per-case comment; unstated brush fields otherwise take
their section-2.2 defaults (they are simply omitted from the op).
"""
import json
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.normpath(os.path.join(HERE, "..", "brush"))


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


def r3(v):
    v = round(float(v), 3)
    return 0.0 if v == 0 else v          # no "-0.0" in the corpus


def S(x, y, p=None, t=None):
    """One sample object (doc 40 2.1). Omitted fields take their defaults."""
    s = {"x": r3(x), "y": r3(y)}
    if p is not None:
        s["pressure"] = float(p)
    if t is not None:
        s["t_ms"] = float(t)
    return s


def timed(samples, dt=8.0):
    """Give samples monotone t_ms = k * dt (a recorded mouse at ~125 Hz)."""
    out = []
    for k, s in enumerate(samples):
        s = dict(s)
        s["t_ms"] = float(k) * dt
        out.append(s)
    return out


def stroke(kind, layer, samples, **kw):
    op = {"op": kind, "layer": layer}
    for k, v in kw.items():
        op[k] = float(v) if isinstance(v, int) and not isinstance(v, bool) and k in DOUBLE_FIELDS else v
    op["samples"] = samples
    return op


DOUBLE_FIELDS = {"size", "hardness", "spacing", "opacity", "flow", "angle", "roundness",
                 "dabs_per_second", "smoothing", "view_zoom"}


def brush(layer, samples, **kw):
    return stroke("brush_stroke", layer, samples, **kw)


def eraser(layer, samples, **kw):
    return stroke("eraser_stroke", layer, samples, **kw)


def clone(layer, samples, **kw):
    return stroke("clone_stroke", layer, samples, **kw)


LAYER = {"op": "add_layer", "id": "L"}


def solid_layer(color, id_="L"):
    return {"op": "add_layer", "id": id_, "fill": "solid", "color": color}


# ---------------------------------------------------------------- deterministic paths
def line(x0, y0, x1, y1, n, p0=None, p1=None):
    """n samples evenly spaced on a straight segment, optional linear pressure ramp."""
    out = []
    for k in range(n):
        u = k / (n - 1)
        p = None if p0 is None else p0 + (p1 - p0) * u
        out.append(S(x0 + (x1 - x0) * u, y0 + (y1 - y0) * u, p))
    return out


def figure_eight(cx, cy, ax, ay, n):
    """Lemniscate of Gerono: x = cx + ax*sin(t), y = cy + ay*sin(2t), t in [0, 2*pi].
    Self-crossing at (cx, cy); n+1 samples, the last one closes the loop."""
    out = []
    for k in range(n + 1):
        t = 2.0 * math.pi * k / n
        out.append(S(cx + ax * math.sin(t), cy + ay * math.sin(2.0 * t)))
    return out


def zigzag(x_left, x_right, y0, dy, n_vertices):
    """Polyline whose vertices alternate between x_left and x_right, y stepping by dy."""
    return [S(x_left if k % 2 == 0 else x_right, y0 + dy * k) for k in range(n_vertices)]


def arc(cx, cy, r, a0_deg, a1_deg, n):
    out = []
    for k in range(n):
        a = math.radians(a0_deg + (a1_deg - a0_deg) * k / (n - 1))
        out.append(S(cx + r * math.cos(a), cy + r * math.sin(a)))
    return out


# ---------------------------------------------------------------- fixtures
def disc_layer_ops():
    """B17/B20: 'layer with an opaque disc on transparent' -- a non-AA ellipse (doc 30) filled
    opaque, so every alpha byte is 0 or 255. Disc: box (32,32,64,64), colour #2060c0ff."""
    return [LAYER,
            {"op": "select_ellipse", "x": 32.0, "y": 32.0, "w": 64.0, "h": 64.0, "antialias": False},
            {"op": "fill_selection", "layer": "L", "color": "#2060c0ff"},
            {"op": "deselect"},
            {"op": "lock_transparency", "layer": "L", "value": True}]


def clone_layer_ops():
    """B21/B22: 'layer with gradient + shape' -- a 2-D gradient (h gradient on L, then a v
    gradient layer merged down) plus an opaque ellipse and a half-transparent rect via doc 30."""
    return [
        {"op": "add_layer", "id": "L", "fill": "gradient", "from": "#1030a0ff", "to": "#f0d020ff", "dir": "h"},
        {"op": "add_layer", "id": "V", "fill": "gradient", "from": "#ff000000", "to": "#ff0000ff", "dir": "v"},
        {"op": "merge_down", "layer": "V"},
        {"op": "select_ellipse", "x": 8.0, "y": 8.0, "w": 28.0, "h": 20.0, "antialias": True},
        {"op": "fill_selection", "layer": "L", "color": "#ffffffff"},
        {"op": "select_rect", "x": 14.0, "y": 22.0, "w": 16.0, "h": 12.0},
        {"op": "fill_selection", "layer": "L", "color": "#00000080"},
        {"op": "deselect"},
    ]


def b05_line():
    return [S(20, 32), S(236, 32)]


def b06_samples():
    # "81 samples x_i = 20.0 + (2.7 * i)" evaluated in binary64, NOT rounded to 1e-3.
    return [{"x": 20.0 + (2.7 * i), "y": 32.0} for i in range(81)]


def b08_path():
    return timed(figure_eight(64, 64, 44, 30, 96))


def pressure_down_up(n, p_hi, p_lo):
    """n pressures: p_hi -> p_lo at the midpoint -> p_hi, piecewise linear."""
    half = (n - 1) / 2.0
    return [p_hi + (p_lo - p_hi) * (1.0 - abs(k - half) / half) for k in range(n)]


# ---------------------------------------------------------------- goldens
G = {}

# B01: one sample at (32,32), size 21, hardness 1, #C03020.
G["B01"] = script(64, 64, "#00000000", [LAYER, brush("L", [S(32, 32)], color="#C03020", size=21, hardness=1)])

# B02: one dab at (31.3,32.6), size 40, hardness 0 (colour: default #000000).
G["B02"] = script(64, 64, "#00000000", [LAYER, brush("L", [S(31.3, 32.6)], size=40, hardness=0)])

# B03: one dab, size 40, hardness 0.5; position unstated -> (32,32).
G["B03"] = script(64, 64, "#00000000", [LAYER, brush("L", [S(32, 32)], size=40, hardness=0.5)])

# B04: 96x96, one dab at (48,48), size 60, roundness 0.3, angle 30.
G["B04"] = script(96, 96, "#00000000",
                  [LAYER, brush("L", [S(48, 48)], size=60, roundness=0.3, angle=30)])

# B05: 256x64, 2-sample line (20,32)->(236,32), size 16, spacing default.
G["B05"] = script(256, 64, "#00000000", [LAYER, brush("L", b05_line(), size=16)])

# B06: same line as 81 samples x_i = 20.0 + 2.7*i.
G["B06"] = script(256, 64, "#00000000", [LAYER, brush("L", b06_samples(), size=16)])

# B07: B05 with view_zoom 4.0; must also equal B05's bytes (harness key equal_to).
G["B07"] = script(256, 64, "#00000000", [LAYER, brush("L", b05_line(), size=16, view_zoom=4.0)],
                  equal_to="B05")

# B08: 128x128 opaque white layer, figure-eight, size 30, hardness 0.8, opacity 0.6, flow 0.25, wash.
G["B08"] = script(128, 128, "#00000000", [
    solid_layer("#ffffffff"),
    brush("L", b08_path(), size=30, hardness=0.8, opacity=0.6, flow=0.25, mode="wash")])

# B09: B08 with mode buildup.
G["B09"] = script(128, 128, "#00000000", [
    solid_layer("#ffffffff"),
    brush("L", b08_path(), size=30, hardness=0.8, opacity=0.6, flow=0.25, mode="buildup")])

# B10: 128x128 layer #808080, zig-zag that re-covers itself (goes down then back up over the
# same band), opacity 0.5, flow 1.0, wash. Size unstated -> 24; colour default.
_zz = zigzag(16, 112, 24, 10, 9)                       # down: y 24..104
_zz = _zz + list(reversed(zigzag(112, 16, 29, 10, 8)))  # back up, offset by half a row
G["B10"] = script(128, 128, "#00000000", [
    solid_layer("#808080"),
    brush("L", timed(_zz), size=24, opacity=0.5, flow=1.0, mode="wash")])

# B11: 128x128 layer #3366CC80, soft stroke #FFCC00, opacity 0.8, flow 0.5.
# Path/size unstated -> an S-curve of 33 samples, size 36.
_s = [S(16 + 96 * k / 32, 64 + 36 * math.sin(2 * math.pi * k / 32)) for k in range(33)]
G["B11"] = script(128, 128, "#00000000", [
    solid_layer("#3366CC80"),
    brush("L", timed(_s), color="#FFCC00", hardness=0, opacity=0.8, flow=0.5, size=36)])

# B12: 256x64, line with pressure ramp 0->1 over 30 samples, size_curve as given.
# Size unstated -> 40. Line (16,32)->(240,32).
G["B12"] = script(256, 64, "#00000000", [LAYER, brush(
    "L", timed(line(16, 32, 240, 32, 30, 0.0, 1.0)), size=40,
    size_curve=[[0.0, 0.0], [0.3, 0.1], [0.7, 0.9], [1.0, 1.0]])])

# B13: 64x64, 2 dabs at constant pressure 128/255 and 127/255, size 24, size_curve given.
# "2 dabs" -> two single-sample strokes (clicks) at (20,32) and (44,32).
_c13 = [[0.0, 0.0], [0.5, 0.0], [0.502, 1.0], [1.0, 1.0]]
G["B13"] = script(64, 64, "#00000000", [
    LAYER,
    brush("L", [S(20, 32, 128 / 255)], size=24, size_curve=_c13),
    brush("L", [S(44, 32, 127 / 255)], size=24, size_curve=_c13)])

# B14: 256x64, pressure 1 -> 0.2 -> 1, opacity_curve identity, wash. 41 samples on
# (16,32)->(240,32); size unstated -> 24.
_p14 = pressure_down_up(41, 1.0, 0.2)
_l14 = line(16, 32, 240, 32, 41)
for s_, p_ in zip(_l14, _p14):
    s_["pressure"] = p_
G["B14"] = script(256, 64, "#00000000", [LAYER, brush(
    "L", timed(_l14), size=24, mode="wash", opacity_curve=[[0.0, 0.0], [1.0, 1.0]])])

# B15: 192x192, zig-zag of 25 samples, smoothing 0.7, pressure ramp, identity size_curve.
# Ramp 0.2 -> 1.0; size unstated -> 24.
_z15 = zigzag(24, 168, 20, 6.5, 25)
for k, s_ in enumerate(_z15):
    s_["pressure"] = 0.2 + 0.8 * k / 24
G["B15"] = script(192, 192, "#00000000", [LAYER, brush(
    "L", timed(_z15), size=24, smoothing=0.7, size_curve=[[0.0, 0.0], [1.0, 1.0]])])

# B16: 64x64, airbrush dabs_per_second 50, 11 samples at (32,32) with t_ms 0,16,..,160,
# soft (hardness 0), flow 0.2; wash on layer L, and a second layer W with buildup.
# Size unstated -> 24. Colours: L default #000000, W #C03020.
_air = [S(32, 32, None, 16 * k) for k in range(11)]
G["B16"] = script(64, 64, "#00000000", [
    LAYER,
    brush("L", _air, size=24, hardness=0, flow=0.2, dabs_per_second=50, mode="wash"),
    {"op": "add_layer", "id": "W"},
    brush("W", _air, color="#C03020", size=24, hardness=0, flow=0.2, dabs_per_second=50, mode="buildup")])

# B17: opaque disc on transparent, lock-transparency on, stroke across the disc edge.
# Stroke unstated -> (8,40)->(120,88), colour #FFCC00, size 20, hardness 0.5.
G["B17"] = script(128, 128, "#00000000", disc_layer_ops() + [
    brush("L", timed(line(8, 40, 120, 88, 12)), color="#FFCC00", size=20, hardness=0.5)])

# B18: elliptical selection with feather, stroke crossing the selection edge.
# Chosen: bg white, transparent layer, select_ellipse (32,40,64,48) aa + feather 8, then a stroke
# that goes out, back and out again across the edge (self-overlap inside the soft edge),
# opacity 0.6, flow 0.5, wash, hardness 0.7, size 22, colour #1E3A8A.
_b18 = [S(8, 60), S(120, 68), S(8, 76), S(120, 60)]
G["B18"] = script(128, 128, "#ffffffff", [
    LAYER,
    {"op": "select_ellipse", "x": 32.0, "y": 40.0, "w": 64.0, "h": 48.0, "antialias": True},
    {"op": "feather", "radius": 8.0},
    brush("L", timed(_b18), color="#1E3A8A", size=22, hardness=0.7, opacity=0.6, flow=0.5, mode="wash")])

# B19: 128x128 opaque gradient layer, eraser self-crossing, soft, opacity 0.7, flow 0.4, wash.
# Gradient h #2040C0FF -> #F0C020FF; figure-eight path as B08; size unstated -> 24.
G["B19"] = script(128, 128, "#00000000", [
    {"op": "add_layer", "id": "L", "fill": "gradient", "from": "#2040C0FF", "to": "#F0C020FF", "dir": "h"},
    eraser("L", b08_path(), size=24, hardness=0, opacity=0.7, flow=0.4, mode="wash")])

# B20: B17's layer, lock on, eraser across the disc (same path as B17's stroke, size 20).
G["B20"] = script(128, 128, "#00000000", disc_layer_ops() + [
    eraser("L", timed(line(8, 40, 120, 88, 12)), size=20, hardness=0.5)])

# B21: clone_stroke with source (20,20), aligned, then a second aligned clone_stroke elsewhere.
# First stroke starts at (64.5,60.25): offset = (rhu(-44.5), rhu(-40.25)) = (-44, -40), which
# separates rhu from round-half-away-from-zero. Size 18, hardness 0.6.
_cl1 = timed(line(64.5, 60.25, 100, 72, 10))
_cl2 = timed(line(60, 100, 110, 110, 10))
G["B21"] = script(128, 128, "#00000000", clone_layer_ops() + [
    clone("L", _cl1, source=[20.0, 20.0], aligned=True, size=18, hardness=0.6),
    clone("L", _cl2, aligned=True, size=18, hardness=0.6)])

# B22: B21 with aligned false for the second stroke, plus a third stroke whose source region
# overlaps its own destination: source (40,70), stroke from (46,74) to (100,74) (offset (-6,-4)).
_cl3 = timed(line(46, 74, 100, 74, 12))
G["B22"] = script(128, 128, "#00000000", clone_layer_ops() + [
    clone("L", _cl1, source=[20.0, 20.0], aligned=True, size=18, hardness=0.6),
    clone("L", _cl2, aligned=False, size=18, hardness=0.6),
    clone("L", _cl3, source=[40.0, 70.0], aligned=True, size=18, hardness=0.6)])

# B23: 64x64, two brush_strokes, then {"op":"undo"} (steps omitted = default 1).
G["B23"] = script(64, 64, "#00000000", [
    LAYER,
    brush("L", timed(line(8, 20, 56, 20, 7)), color="#C03020", size=12),
    brush("L", timed(line(8, 44, 56, 44, 7)), color="#2040C0", size=12),
    {"op": "undo"}])

# B24: 64x64 layer with a white mask, brush_stroke target mask #404040 soft.
# Layer content unstated -> solid #C03020; bg white so the mask edit is visible; stroke
# (8,32)->(56,32), size 24.
G["B24"] = script(64, 64, "#ffffffff", [
    solid_layer("#C03020"),
    {"op": "add_mask", "layer": "L", "fill": "solid", "value": 255},
    brush("L", timed(line(8, 32, 56, 32, 9)), target="mask", color="#404040", hardness=0, size=24)])

# B25: 128x64, pressure tapering to 0.01 at the end, identity size curve, size 12.
# 40 samples on (12,32)->(116,32), pressure 1.0 -> 0.01 linear.
G["B25"] = script(128, 64, "#00000000", [LAYER, brush(
    "L", timed(line(12, 32, 116, 32, 40, 1.0, 0.01)), size=12, size_curve=[[0.0, 0.0], [1.0, 1.0]])])

# B26: 64x64, soft size 6, spacing 0.01, 5-sample curve, view_zoom 0.5.
# Curve: 5 samples on a 150-degree arc of radius 20 about (32,32).
G["B26"] = script(64, 64, "#00000000", [LAYER, brush(
    "L", timed(arc(32, 32, 20, 200, 350, 5)), size=6, hardness=0, spacing=0.01, view_zoom=0.5)])

# B27: 64x64, stroke from (-20,-10) to (90,70), both off-canvas. Size unstated -> 16.
G["B27"] = script(64, 64, "#00000000", [LAYER, brush("L", [S(-20, -10), S(90, 70, None, 30)], size=16)])

# B28 (added 2026-09-26 with the doc 40 3.7 revision): the beading case. 192x96 opaque white layer,
# soft (hardness 0) size 48 stroke at flow 1, opacity 1, default 25 % spacing, a gentle arc.
# The edge must be the smooth accumulation of the dab falloffs (mutation 42 = the old max-envelope).
G["B28"] = script(192, 96, "#00000000", [
    solid_layer("#FFFFFF"),
    brush("L", timed(arc(96, 150, 110, 235, 305, 24)), color="#E0602A", hardness=0,
          opacity=1.0, flow=1.0, size=48)])


# ---------------------------------------------------------------- script-error cases
def _one(**kw):
    return script(64, 64, "#00000000", [LAYER, brush("L", [S(32, 32)], **kw)], expect="error")


ERR = {
    # 2.4: clone_stroke with no source when none was set earlier.
    "err_clone_no_source": script(64, 64, "#00000000", [
        solid_layer("#808080"), clone("L", [S(32, 32)])], expect="error"),
    # 2.2 / 7: target "mask" on a layer with no mask.
    "err_mask_target_without_mask": _one(target="mask"),
    # 2.2: colour AA other than FF is an error.
    "err_color_alpha_not_ff": _one(color="#FF000080"),
    # 2.1: samples must contain at least 1 sample.
    "err_samples_empty": script(64, 64, "#00000000", [LAYER, brush("L", [])], expect="error"),
    # 2.2: size range [1, 5000].
    "err_size_below_1": _one(size=0.5),
    # 2.2: spacing range [0.01, 10].
    "err_spacing_zero": _one(spacing=0.0),
    # 2.2: view_zoom range (0, 256].
    "err_view_zoom_zero": _one(view_zoom=0.0),
    # 2.2: curve first x must be exactly 0.0.
    "err_curve_first_x": _one(size_curve=[[0.1, 0.0], [1.0, 1.0]]),
    # 2.2: curve x strictly increasing.
    "err_curve_x_not_increasing": _one(opacity_curve=[[0.0, 0.0], [0.5, 0.2], [0.5, 0.8], [1.0, 1.0]]),
    # 2.2: mode enum.
    "err_mode_unknown": _one(mode="glaze"),
    # 2.0: unknown field.
    "err_unknown_field": _one(spacng=0.25),
    # 2.1: tilt range [-90, 90].
    "err_tilt_range": script(64, 64, "#00000000", [
        LAYER, brush("L", [{"x": 32.0, "y": 32.0, "tilt_x": 95.0}])], expect="error"),
    # 2.1: t_ms >= 0.
    "err_t_ms_negative": script(64, 64, "#00000000", [
        LAYER, brush("L", [S(32, 32, None, -1)])], expect="error"),
    # 2.0: layer must be a raster layer (group id).
    "err_stroke_on_group": script(64, 64, "#00000000", [
        {"op": "add_group", "id": "G"}, brush("G", [S(32, 32)])], expect="error"),
    # 2.5 / 00 C10: undo past the start of history. add_layer and the stroke are the only two
    # history records, so steps 3 is past the start.
    "err_undo_past_start": script(64, 64, "#00000000", [
        LAYER, brush("L", [S(32, 32)]), {"op": "undo", "steps": 3}], expect="error"),
}


def main():
    os.makedirs(OUT, exist_ok=True)
    for f in os.listdir(OUT):
        if f.endswith(".json"):
            os.remove(os.path.join(OUT, f))
    scripts = dict(G)
    scripts.update(ERR)
    for name in sorted(scripts):
        with open(os.path.join(OUT, name + ".json"), "w", newline="\n") as fh:
            fh.write(dump_script(scripts[name]))
    print(f"brush: {len(G)} goldens + {len(ERR)} error cases -> {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
