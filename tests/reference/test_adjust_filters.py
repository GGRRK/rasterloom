#!/usr/bin/env python3
"""Self-checks for rc/ops_adjust_filters.py (doc 20), with hand-computed expected bytes.

Run:  python3 tests/reference/test_adjust_filters.py               (self-checks)
      python3 tests/reference/test_adjust_filters.py --goldens [OUTDIR]
            (also renders every tests/scripts/adjust_filters/*.json, honouring the harness-only
             "expect": "error" key, and prints per-script timings; PNGs go to OUTDIR if given)

Expected values were worked out by hand from docs/math/20-adjustments-filters.md; the arithmetic
is in the comments. Where a value sits on a real-arithmetic rounding tie the test avoids it.
"""

import glob
import json
import math
import os
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from rc import ops_adjust_filters as AF  # noqa: E402
from rc.core import ScriptError, pixel_hash, q, unit  # noqa: E402
from rc.fields import FieldReader  # noqa: E402
from rc.registry import load_all  # noqa: E402
from rc.runner import render_script_text  # noqa: E402

load_all()


# ---------------------------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------------------------

def planes(pixels):
    """list of (r, g, b) -> three uint8 (1, n) planes."""
    a = np.array([pixels], dtype=np.uint8)
    return a[..., 0], a[..., 1], a[..., 2]


def adj(parse, apply, params, pixels):
    p = FieldReader(params, "params")
    prm = parse(p)
    p.check_unused()
    r, g, b = apply(prm, *planes(pixels))
    return [tuple(int(v) for v in t) for t in zip(r[0], g[0], b[0])]


def lut_of(parse, apply, params):
    i = np.arange(256, dtype=np.uint8)[None, :]
    p = FieldReader(params, "params")
    prm = parse(p)
    p.check_unused()
    r, _, _ = apply(prm, i, i, i)
    return [int(v) for v in r[0]]


def row(pixels):
    """list of RGBA tuples -> (1, n, 4) uint8."""
    return np.array([pixels], dtype=np.uint8)


def tup(arr):
    return [tuple(int(v) for v in p) for p in arr.reshape(-1, 4)]


def script(ops, w=4, h=4, bg="#00000000"):
    return {"canvas": {"w": w, "h": h, "bg": bg}, "ops": ops, "out": "png8"}


def render(ops, **kw):
    return render_script_text(json.dumps(script(ops, **kw)))


def expect_error(ops, **kw):
    try:
        render(ops, **kw)
    except ScriptError:
        return
    raise AssertionError("expected a script error for %r" % (ops,))


T = []


def test(fn):
    T.append(fn)
    return fn


# ---------------------------------------------------------------------------------------------
# A1 Levels
# ---------------------------------------------------------------------------------------------

@test
def levels():
    P, A = AF.parse_levels, AF.apply_levels
    ident = lut_of(P, A, {})
    assert ident == list(range(256)), "A1 identity must be exact"
    # in_black 30, in_white 220: i <= 30 -> v <= 0 -> 0 ; i = 220 -> (hb-lb)/(hb-lb) = 1 -> 255
    lut = lut_of(P, A, {"rgb": {"in_black": 30, "in_white": 220}})
    assert lut[0] == 0 and lut[29] == 0 and lut[30] == 0 and lut[220] == 255 and lut[255] == 255
    # gamma 2.0 -> inv 0.5: i = 64: sqrt(64/255) = 0.500979 * 255 = 127.75 -> 128 (not 16: v^gamma)
    lut = lut_of(P, A, {"rgb": {"gamma": 2.0}})
    assert lut[64] == 128 and lut[0] == 0 and lut[255] == 255
    # crossed output: ob = 1, ow = 0 -> 1 + v * (0 - 1) = 1 - v
    lut = lut_of(P, A, {"rgb": {"out_black": 255, "out_white": 0}})
    assert lut == [255 - i for i in range(256)]
    # channel first, composite second: r in_white 128 (r: 128 -> 1.0), rgb out_white 100:
    # R 128 -> 1.0 -> 100/255 -> 100 ; G 128 -> 128/255 * 100/255 ... = 128*100/255 = 50.196 -> 50
    px = adj(P, A, {"r": {"in_white": 128}, "rgb": {"out_white": 100}}, [(128, 128, 0)])
    assert px == [(100, 50, 0)], px
    # composite-first would give R: 128 -> 50 -> 50/128 -> 100*... = 99.6; the order above is normative.


@test
def levels_errors():
    for bad in ({"rgb": {"in_black": 100, "in_white": 100}}, {"rgb": {"gamma": 10.0}},
                {"rgb": {"gamma": 0.09}}, {"rgb": {"in_black": 255}}, {"rgb": {"in_white": 0}},
                {"rgb": {"out_black": 256}}, {"a": {}}, {"rgb": {"gama": 1.0}},
                {"rgb": {"in_black": 1.0}}, {"rgb": []}):
        expect_error([{"op": "add_adjustment", "id": "a", "type": "levels", "params": bad}])


