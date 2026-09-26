#!/usr/bin/env python3
"""Self-checks for the doc 30 reference module (rc/ops_geometry.py).

Run:  python3 tests/reference/test_geometry.py [--goldens OUTDIR]
  or: python3 -m pytest tests/reference/test_geometry.py

Expected values are worked out by hand from docs/math/30-geometry-selection.md (arithmetic in
the comments); none was produced by running the reference. The golden pass (--goldens, or the
test_goldens_* functions) renders every tests/scripts/geometry/*.json, checks `expect: error`
scripts fail at the op the script targets, and checks cross-script identities the doc implies.
"""

import glob
import json
import os
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from rc import ops_geometry as G  # noqa: E402
from rc.core import ScriptError  # noqa: E402
from rc.registry import load_all  # noqa: E402
from rc.runner import render_script_text, run_script  # noqa: E402

load_all()

GOLDEN_DIR = os.path.join(HERE, "..", "scripts", "geometry")


def script(ops, w=4, h=4, bg="#00000000"):
    return {"canvas": {"w": w, "h": h, "bg": bg}, "ops": ops, "out": "png8"}


def doc_of(ops, **kw):
    return run_script(json.loads(json.dumps(script(ops, **kw))))


def layer(doc, i="L"):
    return doc.find(i).rgba


def expect_error(ops, **kw):
    try:
        doc_of(ops, **kw)
    except ScriptError:
        return
    raise AssertionError("expected a script error for %r" % (ops,))


L = {"op": "add_layer", "id": "L"}


def rect(x, y, w, h, **kw):
    d = {"op": "select_rect", "x": x, "y": y, "w": w, "h": h}
    d.update(kw)
    return d


def fill(color, lid="L", **kw):
    d = {"op": "fill_selection", "layer": lid, "color": color}
    d.update(kw)
    return d


# ---------------------------------------------------------------------------------------------
# §1.3 / §3 rasterisation
# ---------------------------------------------------------------------------------------------

def test_rect_aa_coverage():
    # SEL-02 rect 8.25,8.5,40.3,23.75 aa. x1 = 48.55, y1 = 32.25 (both doubles).
    # pixel col 8: o(i) >= 0.25 -> 2i+1 >= 8 -> i = 4..15 -> 12 samples
    # pixel row 8: o(j) >= 0.5 -> j = 8..15 -> 8 samples                     n = 96  -> 95.625 -> 96
    # col 48: 48 + o < 48.55 -> 2i+1 < 17.6 -> i = 0..8 -> 9 samples; row 32: o < 0.25 -> 4 samples
    d = doc_of([rect(8.25, 8.5, 40.3, 23.75, antialias=True)], w=64, h=64)
    S = d.sel
    assert S[8, 8] == 96, S[8, 8]
    assert S[9, 9] == 255
    assert S[9, 48] == 143, S[9, 48]          # 9*16 = 144 -> 144/256*255 = 143.4375 -> 143
    assert S[32, 48] == 36, S[32, 48]         # 9*4 = 36 -> 35.859 -> 36
    assert S[32, 8] == 48, S[32, 8]           # 12*4 = 48 -> 47.8125 -> 48
    assert S[8, 7] == 0 and S[7, 8] == 0 and S[8, 49] == 0 and S[33, 8] == 0


def test_rect_non_aa_centre():
    # SEL-03: centres x+0.5 in [10.5, 30.7) -> x = 10..30 ; y+0.5 in [6.4, 37.0) -> y = 6..36
    d = doc_of([rect(10.5, 6.4, 20.2, 30.6)], w=64, h=64)
    ys, xs = np.nonzero(d.sel)
    assert (xs.min(), xs.max(), ys.min(), ys.max()) == (10, 30, 6, 36)
    assert int((d.sel == 255).sum()) == 21 * 31


def test_rect_integer_half_open():
    d = doc_of([rect(1, 1, 2, 2, antialias=True)])
    exp = np.zeros((4, 4), np.uint8)
    exp[1:3, 1:3] = 255
    assert (d.sel == exp).all()


def test_ellipse_small():
    # ellipse box (0,0,2,2) non-AA on 2x2: centre (1,1), r = 1; pixel centres (0.5,0.5) etc:
    # dx = -0.5, dy = -0.5 -> 0.5 <= 1 -> inside; all four inside.
    d = doc_of([{"op": "select_ellipse", "x": 0, "y": 0, "w": 2, "h": 2, "antialias": False}], w=2, h=2)
    assert (d.sel == 255).all()
    # box (0,0,1,1) on a 3x3: only pixel (0,0) centre (0.5,0.5) = centre of ellipse -> inside
    d = doc_of([{"op": "select_ellipse", "x": 0, "y": 0, "w": 1, "h": 1, "antialias": False}], w=3, h=3)
    assert d.sel[0, 0] == 255 and int(d.sel.sum()) == 255


