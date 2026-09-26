#!/usr/bin/env python3
"""Self-checks for the brush reference (rc/ops_brush.py, docs/math/40-brush.md).

Run:  python3 tests/reference/test_brush.py            (plain asserts; prints a summary)
      python3 tests/reference/test_brush.py --goldens   (also renders tests/scripts/brush/*.json
                                                         and prints per-script timings)
  or: python3 -m pytest tests/reference/test_brush.py

Expected numbers are either quoted from doc 40 §3.7 / §8 (the doc's worked examples) or worked
out by hand in the comments; none of them were produced by running the reference.
"""

import glob
import json
import os
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from rc.core import ScriptError  # noqa: E402
from rc.fields import FieldReader  # noqa: E402
from rc.model import Document, RasterLayer  # noqa: E402
from rc.ops_brush import (Stroke, _erase, _paint_mask, _paint_rgb, bake_lut, brush_stroke,  # noqa: E402
                          lut_factor, rhu, run_stroke)
from rc.registry import load_all  # noqa: E402
from rc.runner import render_script_text, run_script  # noqa: E402

load_all()

GOLDEN_DIR = os.path.join(os.path.dirname(HERE), "scripts", "brush")


def params(**kw):
    P = {"size": 20.0, "hardness": 1.0, "spacing": 0.25, "opacity": 1.0, "flow": 1.0,
         "angle": 0.0, "roundness": 1.0, "mode": "wash", "dabs_per_second": 0.0,
         "smoothing": 0.0, "size_curve": None, "opacity_curve": None, "samples": []}
    P.update(kw)
    return P


def script(w, h, ops, bg="#00000000"):
    return json.dumps({"canvas": {"w": w, "h": h, "bg": bg}, "ops": ops, "out": "png8"})


def expect_error(text):
    try:
        render_script_text(text)
    except ScriptError:
        return
    raise AssertionError("expected a script error")


# ---------------------------------------------------------------------------------------------
# §3.3 LUT, rhu
# ---------------------------------------------------------------------------------------------

def test_rhu_and_lut():
    assert rhu(-44.5) == -44 and rhu(-40.25) == -40 and rhu(2.5) == 3 and rhu(-0.5) == 0
    ident = bake_lut([(0.0, 0.0), (1.0, 1.0)])
    assert ident == [257 * i for i in range(256)]            # doc 40 §3.3
    assert ident[178] == 45746                                # doc 40 §8 dab 2
    # B13 curve [[0,0],[0.5,0],[0.502,1],[1,1]]: i=128 -> x=0.50196 in segment 1:
    # t = (0.5/255) / 0.002 = 0.98039.. -> y * 65535 = 65535 / 1.02 = 64250 (binary64 gives
    # 64249.9999999997, whose fractional part >= 0.5, so rhu -> 64250).
    # i=127 -> x = 0.49804 < 0.5 -> segment 0, y = 0 -> 0.
    lut = bake_lut([(0.0, 0.0), (0.5, 0.0), (0.502, 1.0), (1.0, 1.0)])
    assert lut[127] == 0 and lut[129] == 65535
    assert lut[128] == 64250
    assert lut_factor(None, 0.3) == 1.0
    # q(128/255) = 128 exactly (the B13 pressure), q(127/255) = 127
    assert lut_factor(lut, 0.5019607843137255) == 64250 / 65535.0
    assert lut_factor(lut, 0.4980392156862745) == 0.0


# ---------------------------------------------------------------------------------------------
# §3.4 walk: the doc's worked examples (§8)
# ---------------------------------------------------------------------------------------------

def test_walk_worked_example():
    log = []
    P = params(size=8.0, spacing=0.25, dabs_per_second=20.0, smoothing=0.5,
               size_curve=[(0.0, 0.0), (1.0, 1.0)],
               samples=[(10.0, 10.0, 1.0, 0.0), (13.0, 14.0, 0.5, 20.0), (13.0, 14.0, 0.5, 120.0)])
    st = run_stroke(P, 32, 32, log)
    want = [
        (10.0, 10.0, 1.0, 8.0),
        (10.90909090909091, 11.212121212121213, 0.696969696969697, 5.584313725490196),
        (11.535483870967742, 12.047311827956989, 0.5, 4.015686274509804),
        (11.766600331996932, 12.355467109329243, 0.5, 4.015686274509804),
        (11.997716793026123, 12.663622390701498, 0.5, 4.015686274509804),
        (12.228833254055314, 12.97177767207375, 0.5, 4.015686274509804),
    ]
    assert log == want, log
    assert st.frac == 0.0915847614247312, repr(st.frac)
    assert st.prev[:2] == (12.25, 13.0)                       # smoothed positions (§8)