# ---------------------------------------------------------------------------------------------
# A2 Curves
# ---------------------------------------------------------------------------------------------

@test
def curves():
    P, A = AF.parse_curves, AF.apply_curves
    assert lut_of(P, A, {}) == list(range(256))
    sp = AF.build_spline([(0, 0), (255, 255)])
    assert sp[2] == [1.0] and sp[3] == [0.0, 0.0] and sp[4] == [0.0]    # B = 1, C = D = 0
    # 3 points (0,0) (100,200) (200,0): H = [100, 100], s = 1, TB = 400,
    # TF = 6 * ((-200/100) - (200/100)) = -24, U = C[1] = -0.06, D = [-0.0006, 0.0006],
    # B[0] = 0 - (1/6)(-6) + 2 = 3.  S(x) on [0,100]: 3x - 0.0001 x^3.
    sp = AF.build_spline([(0, 0), (100, 200), (200, 0)])
    assert abs(sp[3][1] + 0.06) < 1e-15 and abs(sp[2][0] - 3.0) < 1e-12
    lut = lut_of(P, A, {"rgb": [[0, 0], [100, 200], [200, 0]]})
    assert lut[100] == 200          # t = 0 at a knot: exactly Y[1]
    assert lut[20] == 59            # 60 - 0.8 = 59.2
    assert lut[80] == 189           # 240 - 51.2 = 188.8
    assert lut[0] == 0 and lut[210] == 0 and lut[255] == 0     # flat right of X[m] = 200 -> ~0
    # 2 points, flat extension: (48,20)-(208,230): slope 210/160 = 1.3125; x=128 -> 20 + 105 = 125
    lut = lut_of(P, A, {"rgb": [[48, 20], [208, 230]]})
    assert lut[0] == 20 and lut[48] == 20 and lut[128] == 125 and lut[208] == 230 and lut[255] == 230
    # routing: only R curve given
    px = adj(P, A, {"r": [[0, 255], [255, 0]]}, [(10, 10, 10)])
    assert px == [(245, 10, 10)]
    # composite at a non-integer input: g = [[0,0],[2,1],[255,255]] -> S_g(1) = 0.5 exactly?
    # 3 points, not a line; instead check composite(identity) o channel(line) at a half level:
    # g line (0,0)-(254,127): S_g(1) = 0.5; rgb line (0,0)-(1,2): S(0.5) = 1.0 -> q(1/255) = 1
    px = adj(P, A, {"g": [[0, 0], [254, 127]], "rgb": [[0, 0], [1, 2]]}, [(0, 1, 0)])
    assert px == [(0, 1, 0)], px
    # the 4-unknown sweep vs a dense solve (Thomas result must match to rounding)
    pts = [(0, 0), (17, 40), (34, 20), (51, 70), (68, 50), (85, 110), (255, 255)]
    X, Y, B, C, D = AF.build_spline(pts)
    n = len(pts)
    Mx = np.zeros((n - 2, n - 2))
    rhs = np.zeros(n - 2)
    H = [X[i + 1] - X[i] for i in range(n - 1)]
    for k in range(n - 2):
        Mx[k, k] = 2 * (H[k] + H[k + 1])
        if k > 0:
            Mx[k, k - 1] = H[k]
        if k < n - 3:
            Mx[k, k + 1] = H[k + 1]
        rhs[k] = 6 * ((Y[k + 2] - Y[k + 1]) / H[k + 1] - (Y[k + 1] - Y[k]) / H[k])
    sol = np.linalg.solve(Mx, rhs)
    assert np.allclose(sol, C[1:-1], atol=1e-12)
    # continuity at every interior knot (value of segment i-1 at t = H equals Y[i])
    for i in range(1, n - 1):
        t = H[i - 1]
        y = Y[i - 1] + B[i - 1] * t + 0.5 * C[i - 1] * t * t + D[i - 1] * t ** 3 / 6
        assert abs(y - Y[i]) < 1e-9


@test
def curves_errors():
    seventeen = [[i * 15, i * 15] for i in range(17)]
    for bad in ({"rgb": [[0, 0]]}, {"rgb": seventeen}, {"rgb": [[0, 0], [0, 10]]},
                {"rgb": [[10, 0], [5, 10]]}, {"rgb": [[0, 0], [256, 10]]},
                {"rgb": [[0, 0], [255, -1]]}, {"rgb": [[0, 0], [255.0, 255]]},
                {"rgb": [[0, 0, 0], [255, 255]]}, {"rgb": {}}, {"x": [[0, 0], [255, 255]]}):
        expect_error([{"op": "add_adjustment", "id": "a", "type": "curves", "params": bad}])


# ---------------------------------------------------------------------------------------------
# A3 Brightness/Contrast
# ---------------------------------------------------------------------------------------------