def test_polygon_half_open_and_edges():
    # triangle (0,0),(4,0),(0,4) non-AA: inside iff x+0.5 < 4-(y+0.5) i.e. x + y <= 2 (x+y = 3 is
    # exactly on the hypotenuse: px == xc -> outside). 6 pixels.
    d = doc_of([{"op": "select_polygon", "points": [[0, 0], [4, 0], [0, 4]], "antialias": False}])
    exp = np.array([[1 if x + y <= 2 else 0 for x in range(4)] for y in range(4)], np.uint8) * 255
    assert (d.sel == exp).all(), d.sel
    # zero-area polygon rasterises to all 0
    d = doc_of([{"op": "select_polygon", "points": [[0, 0], [4, 4], [2, 2]]}])
    assert not d.sel.any()


def test_polygon_even_odd():
    # SEL-08 pentagram: inner pentagon is empty; a spike tip is selected.
    pts = [[32, 4], [48, 58], [5, 24], [59, 24], [16, 58]]
    d = doc_of([{"op": "select_polygon", "points": pts}], w=64, h=64)
    assert d.sel[34, 32] == 0          # centre of the star (inside the inner pentagon)
    assert d.sel[12, 32] == 255        # top spike, (32.5, 12.5) well inside
    assert d.sel[26, 16] == 255        # left spike


def test_polygon_supersample_matches_bruteforce():
    # brute-force PNPOLY per sample (independent loop) on a small concave polygon
    pts = [(-1.3, 0.7), (5.2, 1.1), (2.4, 2.9), (5.6, 5.3), (0.2, 4.6)]
    W = H = 6
    got = G.raster_polygon(W, H, pts, True)
    for y in range(H):
        for x in range(W):
            n = 0
            for j in range(16):
                for i in range(16):
                    px = x + (2 * i + 1) / 32.0
                    py = y + (2 * j + 1) / 32.0
                    c = False
                    for k in range(len(pts)):
                        xi, yi = pts[k]
                        xj, yj = pts[k - 1]
                        if (yi > py) != (yj > py):
                            if px < (((xj - xi) * (py - yi)) / (yj - yi)) + xi:
                                c = not c
                    n += c
            assert got[y, x] == (255 * n + 128) // 256, (x, y, got[y, x], n)


# ---------------------------------------------------------------------------------------------
# §4 / §5 combine and state
# ---------------------------------------------------------------------------------------------

def test_combine_modes():
    S = np.array([[0, 100, 200, 255]], np.uint8)
    B = np.array([[50, 150, 100, 255]], np.uint8)
    assert G.combine(S, B, "new").tolist() == [[50, 150, 100, 255]]
    assert G.combine(S, B, "add").tolist() == [[50, 150, 200, 255]]
    assert G.combine(S, B, "subtract").tolist() == [[0, 0, 100, 0]]
    assert G.combine(S, B, "intersect").tolist() == [[0, 100, 100, 255]]
    ell = {"op": "select_ellipse", "x": 0.3, "y": 0.2, "w": 3.1, "h": 3.4}
    d = doc_of([ell, dict(ell, mode="subtract")])
    assert not d.sel.any()                                   # subtract(A, A) = empty
    a = doc_of([ell]).sel
    assert (doc_of([ell, dict(ell, mode="add")]).sel == a).all()
    assert (doc_of([ell, dict(ell, mode="intersect")]).sel == a).all()


def test_deselect_reselect_inverse():
    d = doc_of([rect(0, 0, 2, 2), {"op": "deselect"}, rect(2, 2, 2, 2, mode="add"),
                {"op": "deselect"}, {"op": "reselect"}])
    exp = np.zeros((4, 4), np.uint8)
    exp[2:, 2:] = 255
    assert (d.sel == exp).all()
    d = doc_of([{"op": "deselect"}, {"op": "reselect"}])      # nothing saved: no-op
    assert not d.sel.any() and d.sel_saved is None
    d = doc_of([{"op": "select_inverse"}])
    assert (d.sel == 255).all()
    d = doc_of([{"op": "select_all"}, {"op": "undo"}])
    assert not d.sel.any()


# ---------------------------------------------------------------------------------------------
# §7 feather, §8 expand / contract
# ---------------------------------------------------------------------------------------------