def test_walk_split_line():
    log1 = []
    st1 = run_stroke(params(size=16.0, samples=[(32.0, 64.0, 1.0, 0.0), (224.0, 64.0, 1.0, 0.0)]),
                     256, 128, log1)
    assert len(log1) == 48 and st1.frac == 0.9999999999999698, (len(log1), repr(st1.frac))
    log2 = []
    samples = [(32.0 + (2.4 * i), 64.0, 1.0, 0.0) for i in range(81)]
    st2 = run_stroke(params(size=16.0, samples=samples), 256, 128, log2)
    assert len(log2) == 49 and st2.frac == 0.0, (len(log2), repr(st2.frac))
    assert max(abs(a[0] - b[0]) for a, b in zip(log1, log2)) <= 1.2e-13


def test_walk_edge_cases():
    # repeated identical samples, no airbrush: one dab only (§7)
    log = []
    run_stroke(params(samples=[(5.0, 5.0, 1.0, 0.0)] * 5), 16, 16, log)
    assert len(log) == 1
    # decreasing t_ms is held: with 1000 dabs/s, t 0 -> 10 -> 5 -> 10 gives D = 0.01, 0, 0
    # -> todo = 10 on the first segment -> 10 dabs, and nothing afterwards
    log = []
    run_stroke(params(dabs_per_second=1000.0,
                      samples=[(5.0, 5.0, 1.0, 0.0), (5.0, 5.0, 1.0, 10.0), (5.0, 5.0, 1.0, 5.0),
                               (5.0, 5.0, 1.0, 10.0)]), 16, 16, log)
    assert len(log) == 1 + 10, len(log)
    # pressure 0 with identity size curve: d = 0, still counted; next step = spacing * 1 = 0.25
    # A 1 px segment afterwards at p=0 -> 4 more dabs (todo = 1/0.25 = 4 exactly)
    log = []
    st = run_stroke(params(size_curve=[(0.0, 0.0), (1.0, 1.0)],
                           samples=[(5.0, 5.0, 0.0, 0.0), (6.0, 5.0, 0.0, 0.0)]), 16, 16, log)
    assert [e[3] for e in log] == [0.0] * 5 and st.B.max() == 0.0, log


def test_dab_limit():
    # 1 px step, a 1,000,100 px segment far off-canvas -> more than 1e6 dabs -> script error
    t = script(8, 8, [{"op": "add_layer", "id": "L"},
                      {"op": "brush_stroke", "layer": "L", "size": 1.0, "spacing": 1.0,
                       "samples": [{"x": -1e6, "y": -1e6}, {"x": 100.0, "y": -1e6}]}])
    expect_error(t)


# ---------------------------------------------------------------------------------------------
# §3.6 mask and §3.7 accumulation
# ---------------------------------------------------------------------------------------------

def one_dab_m(px_, py_, cx=10.0, cy=10.0, **kw):
    """A single dab at (cx,cy) with opacity 1, flow 1, wash: B = 0 + (1*m)*(1 - 0) = m exactly."""
    P = params(size=8.0, samples=[(cx, cy, 1.0, 0.0)], **kw)
    st = run_stroke(P, 32, 32)
    return st.B[py_, px_]