@test
def brightness_contrast():
    P, A = AF.parse_bc, AF.apply_bc
    assert lut_of(P, A, {}) == list(range(256))       # slant stated as exactly 1.0
    lut = lut_of(P, A, {"contrast": 1.0})              # A14: clean cut
    assert lut == [0] * 128 + [255] * 128
    assert lut_of(P, A, {"contrast": -1.0}) == [128] * 256     # tan(0) = 0 -> q(0.5)
    lut = lut_of(P, A, {"brightness": 0.5})            # bh 0.25: i=0 -> 0.25 -> 63.75 -> 64
    assert lut[0] == 64 and lut[255] == 255
    lut = lut_of(P, A, {"brightness": -1.0})           # bh -0.5: 255 -> 0.5 -> 128 ; 0 -> 0
    assert lut[255] == 128 and lut[0] == 0
    for bad in ({"brightness": 1.5}, {"contrast": -1.01}, {"contrast": "0"}, {"gain": 1}):
        expect_error([{"op": "add_adjustment", "id": "a", "type": "brightness_contrast",
                       "params": bad}])


# ---------------------------------------------------------------------------------------------
# A4 Hue/Saturation
# ---------------------------------------------------------------------------------------------

@test
def hue_saturation():
    P, A = AF.parse_hs, AF.apply_hs
    # red h6 0, s 1, l .5 ; +60 -> h6 1: m2 1, m1 0: r at h 3 -> fall 1; g at 1 -> n2; b at -1->5 -> n1
    assert adj(P, A, {"hue": 60}, [(255, 0, 0)]) == [(255, 255, 0)]
    # -150 -> dh -2.5 -> 3.5: r at 5.5 -> 0; g at 3.5 -> 0.5 -> 128; b at 1.5 -> 255
    assert adj(P, A, {"hue": -150}, [(255, 0, 0)]) == [(0, 128, 255)]
    assert adj(P, A, {"saturation": -100}, [(255, 0, 0)]) == [(128, 128, 128)]
    assert adj(P, A, {"lightness": -100}, [(10, 200, 30)]) == [(0, 0, 0)]
    assert adj(P, A, {"lightness": 100}, [(10, 200, 30)]) == [(255, 255, 255)]
    # achromatic: hue and saturation have no effect
    assert adj(P, A, {"hue": 120, "saturation": 100}, [(77, 77, 77)]) == [(77, 77, 77)]
    # colorize hue 120 sat 100 on grey 128: l = 0.50196 > .5 -> m2 ~ 1, m1 ~ 0.0039 -> (1,255,1)
    assert adj(P, A, {"colorize": True, "hue": 120, "saturation": 100}, [(128, 128, 128)]) \
        == [(1, 255, 1)]
    # colorize hue 360 wraps to 0 (red); default sat 25 in colorize: s 0.25 on l 0.5 (128? no:
    # use l from white): white -> l = 1 -> m2 = 1, m1 = 1 -> white
    assert adj(P, A, {"colorize": True, "hue": 360}, [(255, 255, 255)]) == [(255, 255, 255)]
    # normative property: identity round trip over all 2^24 colours (chunked)
    t0 = time.time()
    prm = P(FieldReader({}, "p"))
    allc = np.arange(1 << 24, dtype=np.uint32)
    for lo in range(0, 1 << 24, 1 << 22):
        c = allc[lo:lo + (1 << 22)]
        r = (c >> 16).astype(np.uint8)
        g = ((c >> 8) & 255).astype(np.uint8)
        b = (c & 255).astype(np.uint8)
        r2, g2, b2 = A(prm, r, g, b)
        assert (r2 == r).all() and (g2 == g).all() and (b2 == b).all(), "A4 round trip broken"
    print("    (A4 2^24 round trip: %.1f s)" % (time.time() - t0))
    # vectorised == scalar over random colours and parameters
    rng = np.random.default_rng(1)
    cols = rng.integers(0, 256, size=(4000, 3))
    for hue, sat, light in ((37.5, 55.0, -20.0), (-179.0, -60.0, 70.0), (180.0, 100.0, 0.0)):
        got = adj(P, A, {"hue": hue, "saturation": sat, "lightness": light},
                  [tuple(int(v) for v in c) for c in cols])
        dh, ks, vl = hue / 60.0, 1.0 + (sat / 100.0), light / 100.0
        for (R, G, Bc), g in zip(cols, got):
            h6, s, l = AF.rgb_to_hsl_scalar(R / 255.0, G / 255.0, Bc / 255.0)
            h6 = h6 + dh
            if h6 >= 6.0:
                h6 -= 6.0
            elif h6 < 0.0:
                h6 += 6.0
            l = l * (vl + 1.0) if vl < 0.0 else l + (vl * (1.0 - l))
            s = min(max(s * ks, 0.0), 1.0)
            exp = tuple(q(v) for v in AF.hsl_to_rgb_scalar(h6, s, l))
            assert g == exp, (R, G, Bc, g, exp)
    for bad in ({"hue": 181}, {"hue": -1, "colorize": True}, {"saturation": 101},
                {"saturation": -1, "colorize": True}, {"lightness": -100.5}, {"colorize": 1}):
        expect_error([{"op": "add_adjustment", "id": "a", "type": "hue_saturation",
                       "params": bad}])