def test_box_widths():
    for r, exp in ((2, [1, 1, 3]), (4, [3, 3, 5]), (8, [7, 7, 9]), (10, [9, 9, 11])):
        assert G.box_widths(r / 2.0) == exp, (r, G.box_widths(r / 2.0))
    assert G.box_widths(125.0) == [249, 249, 251] or max(G.box_widths(125.0)) <= 251


def test_feather_hand():
    # S = [0,0,255,0,0] (5x1), r = 2 -> widths [1,1,3]; w=3 horizontal: [0,255,255,255,0];
    # vertical (H = 1, edge replicate) x3: [0,765,765,765,0]; D = 9; (2*765+9)//18 = 85
    S = np.array([[0, 0, 255, 0, 0]], np.uint8)
    assert G.feather_bytes(S, 2.0).tolist() == [[0, 85, 85, 85, 0]]
    d = doc_of([{"op": "select_all"}, {"op": "feather", "radius": 8}], w=16, h=16)
    assert (d.sel == 255).all()                              # edge replicate
    d = doc_of([{"op": "feather", "radius": 8}])
    assert not d.sel.any()                                   # empty: no-op


def test_expand_contract():
    S = np.zeros((11, 11), np.uint8)
    S[5, 5] = 255
    E = G.morph(S, 2, True)
    # disc(2): (0,0), 4 axial at 1, 4 diagonal (1,1), 4 axial at 2 -> 13 pixels
    assert int((E == 255).sum()) == 13 and E[5, 7] == 255 and E[6, 6] == 255 and E[7, 7] == 0
    S2 = np.full((7, 7), 255, np.uint8)
    S2[3, 3] = 0
    C = G.morph(S2, 1, False)
    assert int((C == 0).sum()) == 5                          # plus-shaped hole
    assert (G.morph(np.full((5, 5), 255, np.uint8), 3, False) == 255).all()   # border stays
    # soft value preserved by max
    S3 = np.zeros((5, 5), np.uint8)
    S3[2, 2] = 128
    assert G.morph(S3, 1, True)[1, 2] == 128


# ---------------------------------------------------------------------------------------------
# §6 region
# ---------------------------------------------------------------------------------------------

def _row(*px):
    return np.array([list(px)], np.uint8)


def test_region_fringe_and_tolerance():
    # d = 0 seed, 50, 45 (not a neighbour of C), tol 40 aa:
    # pixel 1: d = 50 -> a = 1.5 - 1.25 = 0.25 -> q(0.5) = 127.5 -> 128
    # pixel 2: not adjacent to C (C = {0}) -> 0
    rgba = _row((0, 0, 0, 255), (50, 0, 0, 255), (45, 0, 0, 255))
    assert G.region(rgba, 0, 0, 40, True, True).tolist() == [[255, 128, 0]]
    # d = 45 adjacent: a = 1.5 - 1.125 = 0.375 -> q(0.75) = 191.25 -> 191 ; d = 60: a = 0 -> 0
    rgba = _row((45, 0, 0, 255), (0, 0, 0, 255), (0, 60, 0, 255))
    assert G.region(rgba, 1, 0, 40, True, True).tolist() == [[191, 255, 0]]
    # exact boundary d == t matches; alpha counts as a channel
    rgba = _row((0, 0, 0, 255), (20, 0, 0, 255), (0, 0, 0, 234))
    assert G.region(rgba, 0, 0, 20, True, False).tolist() == [[255, 255, 0]]
    # t = 0: no fringe
    assert G.region(rgba, 0, 0, 0, True, True).tolist() == [[255, 0, 0]]
    # seed outside the canvas: all 0
    assert not G.region(rgba, 5, 0, 255, True, True).any()


def test_region_8_connected():
    a = np.zeros((3, 4, 4), np.uint8)
    a[0, 0] = a[1, 1] = a[2, 2] = (0, 0, 0, 255)             # diagonal chain
    a[0, 3] = (0, 0, 0, 255)                                 # isolated corner (not 8-adjacent)
    Rg = G.region(a, 0, 0, 0, True, False)
    assert Rg[0, 0] == Rg[1, 1] == Rg[2, 2] == 255 and Rg[0, 3] == 0 and int((Rg > 0).sum()) == 3
    assert G.region(a, 0, 0, 0, False, False)[0, 3] == 255   # global mode


# ---------------------------------------------------------------------------------------------
# §1.2 paint_over, §10, §17, §18
# ---------------------------------------------------------------------------------------------