def test_dab_mask_worked():
    assert one_dab_m(12, 11, hardness=0.5) == 0.563242072809033
    assert one_dab_m(12, 12, hardness=1.0) == 0.44678887547554913
    assert one_dab_m(13, 10, hardness=0.0) == 0.037317932004975574
    assert one_dab_m(12, 8, hardness=1.0, roundness=0.5, angle=30.0) == 0.5623208812614978
    # touched set: size 8 at (10,10): px in [ceil(5.5), floor(13.5)] = [6, 13]; (14,10) untouched
    assert one_dab_m(14, 10, hardness=1.0) == 0.0 and one_dab_m(10, 10) == 1.0
    # hard size-1 dab at a pixel centre: R = Rm = 0.5, w = 1, he = 0, only (4,4) touched, r = 0
    st = run_stroke(params(size=1.0, samples=[(4.5, 4.5, 1.0, 0.0)]), 8, 8)
    assert st.B[4, 4] == 1.0 and st.B.sum() == 1.0
    # sub-pixel dab (§3.5): size 12, identity size curve, p 0.05 -> q = rhu(12.75) = 13,
    # LUT 13*257 = 3341, d = 12 * (3341/65535) = 0.6118 -> d_draw 1, k_small = d, O = d;
    # at a pixel centre m = 1 so B = d; next step = spacing * max(d, 1) = 0.25
    st = run_stroke(params(size=12.0, size_curve=[(0.0, 0.0), (1.0, 1.0)],
                           samples=[(4.5, 4.5, 0.05, 0.0)]), 8, 8)
    assert st.B[4, 4] == 12.0 * (3341 / 65535.0) and st.B.sum() == st.B[4, 4] and st.step == 0.25
    # angle 90 on roundness 0.5: vertical major axis (§3.6 convention). size 8: R 4, Rm 2.
    # pixel (10, 13): ddx 0.5, ddy 3.5; ca ~ 6e-17, sa = 1 -> uu ~ -3.5, vv ~ 0.5
    # -> r ~ sqrt(0.765625 + 0.0625) = 0.91 < 1 (inside); at angle 0 r = sqrt(0.015625+3.0625) > 1.
    assert one_dab_m(10, 13, roundness=0.5, angle=90.0) > 0.0
    assert one_dab_m(10, 13, roundness=0.5, angle=0.0) == 0.0


def test_accumulation_worked():
    # doc 40 §3.7 (revised 2026-09-26): opacity 0.6, flow 0.25, one pixel hit by three dabs,
    # compared by exact Python repr against the doc's printed doubles.
    # m = 1: dab at (10, 10), size 8, hardness 1 (he = 0.75); pixel (9, 9) centre (9.5, 9.5):
    #   r = 0.5/4*sqrt2 = 0.177 <= he -> m = 1.0.
    for mode, want in (("wash", ["0.15", "0.26249999999999996", "0.34687499999999993"]),
                       ("buildup", ["0.15", "0.27749999999999997", "0.38587499999999997"])):
        st = Stroke(params(size=8.0, opacity=0.6, flow=0.25, mode=mode), 32, 32)
        got = []
        for _ in range(3):
            st._place_dab(10.0, 10.0, 1.0)
            got.append(repr(float(st.B[9, 9])))
        assert got == want, (mode, "m=1", got)
    # m = 0.5 (a soft edge): dab at (9.5, 10.5), size 8, hardness 0 (he = min(0, 1 - 1/4) = 0);
    #   pixel (11, 10) centre (11.5, 10.5): ddx = 2, ddy = 0, ca = 1, sa = 0 -> uu = 2, vv = 0,
    #   r = sqrt(0.25) = 0.5; t = 0.5 -> m = 1 - 0.25 * 2 = 0.5 exactly.
    assert one_dab_m(11, 10, cx=9.5, cy=10.5, hardness=0.0) == 0.5
    for mode, want in (("wash", ["0.075", "0.140625", "0.198046875"]),
                       ("buildup", ["0.075", "0.144375", "0.208546875"])):
        st = Stroke(params(size=8.0, hardness=0.0, opacity=0.6, flow=0.25, mode=mode), 32, 32)
        got = []
        for _ in range(3):
            st._place_dab(9.5, 10.5, 1.0)
            got.append(repr(float(st.B[10, 11])))
        assert got == want, (mode, "m=0.5", got)
    # the mask is in the rate, not the ceiling: 400 dabs at m = 0.5 converge to the opacity
    # (Wash) / to full coverage (Build-up), never to O * m = 0.3 or m = 0.5 as the old model did
    for mode, lo, hi in (("wash", 0.599, 0.6), ("buildup", 0.999, 1.0)):
        st = Stroke(params(size=8.0, hardness=0.0, opacity=0.6, flow=0.25, mode=mode), 32, 32)
        for _ in range(400):
            st._place_dab(9.5, 10.5, 1.0)
        assert lo < st.B[10, 11] <= hi, (mode, st.B[10, 11])
    # a pixel with m = 0 inside the touched square is left exactly as it was
    st = Stroke(params(size=8.0, opacity=0.6, flow=0.25), 32, 32)
    st._place_dab(10.0, 10.0, 1.0)
    assert st.B[6, 6] == 0.0            # centre (6.5, 6.5): r = 3.5*sqrt2/4 = 1.24 >= 1 -> m = 0
    # wash ceiling: 200 dabs at flow 0.25 opacity 0.6 never exceed 0.6
    st = Stroke(params(size=8.0, opacity=0.6, flow=0.25), 32, 32)
    for _ in range(200):
        st._place_dab(10.0, 10.0, 1.0)
    assert st.B.max() <= 0.6


