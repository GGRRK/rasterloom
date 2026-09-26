#!/usr/bin/env python3
"""Golden render scripts for docs/math/20-adjustments-filters.md (Part C).

Emits tests/scripts/adjust_filters/*.json:
  - C1 adjustment goldens A01..A24, file name = the doc's `file` column,
  - C2 filter goldens G01..G19, file name = the doc's `file` column,
  - err_*.json: scripts the doc defines as script errors ("expect": "error").

Written from docs/math/ only (00, 10, 20). Deterministic: no randomness, no clock,
no environment input. Re-running produces byte-identical files.
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.normpath(os.path.join(HERE, "..", "adjust_filters"))


# ---------------------------------------------------------------- helpers
def dump_script(script):
    """Stable formatting: one op per line, keys in insertion order."""
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


def grad(id_, frm, to, dir_):
    return {"op": "add_layer", "id": id_, "fill": "gradient", "from": frm, "to": to, "dir": dir_}


# ---------------------------------------------------------------- fixtures (C0)
def RAMP():
    return 256, 16, "#00000000", [grad("base", "#000000ff", "#ffffffff", "h")]


def SWEEP():
    return 192, 96, "#00000000", [
        grad("base", "#ff0000ff", "#0000ffff", "h"),
        grad("top", "#00ff00ff", "#00ff0000", "v"),
        grad("grey", "#20202080", "#e0e0e080", "h"),
    ]


def ALPHA():
    return 128, 32, "#00000000", [grad("base", "#ff800000", "#3060ffff", "h")]


def FXA():
    return 64, 48, "#00000000", [
        grad("p", "#ff000000", "#0000ffff", "h"),
        grad("q", "#00ff00c0", "#ffff0000", "v"),
        {"op": "merge_down", "layer": "q"},
    ]


def _fxr_ops():
    return [
        {"op": "add_layer", "id": "p", "fill": "solid", "color": "#20c060ff"},
        {"op": "filter_offset", "layer": "p", "dx": 16, "dy": 12, "mode": "transparent"},
    ]


def FXR():
    return 64, 64, "#ffffffff", _fxr_ops()


def FXS():
    return 96, 64, "#ffffffff", _fxr_ops() + [
        {"op": "add_layer", "id": "s", "fill": "solid", "color": "#d02040ff"},
        {"op": "filter_offset", "layer": "s", "dx": -40, "dy": 30, "mode": "transparent"},
        {"op": "merge_down", "layer": "s"},
    ]


FIXTURES = {"RAMP": RAMP, "SWEEP": SWEEP, "ALPHA": ALPHA, "FXA": FXA, "FXR": FXR, "FXS": FXS}


def with_fixture(fx, extra_ops, **extra):
    w, h, bg, ops = FIXTURES[fx]()
    return script(w, h, bg, ops + extra_ops, **extra)


def adj(type_, params=None):
    op = {"op": "add_adjustment", "id": "adj", "type": type_}
    if params is not None:
        op["params"] = params
    return op


# ---------------------------------------------------------------- C1 adjustments
CURVE16 = [[0, 0], [17, 40], [34, 20], [51, 70], [68, 50], [85, 110], [102, 90], [119, 150],
           [136, 130], [153, 190], [170, 170], [187, 220], [204, 200], [221, 245], [238, 230],
           [255, 255]]

ADJ = [
    ("A01", "adj_levels_identity", "RAMP", adj("levels", {})),
    ("A02", "adj_levels_gamma_up", "RAMP", adj("levels", {"rgb": {"gamma": 2.2}})),
    ("A03", "adj_levels_gamma_down", "SWEEP", adj("levels", {"rgb": {"gamma": 0.45}})),
    ("A04", "adj_levels_input_clip", "RAMP", adj("levels", {"rgb": {"in_black": 30, "in_white": 220}})),
    ("A05", "adj_levels_output_invert", "RAMP",
     adj("levels", {"rgb": {"out_black": 255, "out_white": 0, "gamma": 1.3}})),
    ("A06", "adj_levels_channel_then_composite", "SWEEP",
     adj("levels", {"r": {"in_black": 40, "gamma": 1.8},
                    "b": {"out_white": 200},
                    "rgb": {"in_black": 25, "in_white": 230, "gamma": 0.8}})),
    ("A07", "adj_curves_identity", "RAMP", adj("curves", {})),
    ("A08", "adj_curves_s", "RAMP", adj("curves", {"rgb": [[0, 0], [64, 40], [192, 215], [255, 255]]})),
    ("A09", "adj_curves_endpoints", "RAMP", adj("curves", {"rgb": [[48, 20], [100, 160], [208, 230]]})),
    ("A10", "adj_curves_16pt_red", "SWEEP", adj("curves", {"r": CURVE16})),
    ("A11", "adj_curves_channel_then_composite", "SWEEP",
     adj("curves", {"g": [[0, 30], [128, 100], [255, 255]], "rgb": [[0, 0], [90, 140], [255, 255]]})),
    ("A12", "adj_bc_bright", "RAMP", adj("brightness_contrast", {"brightness": 0.5})),
    ("A13", "adj_bc_dark_contrast", "SWEEP", adj("brightness_contrast", {"brightness": -0.6, "contrast": 0.4})),
    ("A14", "adj_bc_contrast_max", "RAMP", adj("brightness_contrast", {"contrast": 1.0})),
    ("A15", "adj_hs_hue_plus60", "SWEEP", adj("hue_saturation", {"hue": 60.0})),
    ("A16", "adj_hs_hue_minus150", "SWEEP", adj("hue_saturation", {"hue": -150.0})),
    ("A17", "adj_hs_desat_light", "SWEEP", adj("hue_saturation", {"saturation": -100.0, "lightness": 40.0})),
    ("A18", "adj_hs_sat_boost_dark", "SWEEP", adj("hue_saturation", {"saturation": 80.0, "lightness": -50.0})),
    ("A19", "adj_hs_colorize", "SWEEP",
     adj("hue_saturation", {"colorize": True, "hue": 200.0, "saturation": 60.0, "lightness": 10.0})),
    ("A20", "adj_bw_default", "SWEEP", adj("black_white", {})),
    ("A21", "adj_bw_custom_tint", "SWEEP",
     adj("black_white", {"reds": -50.0, "yellows": 250.0, "greens": 120.0, "cyans": -10.0,
                         "blues": 300.0, "magentas": 0.0, "tint": "#e1d3b3"})),
    # A6: "the params field may be omitted" -- A22 exercises the omission.
    ("A22", "adj_invert_alpha", "ALPHA", adj("invert")),
    ("A23", "adj_posterize_4", "RAMP", adj("posterize", {"levels": 4})),
    ("A24", "adj_threshold", "SWEEP", adj("threshold", {"level": 128})),
]


# ---------------------------------------------------------------- C2 filters
def gb(radius, **kw):
    op = {"op": "filter_gaussian_blur", "layer": "p", "radius": radius}
    op.update(kw)
    return op


def mb(angle, distance, **kw):
    op = {"op": "filter_motion_blur", "layer": "p", "angle": angle, "distance": distance}
    op.update(kw)
    return op


FLT = [
    ("G01", "flt_gauss_r3", "FXA", [gb(3.0)]),
    ("G02", "flt_gauss_r1_mixed_widths", "FXS", [gb(1.0)]),
    ("G03", "flt_gauss_r8_transparent_edge", "FXR", [gb(8.0, edge="transparent")]),
    ("G04", "flt_gauss_r05_identity", "FXS", [gb(0.5)]),
    ("G05", "flt_gauss_r45_ramp_coverage", "FXS", [gb(4.5, coverage={"src": "ramp", "dir": "h"})]),
    ("G06", "flt_motion_h10", "FXS", [mb(0.0, 10.0)]),
    ("G07", "flt_motion_30_subpixel", "FXA", [mb(30.0, 7.5)]),
    ("G08", "flt_motion_90_transparent", "FXR", [mb(90.0, 12.0, edge="transparent")]),
    ("G09", "flt_motion_neg45", "FXS", [mb(-45.0, 20.0)]),
    ("G10", "flt_usm_basic", "FXS",
     [{"op": "filter_unsharp_mask", "layer": "p", "amount": 150.0, "radius": 2.0, "threshold": 0}]),
    ("G11", "flt_usm_threshold", "FXS",
     [{"op": "filter_unsharp_mask", "layer": "p", "amount": 300.0, "radius": 1.5, "threshold": 8}]),
    ("G12", "flt_noise_uniform", "FXS",
     [{"op": "filter_add_noise", "layer": "p", "amount": 25.0, "distribution": "uniform", "seed": 7}]),
    ("G13", "flt_noise_gaussian", "FXS",
     [{"op": "filter_add_noise", "layer": "p", "amount": 40.0, "distribution": "gaussian", "seed": 7}]),
    ("G14", "flt_noise_mono_alpha", "FXA",
     [{"op": "filter_add_noise", "layer": "p", "amount": 60.0, "distribution": "gaussian",
       "monochromatic": True, "seed": 99}]),
    ("G15", "flt_high_pass", "FXS", [{"op": "filter_high_pass", "layer": "p", "radius": 3.0}]),
    ("G16", "flt_offset_transparent", "FXS",
     [{"op": "filter_offset", "layer": "p", "dx": 20, "dy": -13, "mode": "transparent"}]),
    ("G17", "flt_offset_wrap_neg", "FXS",
     [{"op": "filter_offset", "layer": "p", "dx": -70, "dy": 5, "mode": "wrap"}]),
    ("G18", "flt_offset_repeat_rect_cov", "FXS",
     [{"op": "filter_offset", "layer": "p", "dx": 11, "dy": 9, "mode": "repeat",
       "coverage": {"src": "rect", "x": 8, "y": 8, "w": 48, "h": 40, "value": 128}}]),
    # G19 was "pending doc 10 op name"; doc 10 section 11.2 now defines `lock_transparency`.
    ("G19", "flt_gauss_lock_alpha", "FXA",
     [{"op": "lock_transparency", "layer": "p", "value": True}, gb(3.0)]),
]


# ---------------------------------------------------------------- script-error cases
# Every case below is a script error by an explicit statement of doc 20 (or 00 C9 / doc 10 11).
ERR = [
    # A1: in_black < in_white required.
    ("err_levels_black_not_below_white", "RAMP",
     [adj("levels", {"rgb": {"in_black": 200, "in_white": 200}})]),
    # A1: gamma range 0.1..9.99.
    ("err_levels_gamma_range", "RAMP", [adj("levels", {"rgb": {"gamma": 10.0}})]),
    # A9 / preamble: unknown keys in params are an error.
    ("err_levels_unknown_param", "RAMP", [adj("levels", {"rgb": {"gama": 1.2}})]),
    # A2: 2..16 points.
    ("err_curves_17_points", "RAMP",
     [adj("curves", {"rgb": [[i * 15, i * 15] for i in range(17)]})]),
    # A2: `in` strictly increasing.
    ("err_curves_not_increasing", "RAMP", [adj("curves", {"rgb": [[0, 0], [128, 90], [128, 160], [255, 255]]})]),
    # A4: colorize = false -> hue range -180..180.
    ("err_hs_hue_range", "SWEEP", [adj("hue_saturation", {"hue": 200.0})]),
    # A5: tint is "#RRGGBB", alpha not accepted.
    ("err_bw_tint_with_alpha", "SWEEP", [adj("black_white", {"tint": "#e1d3b3ff"})]),
    # A7: levels range 2..255.
    ("err_posterize_levels_1", "RAMP", [adj("posterize", {"levels": 1})]),
    # A8: level 1..255 and int-typed (a fractional JSON number is a wrong type).
    ("err_threshold_level_not_int", "SWEEP", [adj("threshold", {"level": 128.5})]),
    # A9: type must be one of the eight.
    ("err_adjust_unknown_type", "RAMP", [adj("exposure", {})]),
    # B0: targeting an adjustment layer with a filter is a script error.
    ("err_filter_on_adjustment", "RAMP", [adj("invert"), gb(2.0, layer="adj")]),
    # B1: radius range 0.1..250.0.
    ("err_gauss_radius_range", "FXA", [gb(0.05)]),
    # B2: distance range 1.0..2000.0.
    ("err_motion_distance_range", "FXA", [mb(0.0, 0.5)]),
    # B6: dx is int.
    ("err_offset_dx_not_int", "FXS", [{"op": "filter_offset", "layer": "p", "dx": 1.5, "dy": 0}]),
    # B0.2: rect coverage value range 0..255.
    ("err_coverage_value_range", "FXS",
     [gb(2.0, coverage={"src": "rect", "x": 0, "y": 0, "w": 8, "h": 8, "value": 256})]),
    # B4: distribution enum.
    ("err_noise_distribution", "FXS",
     [{"op": "filter_add_noise", "layer": "p", "amount": 10.0, "distribution": "poisson"}]),
]


def build():
    out = {}
    for gid, name, fx, op in ADJ:
        out[name] = with_fixture(fx, [op])
    for gid, name, fx, ops in FLT:
        out[name] = with_fixture(fx, ops)
    for name, fx, ops in ERR:
        out[name] = with_fixture(fx, ops, expect="error")
    return out


INDEX = [(g, n) for g, n, _, _ in ADJ] + [(g, n) for g, n, _, _ in FLT]


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
    print(f"adjust_filters: {len(scripts) - n_err} goldens + {n_err} error cases -> {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