def test_paint_over_hand():
    # "#0000ff80" over opaque white: as = 128/255; ao ~ 1 -> 255; blue (as + 1*1*(1 - as))/ao -> 255;
    # red/green (0 + 1 * 1 * (1 - as))/ao = 127/255 -> 127
    d = doc_of([{"op": "add_layer", "id": "L", "fill": "solid", "color": "#ffffff"},
                fill("#0000ff80")], w=1, h=1)
    assert tuple(layer(d)[0, 0]) == (127, 127, 255, 255), layer(d)[0, 0]
    # transparent layer, opaque colour, opacity 1: alpha byte == E
    d = doc_of([L, rect(0.5, 0, 1, 1, antialias=True), fill("#ff0000")], w=2, h=1)
    assert layer(d)[0, :, 3].tolist() == d.sel[0].tolist() == [128, 128]
    assert layer(d)[0, 0, :3].tolist() == [255, 0, 0]


def test_paint_over_lock():
    d = doc_of([L, rect(0, 0, 1, 1), fill("#ffffff80"), {"op": "deselect"},
                {"op": "lock_transparency", "layer": "L"}, fill("#000000")], w=2, h=1)
    # pixel 0: alpha 128 kept, colour (1*(1-1)) + (0*1) = 0 ; pixel 1: alpha 0 untouched
    assert layer(d)[0].tolist() == [[0, 0, 0, 128], [0, 0, 0, 0]]


def test_gradient_linear_hand():
    # 4x1, p0 (0,0) p1 (4,0) black -> white: t = 0.125, .375, .625, .875 -> 31.875, 95.625,
    # 159.375, 223.125 -> 32, 96, 159, 223
    d = doc_of([L, {"op": "gradient", "layer": "L", "p0": [0, 0], "p1": [4, 0],
                    "c0": "#000000", "c1": "#ffffff"}], w=4, h=1)
    assert layer(d)[0, :, 0].tolist() == [32, 96, 159, 223]
    assert layer(d)[0, :, 3].tolist() == [255] * 4
    d = doc_of([L, {"op": "gradient", "layer": "L", "p0": [0, 0], "p1": [4, 0], "reverse": True,
                    "c0": "#000000", "c1": "#ffffff"}], w=4, h=1)
    assert layer(d)[0, :, 0].tolist() == [223, 159, 96, 32]   # 1 - t: .875 .625 .375 .125
    d = doc_of([L, {"op": "gradient", "layer": "L", "p0": [1, 1], "p1": [1, 1],
                    "c0": "#000000", "c1": "#ffffff"}], w=4, h=1)
    assert not layer(d).any()                                  # zero length: no-op


def test_gradient_radial_hand():
    # radial p0 (0.5,0.5) p1 (2.5,0.5): radius 2; pixel x: t = x/2 -> 0, .5, 1, 1 (clamped)
    # red channel 0 -> 255: 0, 127.5 -> 128, 255, 255
    d = doc_of([L, {"op": "gradient", "layer": "L", "type": "radial", "p0": [0.5, 0.5],
                    "p1": [2.5, 0.5], "c0": "#000000", "c1": "#ff0000"}], w=4, h=1)
    assert layer(d)[0, :, 0].tolist() == [0, 128, 255, 255]


def test_bucket():
    # BK-04 shape: empty layer, one opaque rect; bucket at (0,0) tol 0 aa off fills the
    # transparent part only (canonical (0,0,0,0) seed); rect stays.
    d = doc_of([L, rect(1, 1, 2, 2), fill("#0000ff"), {"op": "deselect"},
                {"op": "bucket_fill", "layer": "L", "x": 0, "y": 0, "color": "#ff8000",
                 "tolerance": 0, "antialias": False}])
    a = layer(d)
    assert tuple(a[0, 0]) == (255, 128, 0, 255) and tuple(a[1, 1]) == (0, 0, 255, 255)
    # seed outside the canvas: no-op
    d = doc_of([L, {"op": "bucket_fill", "layer": "L", "x": -1, "y": 0, "color": "#ff8000"}])
    assert not layer(d).any()
    # product order ((ca*op)*r)*e with op 0.5 on a transparent layer: alpha q(0.5) = 128
    d = doc_of([L, {"op": "bucket_fill", "layer": "L", "x": 0, "y": 0, "color": "#ff8000",
                    "opacity": 0.5}], w=1, h=1)
    assert tuple(layer(d)[0, 0]) == (255, 128, 0, 128)


# ---------------------------------------------------------------------------------------------
# §11 kernel, §12 transform
# ---------------------------------------------------------------------------------------------

def test_kernel_values():
    assert G.K_scalar(0.0) == 1.0 and G.K_scalar(1.0) == 0.0 and G.K_scalar(-2.0) == 0.0
    assert G.K_scalar(0.5) == 0.5625 and G.K_scalar(-1.5) == -0.0625
    assert G.K_scalar(0.25) == 0.8671875 and G.K_scalar(0.75) == 0.2265625
    assert G.K_scalar(1.25) == -0.0703125
    xs = np.array([0.0, 0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 2.5, -0.5])
    assert G.K_array(xs).tolist() == [G.K_scalar(float(x)) for x in xs]