# ---------------------------------------------------------------------------------------------
# A5 Black & White
# ---------------------------------------------------------------------------------------------

@test
def black_white():
    P, A = AF.parse_bw, AF.apply_bw
    got = adj(P, A, {}, [(255, 0, 0), (255, 255, 0), (0, 255, 0), (0, 255, 255), (0, 0, 255),
                         (255, 0, 255), (255, 255, 255), (77, 77, 77), (200, 100, 50)])
    # reds .4*255 = 102; yellows 153; greens 102; cyans 153; blues 51; magentas 204; white; grey;
    # (200,100,50): mn 50, md 100, mx 200, ws = wY (B is min), wp = wR: 50 + 30 + 40 = 120
    assert [g[0] for g in got] == [102, 153, 102, 153, 51, 204, 255, 77, 120], got
    assert all(g[0] == g[1] == g[2] for g in got)
    # out-of-range grey clamps (reds -200: -510 -> 0 ; reds 300: 765 -> 255)
    assert adj(P, A, {"reds": -200}, [(255, 0, 0)]) == [(0, 0, 0)]
    assert adj(P, A, {"reds": 300}, [(255, 0, 0)]) == [(255, 255, 255)]
    # tint #ff0000 (h6 0, s 1): grey 128/255 -> l > .5 -> m2 ~ 1, m1 ~ 1/255: (255, 1, 1)
    assert adj(P, A, {"tint": "#FF0000"}, [(128, 128, 128)]) == [(255, 1, 1)]
    assert adj(P, A, {"tint": "#ff0000"}, [(255, 255, 255)]) == [(255, 255, 255)]
    assert adj(P, A, {"tint": None}, [(255, 0, 0)]) == [(102, 102, 102)]
    for bad in ({"tint": "#ff000080"}, {"tint": "red"}, {"tint": 5}, {"reds": 301},
                {"blues": -200.5}, {"cyan": 3}):
        expect_error([{"op": "add_adjustment", "id": "a", "type": "black_white", "params": bad}])


# ---------------------------------------------------------------------------------------------
# A7 Posterize, A8 Threshold, A9 add_adjustment
# ---------------------------------------------------------------------------------------------

@test
def posterize():
    P, A = AF.parse_posterize, AF.apply_posterize
    lut = lut_of(P, A, {})            # levels 4, kd 3
    # 42: t = .494 -> 0 ; 43: .506 -> 1 -> 85 ; 128: 1.506 -> 2 -> 170 ; 255 -> 3 -> 255
    assert lut[0] == 0 and lut[42] == 0 and lut[43] == 85 and lut[128] == 170 and lut[255] == 255
    assert sorted(set(lut)) == [0, 85, 170, 255]
    lut = lut_of(P, A, {"levels": 2})
    assert lut == [0] * 128 + [255] * 128
    lut = lut_of(P, A, {"levels": 255})    # 255 levels over 256 inputs: one pair collides
    assert len(set(lut)) == 255 and lut[0] == 0 and lut[255] == 255
    for bad in ({"levels": 1}, {"levels": 256}, {"levels": 4.0}):
        expect_error([{"op": "add_adjustment", "id": "a", "type": "posterize", "params": bad}])


@test
def threshold():
    P, A = AF.parse_threshold, AF.apply_threshold
    # white: 255000 + 500 = 255500 // 1000 = 255 ; red: 76245 + 500 -> 76 ; grey v -> v
    got = adj(P, A, {}, [(255, 255, 255), (128, 128, 128), (127, 127, 127), (255, 0, 0)])
    assert got == [(255,) * 3, (255,) * 3, (0,) * 3, (0,) * 3]
    assert adj(P, A, {"level": 76}, [(255, 0, 0)]) == [(255,) * 3]      # >= , not >
    assert adj(P, A, {"level": 77}, [(255, 0, 0)]) == [(0,) * 3]
    for bad in ({"level": 0}, {"level": 256}, {"level": 128.0}):
        expect_error([{"op": "add_adjustment", "id": "a", "type": "threshold", "params": bad}])