# ---------------------------------------------------------------------------------------------
# §3.8 composites (pixel formulas, hand-computed)
# ---------------------------------------------------------------------------------------------

def one(px_):
    return np.array([[px_]], dtype=np.uint8)


def test_composite_pixels():
    a = np.array([[0.25]])
    # unlocked brush, white onto (0,0,0,128): ad = 128/255 = 0.50196;
    # a_o = 0.25 + 0.50196*0.75 = 0.62647 -> 159.75 -> 160; Co = 0.25/0.62647 = 0.39906 -> 101.76 -> 102
    assert tuple(_paint_rgb(one((0, 0, 0, 128)), a, (1.0, 1.0, 1.0), False)[0, 0]) == (102, 102, 102, 160)
    # locked (source-atop, alpha kept): Co = 0*0.75 + 1*0.25 = 0.25 -> 63.75 -> 64; alpha 128 kept
    assert tuple(_paint_rgb(one((0, 0, 0, 128)), a, (1.0, 1.0, 1.0), True)[0, 0]) == (64, 64, 64, 128)
    # locked on alpha 0: byte copy
    assert tuple(_paint_rgb(one((0, 0, 0, 0)), a, (1.0, 1.0, 1.0), True)[0, 0]) == (0, 0, 0, 0)
    # a_s = 0: byte copy (non-canonical colour kept only because the input is kept)
    assert tuple(_paint_rgb(one((9, 8, 7, 6)), np.array([[0.0]]), (1.0, 1.0, 1.0), False)[0, 0]) == (9, 8, 7, 6)
    # full coverage on transparent: exactly the brush colour
    c = (192 / 255.0, 48 / 255.0, 32 / 255.0)
    assert tuple(_paint_rgb(one((0, 0, 0, 0)), np.array([[1.0]]), c, False)[0, 0]) == (192, 48, 32, 255)
    # tiny coverage on transparent: a_o = 0.001 -> q = 0.255 -> 0 -> canonical (0,0,0,0)
    assert tuple(_paint_rgb(one((0, 0, 0, 0)), np.array([[0.001]]), c, False)[0, 0]) == (0, 0, 0, 0)
    # eraser: (10,20,30,255), a_s 0.25 -> a_o = 0.75 -> 191.25 -> 191, colour unchanged
    assert tuple(_erase(one((10, 20, 30, 255)), a)[0, 0]) == (10, 20, 30, 191)
    # eraser to zero -> canonical
    assert tuple(_erase(one((10, 20, 30, 1)), np.array([[0.9]]))[0, 0]) == (0, 0, 0, 0)
    # mask: #404040 -> g = 64/255 (0.30+0.59+0.11 of equal channels ~ 0.25098); Mv = 1, a_s 0.25
    # -> 1 + 0.25*(0.25098 - 1) = 0.81275 -> 207.25 -> 207
    assert _paint_mask(np.array([[255]], dtype=np.uint8), a, (64, 64, 64))[0, 0] == 207


def run_ops(w, h, ops):
    return run_script(json.loads(script(w, h, ops)))


def test_selection_multiplies_buffer():
    doc = Document(16, 16, (0, 0, 0, 0))
    L = RasterLayer("L", doc.empty_rgba())
    doc.root.children.append(L)
    doc.sel[4, 4] = 128
    f = FieldReader({"layer": "L", "color": "#FFFFFF", "size": 5.0,
                     "samples": [{"x": 4.5, "y": 4.5}]})
    brush_stroke(doc, f)
    f.check_unused()
    # hard dab, m = 1 at (4,4); a_s = 1 * 128/255 -> a_o = 128/255 -> 128; Co = a_s/a_o = 1 -> 255
    assert tuple(L.rgba[4, 4]) == (255, 255, 255, 128)
    assert L.rgba[5, 4, 3] == 0 and L.rgba[4, 5, 3] == 0          # s = 0 -> byte copy