def test_matrices():
    assert G.invert([2.0, 0.0, 3.0, 0.0, 4.0, 5.0, 0.0, 0.0, 1.0]) == \
        [0.5, -0.0, -1.5, 0.0, 0.25, -1.25, 0.0, -0.0, 1.0]
    try:
        G.invert([1.0, 2.0, 0.0, 2.0, 4.0, 0.0, 0.0, 0.0, 1.0])
        raise AssertionError("singular accepted")
    except ScriptError:
        pass
    # quad = the rect's own corners -> identity (affine branch, power-of-two sizes: exact)
    M = G.quad_matrix([0.0, 0.0, 64.0, 32.0], [(0, 0), (64, 0), (64, 32), (0, 32)])
    assert M == [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0], M
    # projective: corners map onto the quad
    quad = [(16.0, 4.0), (48.0, 4.0), (62.0, 60.0), (2.0, 60.0)]
    M = G.quad_matrix([0.0, 0.0, 64.0, 64.0], quad)
    for (X, Y), (qx, qy) in zip([(0, 0), (64, 0), (64, 64), (0, 64)], quad):
        w = M[6] * X + M[7] * Y + M[8]
        assert abs((M[0] * X + M[1] * Y + M[2]) / w - qx) < 1e-9
        assert abs((M[3] * X + M[4] * Y + M[5]) / w - qy) < 1e-9
    # params: rotate 90 about (0,0): (1,0) -> (0,1) (clockwise on screen, y down)
    M = G.params_matrix(0.0, 0.0, 1.0, 1.0, 90.0, 0.0, 0.0, 0.0, 0.0)
    assert abs(M[0] * 1 + M[2]) < 1e-15 and abs(M[3] * 1 + M[5] - 1.0) < 1e-15


def _bw4():
    # 4x1 opaque: black, black, white, white
    return [L, fill("#000000"), rect(2, 0, 2, 1), fill("#ffffff"), {"op": "deselect"}]


def test_transform_half_pixel_hand():
    # translate [0.5, 0]: dest x -> sx = x; fx = x - 0.5; taps x-2..x+1 with weights
    # K(1.5), K(0.5), K(-0.5), K(-1.5) = -.0625, .5625, .5625, -.0625 ; rows: weight 1 on row 0.
    # x0: taps -2,-1,0,1  alpha .5625-.0625 = .5 -> 128 ; colour 0
    # x1: taps -1,0,1,2   alpha 1.0625 -> 1 ; R premult -.0625 -> clamp 0 -> 0
    # x2: taps 0,1,2,3    alpha 1 ; R .5625 - .0625 = .5 -> 127.5 -> 128
    # x3: taps 1,2,3,4    alpha 1.0625 -> 1 ; R 1.125 -> clamp to A' = 1 -> 255
    d = doc_of(_bw4() + [{"op": "transform", "layer": "L", "translate": [0.5, 0]}], w=4, h=1)
    assert layer(d)[0].tolist() == [[0, 0, 0, 128], [0, 0, 0, 255], [128, 128, 128, 255],
                                    [255, 255, 255, 255]], layer(d)[0].tolist()


def test_transform_exact_paths():
    d0 = doc_of(_bw4(), w=4, h=1)
    for form in ({"matrix": [1, 0, 0, 0, 1, 0, 0, 0, 1]}, {}, {"translate": [0, 0]}):
        d = doc_of(_bw4() + [dict({"op": "transform", "layer": "L"}, **form)], w=4, h=1)
        assert (layer(d) == layer(d0)).all(), form
    d = doc_of(_bw4() + [{"op": "transform", "layer": "L", "translate": [1, 0]}], w=4, h=1)
    assert layer(d)[0].tolist() == [[0, 0, 0, 0], [0, 0, 0, 255], [0, 0, 0, 255], [255, 255, 255, 255]]
    d = doc_of(_bw4() + [{"op": "transform", "layer": "L", "matrix": [-1, 0, 4, 0, 1, 0, 0, 0, 1]}],
               w=4, h=1)
    f = doc_of(_bw4() + [{"op": "flip", "axis": "h", "layer": "L"}], w=4, h=1)
    assert (layer(d) == layer(f)).all()
    # nearest, scale 2 about (0,0): dest x -> sx = (x + .5)/2 -> floor: 0,0,1,1
    d = doc_of(_bw4() + [{"op": "transform", "layer": "L", "scale": [2, 1], "pivot": [0, 0],
                          "interp": "nearest"}], w=4, h=1)
    assert layer(d)[0, :, 3].tolist() == [255] * 4 and layer(d)[0, :, 0].tolist() == [0] * 4
    # w <= 0 is transparent: matrix row 3 [0, 1, -0.5]: w = y + .5 - .5 = y -> row 0 has w = 0
    d = doc_of([L, fill("#ff0000"), {"op": "transform", "layer": "L",
                                    "matrix": [1, 0, 0, 0, 1, 0, 0, 1, -0.5]}], w=2, h=2)
    assert not layer(d)[0].any()