@test
def add_adjustment_in_stack():
    # 2x1: opaque red + half-alpha grey; levels out_white 0 -> black; alpha kept, canonical kept
    out = render([{"op": "add_layer", "id": "p", "fill": "solid", "color": "#ff0000ff"},
                  {"op": "add_adjustment", "id": "a", "type": "threshold"}], w=2, h=1)
    assert tup(out) == [(0, 0, 0, 255)] * 2        # red luma 76 < 128
    out = render([{"op": "add_layer", "id": "p", "fill": "solid", "color": "#80808080"},
                  {"op": "add_adjustment", "id": "a", "type": "posterize",
                   "params": {"levels": 2}}], w=1, h=1)
    assert tup(out) == [(255, 255, 255, 128)]
    expect_error([{"op": "add_adjustment", "id": "a", "type": "sepia"}])
    expect_error([{"op": "add_adjustment", "id": "a"}])
    expect_error([{"op": "add_adjustment", "id": "a", "type": "invert", "params": {"x": 1}}])


# ---------------------------------------------------------------------------------------------
# B0 framework
# ---------------------------------------------------------------------------------------------

@test
def coverage_lerp():
    O = row([(255, 0, 0, 255), (200, 100, 50, 255), (0, 0, 0, 0), (1, 2, 3, 4), (1, 2, 3, 4)])
    F = row([(0, 0, 255, 255), (0, 0, 0, 0), (255, 0, 0, 255), (9, 9, 9, 9), (9, 9, 9, 9)])
    M = np.array([[128, 128, 128, 0, 255]], dtype=np.uint8)
    got = tup(AF.coverage_lerp(O, F, M))
    # red->blue at m = 128/255: (1-m)*255 = 127, m*255 = 128, a = 1 -> (127, 0, 128, 255)
    # O opaque, F transparent: a = 1-m -> 127; colour (x*y)/y ~ x -> (200,100,50,127)
    # O transparent, F red: premultiplied -> colour stays 255 (a straight lerp would give 128)
    assert got == [(127, 0, 128, 255), (200, 100, 50, 127), (255, 0, 0, 128),
                   (1, 2, 3, 4), (9, 9, 9, 9)], got


def _doc_script(ops, w, h, bg="#00000000"):
    return render(ops, w=w, h=h, bg=bg)


@test
def coverage_sources_and_lock():
    base = [{"op": "add_layer", "id": "p", "fill": "solid", "color": "#ff0000ff"}]
    # ramp h on 4 px: M = 0, 85, 170, 255 ; offset wrap 1 (red everywhere) -> no change at all
    # use offset transparent dx=4 -> F transparent everywhere: alpha = q(1 - m)
    out = _doc_script(base + [{"op": "filter_offset", "layer": "p", "dx": 4,
                               "coverage": {"src": "ramp", "dir": "h"}}], 4, 1)
    assert tup(out) == [(255, 0, 0, 255), (255, 0, 0, 170), (255, 0, 0, 85), (0, 0, 0, 0)], tup(out)
    out = _doc_script(base + [{"op": "filter_offset", "layer": "p", "dx": 4,
                               "coverage": {"src": "ramp", "dir": "v"}}], 1, 1)
    assert tup(out) == [(0, 0, 0, 0)]                     # H == 1 -> 255
    # rect clipped past the canvas, value 51 -> alpha q(1 - 0.2) = 204
    out = _doc_script(base + [{"op": "filter_offset", "layer": "p", "dx": 4,
                               "coverage": {"src": "rect", "x": 2, "y": -5, "w": 10, "h": 6,
                                            "value": 51}}], 4, 2)
    assert tup(out) == [(255, 0, 0, 255)] * 2 + [(255, 0, 0, 204)] * 2 + [(255, 0, 0, 255)] * 4
    # w = 0 rect: nothing selected
    out = _doc_script(base + [{"op": "filter_offset", "layer": "p", "dx": 4,
                               "coverage": {"src": "rect", "x": 0, "y": 0, "w": 0, "h": 1}}], 2, 1)
    assert tup(out) == [(255, 0, 0, 255)] * 2
    # selection source with no selection = 255 everywhere
    out = _doc_script(base + [{"op": "filter_offset", "layer": "p", "dx": 1,
                               "coverage": {"src": "selection"}}], 2, 1)
    assert tup(out) == [(0, 0, 0, 0), (255, 0, 0, 255)]
    # lock transparency: offset of [red, clear] by 1 wrap -> [clear, red]; lock restores alpha:
    # px0 keeps alpha 255 with F colour (0,0,0) ; px1 had alpha 0 -> stays (0,0,0,0)
    ops = [{"op": "add_layer", "id": "p", "fill": "solid", "color": "#ff0000ff", "rect": [0, 0, 1, 1]},
           {"op": "lock_transparency", "layer": "p", "value": True},
           {"op": "filter_offset", "layer": "p", "dx": 1, "mode": "wrap"}]
    out = _doc_script(ops, 2, 1)
    assert tup(out) == [(0, 0, 0, 255), (0, 0, 0, 0)], tup(out)
    for badcov in ({"src": "rect", "x": 0, "y": 0, "w": 1, "h": 1, "value": 256},
                   {"src": "rect", "x": 0, "y": 0, "w": -1, "h": 1},
                   {"src": "rect", "x": 0, "y": 0, "w": 1},
                   {"src": "ramp"}, {"src": "ramp", "dir": "d"}, {"src": "all"},
                   {"src": "selection", "value": 3}, {"src": "ramp", "dir": "h", "x": 0}, None):
        expect_error(base + [{"op": "filter_offset", "layer": "p", "coverage": badcov}], w=2, h=1)
    # wrong targets
    expect_error([{"op": "add_adjustment", "id": "a", "type": "invert"},
                  {"op": "filter_gaussian_blur", "layer": "a"}])
    expect_error([{"op": "add_group", "id": "g"}, {"op": "filter_high_pass", "layer": "g"}])
    expect_error([{"op": "filter_offset", "layer": "nope"}])
    expect_error(base + [{"op": "filter_offset"}])