def test_lock_rule_ops():
    base = [{"op": "add_layer", "id": "L", "fill": "solid", "color": "#3366CC80"},
            {"op": "lock_transparency", "layer": "L"}]
    dab = {"layer": "L", "size": 5.0, "samples": [{"x": 4.5, "y": 4.5}]}
    doc = run_ops(8, 8, base + [dict(dab, op="brush_stroke", color="#FFFFFF")])
    assert tuple(doc.find("L").rgba[4, 4]) == (255, 255, 255, 128)      # source-atop, alpha kept
    doc = run_ops(8, 8, base[:1] + [dict(dab, op="brush_stroke", color="#FFFFFF")])
    assert tuple(doc.find("L").rgba[4, 4]) == (255, 255, 255, 255)      # unlocked source-over
    doc = run_ops(8, 8, base + [dict(dab, op="eraser_stroke")])
    assert (doc.find("L").rgba == np.array([0x33, 0x66, 0xCC, 0x80], np.uint8)).all()   # no-op
    doc = run_ops(8, 8, base[:1] + [dict(dab, op="eraser_stroke")])
    assert tuple(doc.find("L").rgba[4, 4]) == (0, 0, 0, 0)
    # lock on a transparent layer: brush leaves it transparent
    doc = run_ops(8, 8, [{"op": "add_layer", "id": "L"}, {"op": "lock_transparency", "layer": "L"},
                         dict(dab, op="brush_stroke", color="#FFFFFF")])
    assert not doc.find("L").rgba.any()


def test_mask_target():
    doc = run_ops(8, 8, [{"op": "add_layer", "id": "L", "fill": "solid", "color": "#C03020"},
                         {"op": "add_mask", "layer": "L", "fill": "solid", "value": 255},
                         {"op": "brush_stroke", "layer": "L", "target": "mask", "color": "#000000",
                          "size": 5.0, "samples": [{"x": 4.5, "y": 4.5}]}])
    L = doc.find("L")
    assert L.mask.data[4, 4] == 0 and L.mask.data[0, 0] == 255
    assert (L.rgba[..., 3] == 255).all()                           # pixels untouched


# ---------------------------------------------------------------------------------------------
# §4 clone stamp
# ---------------------------------------------------------------------------------------------

def clone_setup():
    # red opaque layer with one blue pixel at (4,4) (hard size-1 dab at the pixel centre, m = 1)
    return [{"op": "add_layer", "id": "L", "fill": "solid", "color": "#FF0000"},
            {"op": "brush_stroke", "layer": "L", "color": "#0000FF", "size": 1.0,
             "samples": [{"x": 4.5, "y": 4.5}]}]


def clone(x, y, **kw):
    # size 3, hardness 1: R = Rm = 1.5, w = 2/3, he = 1/3; the pixel under the centre has
    # r <= 0.2/1.5*sqrt2 < 1/3 -> m = 1, so it becomes exactly the source pixel.
    d = {"op": "clone_stroke", "layer": "L", "size": 3.0, "samples": [{"x": x, "y": y}]}
    d.update(kw)
    return d