# ---------------------------------------------------------------------------------------------
# §13 - §16 Image menu
# ---------------------------------------------------------------------------------------------

def test_pillow_coeffs_hand():
    # 4 -> 2, o = 0: scale 2, fs 2, supp 4, center 1.0, taps 0..3 with
    # K(-.25), K(.25), K(.75), K(1.25) = .8671875, .8671875, .2265625, -.0703125 ; ww = 1.890625
    idx, wt = G.pillow_coeffs(4, 2)
    assert idx[0].tolist()[:4] == [0, 1, 2, 3]
    ks = [0.8671875, 0.8671875, 0.2265625, -0.0703125]
    assert wt[0].tolist()[:4] == [k / 1.890625 for k in ks]


def test_image_size_constant_and_nearest():
    d = doc_of([{"op": "add_layer", "id": "L", "fill": "solid", "color": "#336699"},
                {"op": "image_size", "w": 7, "h": 3}], w=4, h=4)
    a = layer(d)
    assert a.shape == (3, 7, 4) and (a == np.array([0x33, 0x66, 0x99, 255], np.uint8)).all()
    # nearest 4 -> 2: x = floor((x + .5) * 2) = 1, 3
    d = doc_of(_bw4() + [{"op": "image_size", "w": 2, "h": 1, "interp": "nearest"}], w=4, h=1)
    assert layer(d)[0, :, 0].tolist() == [0, 255]
    d = doc_of([rect(0, 0, 1, 1), {"op": "image_size", "w": 4, "h": 4}])
    assert not d.sel.any() and d.sel_saved is None


def test_canvas_size_crop_rotate_flip():
    # canvas_size 64x64 -> 81x51 anchor c: ox = 17 // 2 = 8, oy = -13 // 2 = -7 (floor)
    d = doc_of([L, rect(0, 0, 1, 1), fill("#ff0000"), {"op": "deselect"},
                {"op": "add_mask", "layer": "L"},
                {"op": "canvas_size", "w": 81, "h": 51}], w=64, h=64)
    a = layer(d)
    assert a.shape == (51, 81, 4)
    assert tuple(a[0, 8]) == (0, 0, 0, 0)          # old (0, 0) is at new (8, -7): cropped away
    d = doc_of([L, rect(0, 7, 1, 1), fill("#ff0000"), {"op": "deselect"},
                {"op": "canvas_size", "w": 81, "h": 51}], w=64, h=64)
    assert tuple(layer(d)[0, 8]) == (255, 0, 0, 255) and int((layer(d)[..., 3] > 0).sum()) == 1
    # crop past the canvas: transparent layers, mask 255 outside
    d = doc_of([{"op": "add_layer", "id": "L", "fill": "solid", "color": "#00ff00"},
                {"op": "add_mask", "layer": "L", "fill": "solid", "value": 0},
                {"op": "crop", "x": -1, "y": 0, "w": 3, "h": 1}], w=2, h=1)
    assert layer(d)[0, :, 3].tolist() == [0, 255, 255]
    assert d.find("L").mask.data[0].tolist() == [255, 0, 0]
    # rotate 90 on 3x2 (W=3,H=2): new size 2x3 ; old (0,0) -> new (H-1, 0) = (1, 0)
    d = doc_of([L, rect(0, 0, 1, 1), fill("#ff0000"), {"op": "deselect"},
                {"op": "rotate_canvas", "angle": 90}], w=3, h=2)
    assert (d.w, d.h) == (2, 3) and tuple(layer(d)[0, 1]) == (255, 0, 0, 255)
    d = doc_of([L, rect(0, 0, 1, 1), fill("#ff0000"), {"op": "deselect"},
                {"op": "rotate_canvas", "angle": -90}], w=3, h=2)      # 270: old(0,0) -> new(0, W-1)
    assert (d.w, d.h) == (2, 3) and tuple(layer(d)[2, 0]) == (255, 0, 0, 255)
    d = doc_of([L, rect(0, 0, 1, 1), fill("#ff0000"), {"op": "deselect"},
                {"op": "rotate_canvas", "angle": 540}], w=3, h=2)      # 180
    assert tuple(layer(d)[1, 2]) == (255, 0, 0, 255)
    # arbitrary: 30 deg on 64x64 -> 88x88 (doc RA-07)
    d = doc_of([{"op": "rotate_canvas", "angle": 30}], w=64, h=64)
    assert (d.w, d.h) == (88, 88)
    d = doc_of([{"op": "rotate_canvas", "angle": 360}, rect(0, 0, 1, 1)], w=3, h=2)
    assert (d.w, d.h) == (3, 2)
    # flip canvas clears the selection; flip layer keeps it
    d = doc_of([L, rect(0, 0, 1, 1), {"op": "flip", "axis": "v"}])
    assert not d.sel.any()
    d = doc_of([L, rect(0, 0, 1, 1), {"op": "flip", "axis": "v", "layer": "L"}])
    assert d.sel[0, 0] == 255