# ---------------------------------------------------------------------------------------------
# B1 Gaussian
# ---------------------------------------------------------------------------------------------

@test
def gauss_widths():
    table = {0.5: [1, 1, 1], 1.0: [1, 1, 3], 1.5: [3, 3, 3], 2.0: [3, 3, 5], 3.0: [5, 5, 7],
             4.5: [9, 9, 9], 8.0: [15, 15, 17], 25: [49, 49, 51], 250: [499, 499, 501]}
    for s, w in table.items():
        assert AF.box_widths(float(s)) == w, (s, AF.box_widths(float(s)))


@test
def gauss_pipeline():
    O = row([(0, 0, 0, 0), (0, 0, 0, 0), (255, 0, 0, 255), (0, 0, 0, 0), (0, 0, 0, 0)])
    # widths [1,1,3]; H pass: x=1..3 -> (65025 + 1) // 3 = 21675 ; V passes on h=1:
    #   clamp: (3P + 1) // 3 = P -> a = 1/3 -> 85, C = 1 -> (255, 0, 0, 85)
    #   transparent: (P + 1) // 3 = 7225 -> a = 7225/65025 * 255 = 28.33 -> 28
    got = tup(AF.gaussian_blur(O, 1.0, "clamp"))
    assert got == [(0, 0, 0, 0)] + [(255, 0, 0, 85)] * 3 + [(0, 0, 0, 0)], got
    got = tup(AF.gaussian_blur(O, 1.0, "transparent"))
    assert got == [(0, 0, 0, 0)] + [(255, 0, 0, 28)] * 3 + [(0, 0, 0, 0)], got
    # grey step 100|160 opaque, radius 1: x=2: (2*25500 + 40800 + 1)//3 = 30600 -> 120 ;
    # x=3: (25500 + 2*40800 + 1)//3 = 35700 -> 140
    S = row([(100, 100, 100, 255)] * 3 + [(160, 160, 160, 255)] * 3)
    got = [p[0] for p in tup(AF.gaussian_blur(S, 1.0, "clamp"))]
    assert got == [100, 100, 120, 140, 160, 160], got
    # radius 0.5 = identity on arbitrary content
    rng = np.random.default_rng(3)
    X = rng.integers(0, 256, size=(9, 11, 4)).astype(np.uint8)
    X[X[..., 3] == 0] = 0
    assert (AF.gaussian_blur(X, 0.5, "clamp") == X).all()
    # box pass: running sum == direct sum (clamp and transparent), odd widths, small N < r
    P = rng.integers(0, 65026, size=(3, 5)).astype(np.int64)
    for edge in ("clamp", "transparent"):
        for w in (3, 5, 13):
            r = (w - 1) // 2
            got = AF.box_pass(P, w, 1, edge)
            for y in range(3):
                for x in range(5):
                    s = 0
                    for j in range(x - r, x + r + 1):
                        if edge == "clamp":
                            s += int(P[y, min(max(j, 0), 4)])
                        elif 0 <= j < 5:
                            s += int(P[y, j])
                    assert got[y, x] == (s + r) // w


# ---------------------------------------------------------------------------------------------
# B2 Motion blur
# ---------------------------------------------------------------------------------------------