def test_clone_offsets():
    BLUE, RED = (0, 0, 255, 255), (255, 0, 0, 255)
    # offset = (rhu(4.5 - 20.3), rhu(4.5 - 10.6)) = (rhu(-15.8), rhu(-6.1)) = (-16, -6):
    # dest (20,10) reads (4,4) = blue
    doc = run_ops(64, 64, clone_setup() + [clone(20.3, 10.6, source=[4.5, 4.5])])
    assert tuple(doc.find("L").rgba[10, 20]) == BLUE
    # aligned second stroke at (40.3, 30.6) reuses (-16, -6): reads (24, 24) = red
    # non-aligned would recompute (-36, -26): dest (40,30) reads (4,4) = blue
    ops = clone_setup() + [clone(20.3, 10.6, source=[4.5, 4.5]), clone(40.3, 30.6)]
    assert tuple(run_ops(64, 64, ops).find("L").rgba[30, 40]) == RED
    ops = clone_setup() + [clone(20.3, 10.6, source=[4.5, 4.5]), clone(40.3, 30.6, aligned=False)]
    assert tuple(run_ops(64, 64, ops).find("L").rgba[30, 40]) == BLUE
    # clone state is tool state: undo does not clear it, so a later sourceless stroke is valid
    ops = clone_setup() + [clone(20.3, 10.6, source=[4.5, 4.5]), {"op": "undo"}, clone(40.3, 30.6)]
    doc = run_ops(64, 64, ops)
    assert tuple(doc.find("L").rgba[30, 40]) == RED and tuple(doc.find("L").rgba[10, 20]) == RED
    # ...but it does not leak between scripts
    expect_error(script(64, 64, clone_setup() + [clone(40.3, 30.6)]))
    # source out of canvas: reads (0,0,0,0) -> a_s = 0 -> byte copy
    doc = run_ops(64, 64, clone_setup() + [clone(20.3, 10.6, source=[-500.0, 4.5])])
    assert tuple(doc.find("L").rgba[10, 20]) == RED
    # no echo: source region overlapping its own destination samples S0, not the live layer.
    # Stroke from (5.5,4.5) to (12.5,4.5), source (4.5,4.5): offset -1 -> every dest pixel
    # (x,4) reads S0(x-1,4); only (5,4) sees blue; (6..,4) read red S0 pixels, not painted blue.
    ops = clone_setup() + [{"op": "clone_stroke", "layer": "L", "size": 3.0, "source": [4.5, 4.5],
                            "samples": [{"x": 5.5, "y": 4.5}, {"x": 12.5, "y": 4.5}]}]
    rgba = run_ops(64, 64, ops).find("L").rgba
    assert tuple(rgba[4, 5]) == BLUE and tuple(rgba[4, 7]) == RED and tuple(rgba[4, 12]) == RED


# ---------------------------------------------------------------------------------------------
# §2 script errors (own list; the err_*.json goldens are checked in --goldens mode)
# ---------------------------------------------------------------------------------------------

def test_script_errors():
    lay = {"op": "add_layer", "id": "L"}
    good = {"op": "brush_stroke", "layer": "L", "samples": [{"x": 1.0, "y": 1.0}]}
    render_script_text(script(8, 8, [lay, good]))                   # sanity: renders
    bad = [
        {"size": 0.5}, {"size": 5000.5}, {"spacing": 0.0}, {"roundness": 0.0}, {"angle": 361.0},
        {"smoothing": 1.0}, {"dabs_per_second": -1.0}, {"view_zoom": 0.0}, {"view_zoom": 257.0},
        {"mode": "air"}, {"target": "both"}, {"color": "#FF000080"}, {"color": "red"},
        {"size_curve": [[0, 0]]}, {"size_curve": [[0, 0], [0.5, 1], [0.5, 1], [1, 1]]},
        {"size_curve": [[0.1, 0], [1, 1]]}, {"size_curve": [[0, 0], [0.9, 1]]},
        {"size_curve": [[0, 0], [1, 1.5]]}, {"size_curve": [[i / 16.0, 0] for i in range(16)] + [[1, 1]]},
        {"opacity_curve": "linear"}, {"samples": []}, {"samples": [{"x": 1.0}]},
        {"samples": [{"x": 1.0, "y": 2e6}]}, {"samples": [{"x": 1.0, "y": 1.0, "t_ms": -1.0}]},
        {"samples": [{"x": 1.0, "y": 1.0, "tilt_y": -91.0}]},
        {"samples": [{"x": 1.0, "y": 1.0, "speed": 1.0}]}, {"target": "mask"}, {"layer": "nope"},
        {"hardness": True}, {"size": "8"},
    ]
    for b in bad:
        expect_error(script(8, 8, [lay, dict(good, **b)]))
    # pressure is clamped, never an error
    render_script_text(script(8, 8, [lay, dict(good, samples=[{"x": 1.0, "y": 1.0, "pressure": 7.5}])]))
    # colour hex is case-insensitive and #RRGGBBff is accepted
    render_script_text(script(8, 8, [lay, dict(good, color="#aabbccff")]))
    # eraser and clone take no colour / target
    er = {"op": "eraser_stroke", "layer": "L", "samples": [{"x": 1.0, "y": 1.0}]}
    render_script_text(script(8, 8, [lay, er]))
    expect_error(script(8, 8, [lay, dict(er, color="#000000")]))
    expect_error(script(8, 8, [lay, dict(er, target="pixels")]))
    cl = {"op": "clone_stroke", "layer": "L", "source": [1.0, 1.0], "samples": [{"x": 1.0, "y": 1.0}]}
    render_script_text(script(8, 8, [lay, cl]))
    for b in ({"color": "#000000"}, {"source": [1.0]}, {"source": [1.0, 2e6]}, {"aligned": 1},
              {"source_layer": "nope"}, {"target": "pixels"}):
        expect_error(script(8, 8, [lay, dict(cl, **b)]))
    expect_error(script(8, 8, [{"op": "add_group", "id": "G"}, dict(good, layer="G")]))