# ---------------------------------------------------------------------------------------------
# C9 errors
# ---------------------------------------------------------------------------------------------

def test_errors():
    expect_error([rect(0, 0, 0, 1)])
    expect_error([rect(0, 0, 1, 1, mode="xor")])
    expect_error([rect(0, 0, 1, 1, antialias=1)])
    expect_error([{"op": "select_polygon", "points": [[0, 0], [1, 1]]}])
    expect_error([{"op": "select_polygon", "points": [[0, 0], [1, 1], [1, "a"]]}])
    expect_error([L, {"op": "select_wand", "layer": "L", "x": 0.0, "y": 0}])
    expect_error([L, {"op": "select_wand", "layer": "L", "x": 0, "y": 0, "tolerance": 256}])
    expect_error([{"op": "add_group", "id": "g"}, {"op": "select_wand", "layer": "g", "x": 0, "y": 0}])
    expect_error([{"op": "feather", "radius": -1}])
    expect_error([{"op": "feather", "radius": 250.5}])
    expect_error([{"op": "expand", "by": 0}])
    expect_error([{"op": "contract", "by": 101}])
    expect_error([L, fill("#12345")])
    expect_error([L, fill("#123456", opacity=1.5)])
    expect_error([L, {"op": "transform", "layer": "L", "matrix": [1, 0, 0, 0, 1, 0, 0, 0]}])
    expect_error([L, {"op": "transform", "layer": "L", "matrix": [1, 2, 0, 2, 4, 0, 0, 0, 1]}])
    expect_error([L, {"op": "transform", "layer": "L", "scale": [0, 1]}])
    expect_error([L, {"op": "transform", "layer": "L", "scale": [1001, 1]}])
    expect_error([L, {"op": "transform", "layer": "L", "skew": [89.5, 0]}])
    expect_error([L, {"op": "transform", "layer": "L", "rotate": 3601}])
    expect_error([L, {"op": "transform", "layer": "L", "rect": [0, 0, 4, 4]}])       # quad missing
    expect_error([L, {"op": "transform", "layer": "L", "quad": [[0, 0], [1, 0], [1, 1], [0, 1]],
                      "rect": [0, 0, 0, 4]}])
    expect_error([L, {"op": "transform", "layer": "L", "quad": [[0, 0], [1, 0], [1, 1], [0, 1]],
                      "pivot": [0, 0]}])
    expect_error([L, {"op": "transform", "layer": "L", "interp": "bilinear"}])
    expect_error([{"op": "image_size", "w": 0, "h": 4}])
    expect_error([{"op": "image_size", "w": 16385, "h": 4}])
    expect_error([{"op": "canvas_size", "w": 4, "h": 4, "anchor": "center"}])
    expect_error([{"op": "crop", "x": 0.5, "y": 0, "w": 1, "h": 1}])
    expect_error([{"op": "rotate_canvas", "angle": 3600.5}])
    expect_error([{"op": "rotate_canvas", "angle": 45}], w=16384, h=16384)            # > 16384
    expect_error([{"op": "flip", "axis": "x"}])
    expect_error([L, {"op": "gradient", "layer": "L", "type": "angle", "p0": [0, 0], "p1": [1, 0],
                      "c0": "#000000", "c1": "#ffffff"}])
    expect_error([L, {"op": "gradient", "layer": "L", "p0": [0, 0], "c0": "#000000", "c1": "#ffffff"}])
    expect_error([L, {"op": "bucket_fill", "layer": "L", "x": 0, "y": 0}])             # colour required
    expect_error([{"op": "select_all", "x": 1}])                                       # unknown field


# ---------------------------------------------------------------------------------------------
# golden scripts
# ---------------------------------------------------------------------------------------------