@test
def motion():
    N, taps = AF.motion_taps(0.0, 1.0)
    assert N == 2 and taps == [(-1, 0.5, 0, 0.0), (0, 0.5, 0, 0.0)], taps
    N, taps = AF.motion_taps(90.0, 2.0)     # cos snap -> ox 0 ; oy = -2 ; t = -.5, 0, .5
    assert N == 3 and [(t[0], t[1]) for t in taps] == [(0, 0.0)] * 3
    assert [(t[2], t[3]) for t in taps] == [(1, 0.0), (0, 0.0), (-1, 0.0)], taps
    N, _ = AF.motion_taps(30.0, 7.5)
    assert N == 9
    # impulse, horizontal distance 1, transparent: acc_a = 0.5, 1.0, 0.5 over N=2 ->
    # a = .25 -> 64, .5 -> 128, .25 -> 64 ; colour pc/a = 1 -> 255
    O = row([(0, 0, 0, 0), (0, 0, 0, 0), (255, 0, 0, 255), (0, 0, 0, 0), (0, 0, 0, 0)])
    got = tup(AF.motion_blur(O, 0.0, 1.0, "transparent"))
    assert got == [(0, 0, 0, 0), (255, 0, 0, 64), (255, 0, 0, 128), (255, 0, 0, 64),
                   (0, 0, 0, 0)], got
    # vertical 3-tap box on a column impulse -> a = 1/3 -> 85 at y = 1..3
    Oc = O.reshape(5, 1, 4)
    got = tup(AF.motion_blur(Oc, 90.0, 2.0, "transparent"))
    assert got == [(0, 0, 0, 0)] + [(255, 0, 0, 85)] * 3 + [(0, 0, 0, 0)], got
    # -45 smears toward bottom-right / top-left: oy = +d sin45 (y down) and ox = +d cos45
    N, taps = AF.motion_taps(-45.0, 2.0)
    assert taps[0][0] < 0 and taps[0][2] < 0 and taps[-1][0] >= 0 and taps[-1][2] >= 0
    # clamp edge: opaque constant layer stays constant
    C = np.full((6, 7, 4), 200, dtype=np.uint8)
    C[..., 3] = 255
    assert (AF.motion_blur(C, 33.0, 5.3, "clamp") == C).all()


# ---------------------------------------------------------------------------------------------
# B3 USM, B5 High pass
# ---------------------------------------------------------------------------------------------

@test
def usm_high_pass():
    S = row([(100, 100, 100, 255)] * 3 + [(160, 160, 160, 255)] * 3)
    # blur = [100,100,120,140,160,160] ; amount 100 (k 1): x=2 d=-20 -> 80 ; x=3 d=20 -> 180
    got = [p[0] for p in tup(AF.unsharp_mask(S, 100.0, 1.0, 0, "clamp"))]
    assert got == [100, 100, 80, 180, 160, 160], got
    got = [p[0] for p in tup(AF.unsharp_mask(S, 100.0, 1.0, 20, "clamp"))]     # 20 < 20 false
    assert got == [100, 100, 80, 180, 160, 160], got
    got = [p[0] for p in tup(AF.unsharp_mask(S, 100.0, 1.0, 21, "clamp"))]     # gated
    assert got == [100, 100, 100, 160, 160, 160], got
    # amount 150 (k 1.5): 100 - 30 = 70 ; 160 + 30 = 190
    got = [p[0] for p in tup(AF.unsharp_mask(S, 150.0, 1.0, 0, "clamp"))]
    assert got == [100, 100, 70, 190, 160, 160], got
    # alpha never sharpened
    Sa = S.copy()
    Sa[0, 3:, 3] = 40
    assert (AF.unsharp_mask(Sa, 300.0, 1.0, 0, "clamp")[..., 3] == Sa[..., 3]).all()
    # high pass: flat -> q(0.5) = 128 ; alpha kept
    got = tup(AF.high_pass(S, 1.0, "clamp"))
    assert got[0] == (128, 128, 128, 255) and got[5] == (128, 128, 128, 255)
    # step: every d != 0 is a real-arithmetic tie (d + 127.5); only check the side of 128
    assert got[2][0] < 128 < got[3][0]


# ---------------------------------------------------------------------------------------------
# B4 noise
# ---------------------------------------------------------------------------------------------

@test
def noise():
    O = np.full((3, 5, 4), 128, dtype=np.uint8)
    O[..., 3] = 255
    O[0, 0] = 0                                        # a transparent pixel stays canonical
    assert (AF.add_noise(O, 0.0, "uniform", False, 5) == O).all()
    assert (AF.add_noise(O, 0.0, "gaussian", True, 5) == O).all()
    for dist in ("uniform", "gaussian"):
        for mono in (False, True):
            F = AF.add_noise(O, 37.0, dist, mono, 12345)
            assert (F[..., 3] == O[..., 3]).all() and tuple(F[0, 0]) == (0, 0, 0, 0)
            A = (37.0 * 255.0) / 100.0
            sigma = A / 1.7320508075688772
            for (y, x) in ((1, 2), (2, 4)):
                for c in range(3):
                    if dist == "uniform":
                        u = unit(pixel_hash(12345, x, y, 0 if mono else c))
                        n = A * ((2.0 * u) - 1.0)
                    else:
                        k1, k2 = (0, 1) if mono else (2 * c, 2 * c + 1)
                        u1 = unit(pixel_hash(12345, x, y, k1))
                        u2 = unit(pixel_hash(12345, x, y, k2))
                        n = sigma * (math.sqrt(-2.0 * math.log(1.0 - u1))
                                     * math.cos(6.283185307179586 * u2))
                    assert int(F[y, x, c]) == q((128.0 + n) / 255.0), (dist, mono, x, y, c)
            if mono:
                assert (F[1:, :, 0] == F[1:, :, 1]).all() and (F[1:, :, 1] == F[1:, :, 2]).all()
    for bad in ({"amount": 400.5}, {"distribution": "poisson"}, {"seed": 2 ** 53},
                {"seed": -1}, {"monochromatic": 0}):
        e = {"op": "filter_add_noise", "layer": "p"}
        e.update(bad)
        expect_error([{"op": "add_layer", "id": "p"}, e])