def test_undo_one_record_per_stroke():
    ops = [{"op": "add_layer", "id": "L"},
           {"op": "brush_stroke", "layer": "L", "size": 4.0,
            "samples": [{"x": 2.0 + i, "y": 8.0} for i in range(40)]},
           {"op": "brush_stroke", "layer": "L", "size": 4.0, "color": "#FF0000",
            "samples": [{"x": 8.0, "y": 2.0 + i} for i in range(40)]}]
    one_stroke = run_ops(64, 64, ops[:2]).find("L").rgba
    after_undo = run_ops(64, 64, ops + [{"op": "undo"}]).find("L").rgba
    assert (one_stroke == after_undo).all()
    expect_error(script(64, 64, ops + [{"op": "undo", "steps": 4}]))


# ---------------------------------------------------------------------------------------------
# goldens (tests/scripts/brush), rendered when present
# ---------------------------------------------------------------------------------------------

def render_goldens(verbose=True):
    """Render every brush golden. Returns (results, failures). Harness keys are stripped first."""
    paths = sorted(glob.glob(os.path.join(GOLDEN_DIR, "*.json")))
    results, fails, pngs = [], [], {}
    for p in paths:
        stem = os.path.splitext(os.path.basename(p))[0]
        d = json.load(open(p))
        expect = d.pop("expect", None)
        equal_to = d.pop("equal_to", None)
        t0 = time.perf_counter()
        try:
            rgba = render_script_text(json.dumps(d))
            status, err = "ok", None
        except ScriptError as e:
            rgba, status, err = None, "script-error", str(e)
        dt = time.perf_counter() - t0
        if expect == "error":
            verdict = "PASS" if status == "script-error" else "FAIL(rendered)"
        else:
            verdict = "PASS" if status == "ok" else "FAIL(%s)" % err
        if rgba is not None:
            pngs[stem] = rgba
        results.append([stem, verdict, dt, equal_to])
    for r in results:
        if r[3] is not None and r[1] == "PASS":
            a, b = pngs.get(r[0]), pngs.get(r[3])
            if b is None or not np.array_equal(a, b):
                r[1] = "FAIL(not equal to %s)" % r[3]
    for r in results:
        if r[1] != "PASS":
            fails.append(r)
        if verbose:
            print("  %-34s %-60s %7.3f s" % (r[0], r[1][:60], r[2]))
    return results, fails


def main():
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_") and callable(v)]
    ok = 0
    for t in tests:
        t0 = time.perf_counter()
        try:
            t()
            ok += 1
            print("PASS %-40s %6.3f s" % (t.__name__, time.perf_counter() - t0))
        except Exception as e:                            # noqa: BLE001
            import traceback
            traceback.print_exc()
            print("FAIL %s: %s" % (t.__name__, e))
    print("%d/%d self-checks passed" % (ok, len(tests)))
    rc = 0 if ok == len(tests) else 1
    if "--goldens" in sys.argv:
        if not os.path.isdir(GOLDEN_DIR):
            print("no golden directory %s" % GOLDEN_DIR)
        else:
            t0 = time.perf_counter()
            results, fails = render_goldens()
            print("goldens: %d/%d pass, total %.2f s" % (len(results) - len(fails), len(results),
                                                        time.perf_counter() - t0))
            if fails:
                rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