def _load(path):
    with open(path) as fh:
        d = json.load(fh)
    expect = d.pop("expect", None)
    d.pop("equal_to", None)
    return d, expect


def _render(d):
    return render_script_text(json.dumps(d))


def render_goldens(outdir=None):
    """Render every geometry golden. Returns (results dict stem -> array, timings, failures)."""
    paths = sorted(glob.glob(os.path.join(GOLDEN_DIR, "*.json")))
    results, timings, failures = {}, {}, []
    for p in paths:
        stem = os.path.splitext(os.path.basename(p))[0]
        d, expect = _load(p)
        t0 = time.perf_counter()
        try:
            img = _render(d)
            err = None
        except ScriptError as e:
            img, err = None, e
        timings[stem] = time.perf_counter() - t0
        if expect == "error":
            if err is None:
                failures.append("%s: expected a script error, rendered" % stem)
            else:                    # non-vacuity: the error must come from the last op
                d2 = dict(d, ops=d["ops"][:-1])
                try:
                    _render(d2)
                except ScriptError as e2:
                    failures.append("%s: fails before its last op: %s" % (stem, e2))
            results[stem] = err
        else:
            if err is not None:
                failures.append("%s: script error %s" % (stem, err))
            else:
                results[stem] = img
                if outdir:
                    from PIL import Image
                    os.makedirs(outdir, exist_ok=True)
                    Image.fromarray(img, "RGBA").save(os.path.join(outdir, stem + ".png"))
    return results, timings, failures


def golden_identities(results):
    """Cross-script identities the doc implies (each returns a failure string or None)."""
    fails = []

    def prefix(stem, n_drop, extra=()):
        d, _ = _load(os.path.join(GOLDEN_DIR, stem + ".json"))
        d["ops"] = d["ops"][:len(d["ops"]) - n_drop] + list(extra)
        return _render(d)

    def same(a, b, what):
        if a.shape != b.shape or not (a == b).all():
            fails.append(what)

    if "TR-01" in results:           # identity transform reproduces the input bytes
        same(results["TR-01"], prefix("TR-01", 1), "TR-01 != input")
    if "TR-14" in results:           # mirror matrix == flip h of the layer
        same(results["TR-14"], prefix("TR-14", 1, [{"op": "flip", "axis": "h", "layer": "L"}]),
             "TR-14 != flip h")
    if "RA-08" in results:           # 90 + 180 + 270 = 180 rotation, then flip v == flip h
        same(results["RA-08"], prefix("RA-08", 4, [{"op": "flip", "axis": "h"}]), "RA-08 != flip h")
    if "SEL-13" in results:          # subtract(A, A) = empty -> full fill
        if not (results["SEL-13"][..., 3] == 255).all():
            fails.append("SEL-13 not a full fill")
    if "TR-02" in results:           # integer translate [5, -3] is an exact shift
        base = prefix("TR-02", 1)
        exp = np.zeros_like(base)
        exp[...] = base[0, 0]        # bg-only pixel (white)
        exp[0:61, 5:64] = base[3:64, 0:59]
        same(results["TR-02"], exp, "TR-02 != exact shift")
    return fails


def test_goldens():
    if not os.path.isdir(GOLDEN_DIR):
        return
    results, _, failures = render_goldens()
    failures += golden_identities(results)
    assert not failures, failures


# ---------------------------------------------------------------------------------------------

def main(argv):
    outdir = None
    if len(argv) >= 3 and argv[1] == "--goldens":
        outdir = argv[2]
    tests = [(k, v) for k, v in sorted(globals().items()) if k.startswith("test_") and k != "test_goldens"]
    t0 = time.perf_counter()
    bad = 0
    for name, fn in tests:
        try:
            fn()
        except Exception as e:           # report every failure, keep going
            bad += 1
            print("FAIL %s: %r" % (name, e))
    print("unit self-checks: %d/%d passed in %.2fs" % (len(tests) - bad, len(tests),
                                                       time.perf_counter() - t0))
    if os.path.isdir(GOLDEN_DIR):
        t1 = time.perf_counter()
        results, timings, failures = render_goldens(outdir)
        failures += golden_identities(results)
        n_err = sum(1 for v in results.values() if isinstance(v, ScriptError))
        print("goldens: %d scripts (%d error scripts) in %.2fs; slowest: %s" % (
            len(timings), n_err, time.perf_counter() - t1,
            ", ".join("%s %.2fs" % kv for kv in sorted(timings.items(), key=lambda kv: -kv[1])[:5])))
        for f in failures:
            print("FAIL golden " + f)
        bad += len(failures)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