# ---------------------------------------------------------------------------------------------
# B6 Offset
# ---------------------------------------------------------------------------------------------

@test
def offset():
    O = row([(1, 1, 1, 255), (2, 2, 2, 255), (3, 3, 3, 255), (4, 4, 4, 255)])
    f = lambda dx, mode: [p[0] for p in tup(AF.offset(O, dx, 0, mode))]  # noqa: E731
    assert f(1, "transparent") == [0, 1, 2, 3]
    assert f(-1, "transparent") == [2, 3, 4, 0]
    assert f(1, "repeat") == [1, 1, 2, 3]
    assert f(-10, "repeat") == [4, 4, 4, 4]
    assert f(1, "wrap") == [4, 1, 2, 3]
    assert f(-7, "wrap") == [4, 1, 2, 3]            # sx = x + 7, mod 4 -> x + 3
    assert f(65536, "wrap") == [1, 2, 3, 4]
    Oc = O.reshape(4, 1, 4)
    assert [p[0] for p in tup(AF.offset(Oc, 0, 2, "wrap"))] == [3, 4, 1, 2]
    for bad in ({"dx": 1.0}, {"dy": 65537}, {"mode": "mirror"}):
        e = {"op": "filter_offset", "layer": "p"}
        e.update(bad)
        expect_error([{"op": "add_layer", "id": "p"}, e])


@test
def filter_field_errors():
    L = {"op": "add_layer", "id": "p"}
    for e in ({"op": "filter_gaussian_blur", "layer": "p", "radius": 0.09},
              {"op": "filter_gaussian_blur", "layer": "p", "radius": 250.5},
              {"op": "filter_gaussian_blur", "layer": "p", "edge": "wrap"},
              {"op": "filter_motion_blur", "layer": "p", "distance": 0.5},
              {"op": "filter_motion_blur", "layer": "p", "angle": 361},
              {"op": "filter_unsharp_mask", "layer": "p", "amount": 0.5},
              {"op": "filter_unsharp_mask", "layer": "p", "threshold": 1.5},
              {"op": "filter_high_pass", "layer": "p", "radius": 0},
              {"op": "filter_high_pass", "layer": "p", "amount": 3}):
        expect_error([L, e])


# ---------------------------------------------------------------------------------------------
# goldens
# ---------------------------------------------------------------------------------------------

def render_goldens(outdir=None):
    from PIL import Image
    paths = sorted(glob.glob(os.path.join(HERE, "..", "scripts", "adjust_filters", "*.json")))
    fails = []
    tot = 0.0
    for pth in paths:
        name = os.path.splitext(os.path.basename(pth))[0]
        with open(pth) as fh:
            s = json.load(fh)
        expect = s.pop("expect", None)
        s.pop("equal_to", None)
        t0 = time.time()
        try:
            rgba = render_script_text(json.dumps(s))
            err = None
        except ScriptError as e:
            rgba, err = None, str(e)
        dt = time.time() - t0
        tot += dt
        if expect == "error":
            status = "ok (error: %s)" % err if err else "FAIL (rendered, expected error)"
        else:
            status = "ok" if err is None else "FAIL (%s)" % err
            if err is None and outdir:
                Image.fromarray(np.ascontiguousarray(rgba)).save(os.path.join(outdir, name + ".png"))
        if status.startswith("FAIL"):
            fails.append(name)
        print("  %-40s %7.3f s  %s" % (name, dt, status[:110]))
    print("goldens: %d scripts, %d failures, total %.2f s" % (len(paths), len(fails), tot))
    return fails


def main(argv):
    t_all = time.time()
    bad = 0
    for fn in T:
        t0 = time.time()
        try:
            fn()
            print("ok    %-28s %.2f s" % (fn.__name__, time.time() - t0))
        except Exception:
            bad += 1
            import traceback
            traceback.print_exc()
            print("FAIL  %s" % fn.__name__)
    print("self-checks: %d/%d passed in %.1f s" % (len(T) - bad, len(T), time.time() - t_all))
    if "--goldens" in argv:
        i = argv.index("--goldens")
        outdir = argv[i + 1] if len(argv) > i + 1 else None
        if outdir:
            os.makedirs(outdir, exist_ok=True)
        if render_goldens(outdir):
            bad += 1
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
