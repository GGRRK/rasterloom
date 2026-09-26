#!/usr/bin/env python3
"""Self-checks for the reference renderer, with hand-computed expected bytes.

Run:  python3 tests/reference/test_refcomp.py      (plain asserts; prints a summary)
  or: python3 -m pytest tests/reference/test_refcomp.py

Expected values below were worked out by hand from docs/math/10-compositing.md (the arithmetic
is shown in the comments); none of them were produced by running the reference.
"""

import json
import os
import subprocess
import sys
import tempfile
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from rc.composite import ADJUST, COMPOSITE, pass_lerp  # noqa: E402
from rc.core import (ScriptError, dec, pixel_hash, pixel_hash_grid, q, splitmix64,  # noqa: E402
                     unit, unit_grid)
from rc.model import AdjustmentLayer  # noqa: E402
from rc.registry import load_all  # noqa: E402
from rc.runner import render_script_text, run_script  # noqa: E402

load_all()


# ---------------------------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------------------------

def px(mode, bd, src, c=1.0, seed=0):
    """COMPOSITE one pixel: backdrop RGBA bytes, source RGB bytes, coverage -> RGBA tuple."""
    D = np.array([[bd]], dtype=np.uint8)
    S = np.array([[src]], dtype=np.uint8)
    out = COMPOSITE(D, S, np.array([[c]]), mode, seed)
    return tuple(int(v) for v in out[0, 0])


def script(ops, w=4, h=4, bg="#00000000"):
    return {"canvas": {"w": w, "h": h, "bg": bg}, "ops": ops, "out": "png8"}


def render(ops, **kw):
    return render_script_text(json.dumps(script(ops, **kw)))


def solid(i, color, **extra):
    d = {"op": "add_layer", "id": i, "fill": "solid", "color": color}
    d.update(extra)
    return d


def expect_error(ops, **kw):
    try:
        render(ops, **kw)
    except ScriptError:
        return
    raise AssertionError("expected a script error for %r" % (ops,))


# ---------------------------------------------------------------------------------------------
# C2 q(), C3 canonical pixel, C6 randomness
# ---------------------------------------------------------------------------------------------

def test_q_basics():
    assert q(0.0) == 0 and q(1.0) == 255
    assert q(-0.3) == 0 and q(1.7) == 255            # clamp first
    assert q(0.5) == 128                             # 127.5 exactly -> away from zero
    assert q(0.25) == 64                             # 63.75
    assert q(0.2) == 51                              # 51.0 (0.2*255 = 51.00000000000001)
    arr = q(np.array([0.0, 0.5, 0.25, 1.0, -1.0, 2.0]))
    assert arr.dtype == np.uint8 and list(arr) == [0, 128, 64, 255, 0, 255]


def test_q_exact_half_ties():
    # every k + 0.5 that is reached exactly must round UP (half away from zero), unlike
    # banker's rounding (np.round) which sends even k + 0.5 down.
    ties = 0
    for k in range(255):
        x = (k + 0.5) / 255.0
        if x * 255.0 == k + 0.5:
            ties += 1
            assert q(x) == k + 1, (k, q(x))
            assert int(q(np.array([x]))[0]) == k + 1
    assert ties > 100, ties
    # q(0.3): 0.3*255 rounds (in binary64) to exactly 76.5 -> 77, truncation would give 76
    assert 0.3 * 255.0 == 76.5 and q(0.3) == 77


def test_splitmix64_vectors():
    # SplitMix64 reference outputs for state 0 (Vigna): first two outputs
    assert splitmix64(0) == 0xE220A8397B1DCDAF
    assert splitmix64(0x9E3779B97F4A7C15) == 0x6E789E6AA1B965F4
    # the vectorised grid agrees with the scalar definition
    g = pixel_hash_grid(1234567, 7, 5, 5)
    for (x, y) in [(0, 0), (6, 4), (3, 2), (1, 4)]:
        assert int(g[y, x]) == pixel_hash(1234567, x, y, 5)
    u = unit_grid(g)
    assert u.min() >= 0.0 and u.max() < 1.0
    assert unit(0xFFFFFFFFFFFFFFFF) == 1.0 - 2.0 ** -53


def test_canonical_transparent():
    # near-zero coverage: ao = 0.001 -> q(ao) = q(0.255) = 0 -> whole pixel (0,0,0,0)
    assert px("norm", (0, 0, 0, 0), (255, 128, 0), 0.001) == (0, 0, 0, 0)
    # ao == 0 (nothing anywhere)
    assert px("mul", (0, 0, 0, 0), (255, 128, 0), 0.0) == (0, 0, 0, 0)
    # a script: bg alpha 0 with a colour is canonicalised
    out = render([], bg="#FF804000")
    assert (out == 0).all()
    # fo_near_zero shape: opacity 0.001 layer over transparent bg -> all (0,0,0,0)
    out = render([solid("u", "#FF8000FF"), {"op": "set_opacity", "layer": "u", "value": 0.001}])
    assert (out == 0).all()


# ---------------------------------------------------------------------------------------------
# §3 blend functions, opaque backdrop + full coverage: output = q(B)
# ---------------------------------------------------------------------------------------------

OPQ = 255


def b1(mode, cb, cs):
    """Single-channel opaque blend (R channel), G and B zeroed."""
    return px(mode, (cb, 0, 0, OPQ), (cs, 0, 0))[0]


def test_normal_half_tie():
    # red backdrop, blue source, c = 0.5: ao = 1, co_R = 0.5*0 + 1*1*0.5 = 0.5 -> 127.5 -> 128
    assert px("norm", (255, 0, 0, 255), (0, 0, 255), 0.5) == (128, 0, 128, 255)


def test_separable_values():
    assert b1("mul", 200, 100) == 78       # 200*100/255 = 78.43
    assert b1("scrn", 200, 100) == 222     # 255 - 55*155/255 = 221.57
    assert b1("dark", 200, 100) == 100 and b1("lite", 200, 100) == 200
    assert b1("diff", 100, 200) == 100 and b1("diff", 200, 100) == 100
    assert b1("smud", 255, 255) == 0       # (1+1) - 2*1*1
    assert b1("fsub", 100, 200) == 0 and b1("fsub", 200, 100) == 100   # backdrop minus source
    assert b1("lbrn", 100, 100) == 0 and b1("lbrn", 200, 100) == 45    # 300-255
    assert b1("lddg", 200, 100) == 255
    assert b1("lLit", 100, 100) == 45      # 100 + 200 - 255
    assert b1("pLit", 250, 100) == 200     # cs <= .5: min(cb, 2cs)
    assert b1("pLit", 10, 200) == 145      # cs > .5: max(cb, 2cs-1) = 400-255
    assert b1("over", 64, 200) == 100      # cb <= .5: cs*(2cb) = 200*128/255 = 100.39
    assert b1("hLit", 200, 64) == 100      # cs <= .5: cb*(2cs) = 200*128/255
    assert b1("sLit", 64, 255) == 128      # D = sqrt(64/255) = 0.50098 -> 127.75
    assert b1("sLit", 63, 255) == 127      # poly D: 0.497059 -> 126.75
    assert b1("sLit", 128, 0) == 64        # cs <= .5: cb - cb*(1-cb) = cb^2 = 0.25197 -> 64.25


def test_guards():
    # Color Dodge: cb == 0 checked first -> B(0, 1) = 0 ; B(>0, 1) = 1
    assert b1("div", 0, 255) == 0 and b1("div", 1, 255) == 255 and b1("div", 0, 0) == 0
    # Color Burn: cb == 1 first -> B(1, 0) = 1 ; B(<1, 0) = 0
    assert b1("idiv", 255, 0) == 255 and b1("idiv", 254, 0) == 0
    # Divide: B(0,0) = 0, B(x>0, 0) = 1 ; 50/200 = 0.25 -> 63.75 -> 64
    assert b1("fdiv", 0, 0) == 0 and b1("fdiv", 10, 0) == 255 and b1("fdiv", 50, 200) == 64
    # Vivid Light inherits both: cs=0 -> 1 iff cb == 1 ; cs=255 -> 0 iff cb == 0
    assert b1("vLit", 255, 0) == 255 and b1("vLit", 254, 0) == 0
    assert b1("vLit", 0, 255) == 0 and b1("vLit", 1, 255) == 255


def test_integer_modes():
    assert b1("hMix", 100, 155) == 255     # 100 + 155 = 255 >= 255
    assert b1("hMix", 100, 154) == 0
    # Darker/Lighter Color ties keep the backdrop: #3B0000 lum 30*59 = 1770, #001E00 59*30 = 1770
    assert px("dkCl", (59, 0, 0, 255), (0, 30, 0)) == (59, 0, 0, 255)
    assert px("lgCl", (59, 0, 0, 255), (0, 30, 0)) == (59, 0, 0, 255)
    # whole-vector selection
    assert px("dkCl", (255, 255, 255, 255), (10, 20, 30)) == (10, 20, 30, 255)
    assert px("lgCl", (255, 255, 255, 255), (10, 20, 30)) == (255, 255, 255, 255)
    assert px("lgCl", (10, 20, 30, 255), (0, 0, 200)) == (0, 0, 200, 255)   # 2200 > 1790


def test_nonseparable():
    # lum: SetLum((1,0,0), 128/255): d = .201961, C = (1.201961, .201961, .201961);
    # ClipColor x > 1: L = .501961, d2 = .7 -> r = 1.0, g = b = .501961 - .3*.498039/.7 = .288516 -> 73.57
    assert px("lum", (255, 0, 0, 255), (128, 128, 128)) == (255, 74, 74, 255)
    # colr: SetLum(Cs = (1,0,0), Lum(grey 128)) is the same computation
    assert px("colr", (128, 128, 128, 255), (255, 0, 0)) == (255, 74, 74, 255)
    # hue over a grey backdrop: SetSat(Cs, 0) = 0, SetLum(0, Lum(Cb)) = the grey
    assert px("hue", (100, 100, 100, 255), (255, 0, 0)) == (100, 100, 100, 255)
    # sat with a grey source: SetSat(Cb, 0) = 0 -> SetLum(0, 0.3) = (0.3,)*3 -> 76.5 -> 77
    assert px("sat", (255, 0, 0, 255), (60, 60, 60)) == (77, 77, 77, 255)


def test_partial_alpha():
    # multiply onto a transparent backdrop acts like Normal: mixed = (1-0)*cs + 0*B
    assert px("mul", (0, 0, 0, 0), (10, 20, 30)) == (10, 20, 30, 255)
    # white 128-alpha backdrop, black source c = .25:
    # ab = .501961, ao = .25 + .501961*.75 = .626471 -> 159.75 -> 160
    # co = (0 + (.501961*1)*.75) / .626471 = .600939 -> 153.24 -> 153
    assert px("norm", (255, 255, 255, 128), (0, 0, 0), 0.25) == (153, 153, 153, 160)
    # screen with ab = 0.5ish: mixed = (1-ab)*cs + ab*B ; bd (0,0,0,128) src (255,255,255) c=1
    # B = 1 so mixed = 1, co = 1*1 / 1 = 1
    assert px("scrn", (0, 0, 0, 128), (255, 255, 255)) == (255, 255, 255, 255)


def test_dissolve():
    seed = 7
    D = np.zeros((8, 8, 4), dtype=np.uint8)
    S = np.full((8, 8, 3), 200, dtype=np.uint8)
    out = COMPOSITE(D, S, np.full((8, 8), 0.5), "diss", seed)
    for y in range(8):
        for x in range(8):
            keep = unit(pixel_hash(seed, x, y, 5)) < 0.5      # scalar C6 path
            assert tuple(out[y, x]) == ((200, 200, 200, 255) if keep else (0, 0, 0, 0))
    assert 10 < int((out[..., 3] > 0).sum()) < 54
    # c = 1 always kept
    out = COMPOSITE(D, S, np.ones((8, 8)), "diss", seed)
    assert (out[..., 3] == 255).all()


def test_adjust_invert():
    node = AdjustmentLayer("a", "invert", None)
    D = np.array([[[10, 20, 30, 255], [10, 20, 30, 0], [200, 100, 0, 77]]], dtype=np.uint8)
    out = ADJUST(D, node, np.ones((1, 3)), "norm", 0)
    assert tuple(out[0, 0]) == (245, 235, 225, 255)
    assert tuple(out[0, 1]) == (0, 0, 0, 0)                  # alpha 0 stays canonical
    assert tuple(out[0, 2]) == (55, 155, 255, 77)            # alpha byte unchanged
    # fill 0.5: co = .5*cb + .5*(1-cb) = .5 for every channel -> 128
    out = ADJUST(D, node, np.full((1, 3), 0.5), "norm", 0)
    assert tuple(out[0, 0]) == (128, 128, 128, 255)


# ---------------------------------------------------------------------------------------------
# exhaustive checks of the doc's "skipping is exact" claims (§4.1, §7.1, §9.3)
# ---------------------------------------------------------------------------------------------

def test_stability_claims():
    a = np.arange(256, dtype=np.uint8)
    A, C = np.meshgrid(a, a, indexing="ij")          # A = alpha byte, C = colour byte
    buf = np.stack([C, C, C, A], axis=-1).astype(np.uint8)
    buf[A == 0] = 0
    # §4.1: c == 0 returns the backdrop bytes
    out = COMPOSITE(buf, np.full(buf.shape[:2] + (3,), 99, np.uint8), np.zeros(A.shape), "mul", 0)
    assert (out == buf).all()
    # §9.3: P composited in Normal with c = n(A) onto transparent returns P
    out = COMPOSITE(np.zeros_like(buf), buf[..., :3], dec(buf[..., 3]), "norm", 0)
    assert (out == buf).all()
    # §7.1: w == 1 returns R's bytes; R == B0 (no visible children) returns B0 for any w
    other = np.roll(buf, 77, axis=0)
    assert (pass_lerp(other, buf, np.ones(A.shape)) == buf).all()
    for wv in (0.0, 0.1, 0.3, 0.5, 0.7, 0.999):
        assert (pass_lerp(buf, buf, np.full(A.shape, wv)) == buf).all(), wv


# ---------------------------------------------------------------------------------------------
# content fills
# ---------------------------------------------------------------------------------------------

def test_fills():
    out = render([{"op": "add_layer", "id": "g", "fill": "gradient"}], w=256, h=2)
    assert (out[0, :, 0] == np.arange(256)).all() and (out[..., 3] == 255).all()
    out = render([{"op": "add_layer", "id": "g", "fill": "gradient", "from": "#FF0000",
                   "to": "#0000FF", "rect": [1, 0, 1, 4]}])                    # rw == 1: t = 0
    assert tuple(out[0, 1]) == (255, 0, 0, 255) and tuple(out[0, 0]) == (0, 0, 0, 0)
    out = render([{"op": "add_layer", "id": "c", "fill": "checker", "cell": 2}])
    assert tuple(out[0, 0]) == (255, 255, 255, 255) and tuple(out[0, 2]) == (204, 204, 204, 255)
    assert tuple(out[2, 2]) == (255, 255, 255, 255)
    out = render([{"op": "add_layer", "id": "n", "fill": "noise", "seed": 3, "alpha": 9}])
    assert tuple(out[1, 2][:3]) == tuple(pixel_hash(3, 2, 1, k) >> 56 for k in range(3))
    assert out[1, 2, 3] == 9
    # a gradient mask 0 -> 255 over 256 px equals the column index; outside value elsewhere
    doc = run_script(script([solid("l", "#FFFFFF"),
                             {"op": "add_mask", "layer": "l", "fill": "gradient",
                              "rect": [0, 0, 256, 1], "outside": 7}], w=256, h=2))
    m = doc.find("l").mask.data
    assert (m[0] == np.arange(256)).all() and (m[1] == 7).all()


# ---------------------------------------------------------------------------------------------
# groups (§7)
# ---------------------------------------------------------------------------------------------

def test_groups():
    back = solid("k", "#C86432")                     # (200, 100, 50)
    # pass-through with a mul child == the mul layer without a group
    a = render([back, {"op": "add_group", "id": "g"},
                solid("m", "#808080", parent="g"), {"op": "set_blend", "layer": "m", "mode": "mul"}])
    assert tuple(a[0, 0]) == (100, 50, 25, 255)     # 200*128/255 = 100.4, 50.2, 25.1
    # isolated: the child multiplies onto transparency (= Normal), then the group is Normal
    b = render([back, {"op": "add_group", "id": "g", "mode": "isolated"},
                solid("m", "#808080", parent="g"), {"op": "set_blend", "layer": "m", "mode": "mul"}])
    assert tuple(b[0, 0]) == (128, 128, 128, 255)
    # pass-through opacity .5 with two overlapping opaque children: ONE lerp against the
    # pre-group backdrop -> (0, .5, .5) -> (0,128,128); per-child opacity would give (64,128,64)
    c = render([solid("k", "#0000FF"), {"op": "add_group", "id": "g"},
                solid("r", "#FF0000", parent="g"), solid("gr", "#00FF00", parent="g"),
                {"op": "set_opacity", "layer": "g", "value": 0.5}])
    assert tuple(c[0, 0]) == (0, 128, 128, 255)
    # adjustment inside a pass-through group reaches the layer below the group
    d = render([back, {"op": "add_group", "id": "g"},
                {"op": "add_adjustment", "id": "i", "parent": "g", "type": "invert"}])
    assert tuple(d[0, 0]) == (55, 155, 205, 255)
    # ... but inside an isolated group it only sees the (transparent) group content
    e = render([back, {"op": "add_group", "id": "g", "mode": "isolated"},
                {"op": "add_adjustment", "id": "i", "parent": "g", "type": "invert"}])
    assert tuple(e[0, 0]) == (200, 100, 50, 255)
    # hidden/empty groups change nothing
    f = render([back, {"op": "add_group", "id": "e1"}, {"op": "add_group", "id": "e2", "mode": "scrn"},
                {"op": "add_group", "id": "h"}, solid("x", "#FFFFFF", parent="h"),
                {"op": "set_visible", "layer": "h", "value": False}])
    assert (f == render([back])).all()


# ---------------------------------------------------------------------------------------------
# clip groups (§6)
# ---------------------------------------------------------------------------------------------

def test_clip_fill_vs_opacity():
    ops = [solid("k", "#0000FF"), solid("base", "#FF0000", rect=[0, 0, 2, 4]),
           solid("c", "#00FF00"), {"op": "set_clip", "layer": "c", "value": True}]
    # fill 0 base: clipped layer shows inside the shape, not outside, base colour invisible
    out = render(ops + [{"op": "set_fill", "layer": "base", "value": 0.0}])
    assert tuple(out[0, 0]) == (0, 255, 0, 255) and tuple(out[0, 3]) == (0, 0, 255, 255)
    # opacity 0 base: whole clip group hidden
    out = render(ops + [{"op": "set_opacity", "layer": "base", "value": 0.0}])
    assert tuple(out[0, 0]) == (0, 0, 255, 255)
    # fill .4, clipped opacity .25: G seed (255,0,0,q(.4)=102);
    # G = COMPOSITE: ao = .25 + .4*.75 = .55 -> 140 ; R = .3/.55 = .5454 -> 139 ; G = .25/.55 -> 116
    # onto blue with cg = 140/255: R = 140*139/65025*255 = 76.3 ; G = 140*116/65025*255 = 63.7 ; B = 115
    out = render(ops + [{"op": "set_fill", "layer": "base", "value": 0.4},
                        {"op": "set_opacity", "layer": "c", "value": 0.25}])
    assert tuple(out[0, 0]) == (76, 64, 115, 255), tuple(out[0, 0])


def test_clbl():
    ops = [solid("k", "#C86432"), solid("base", "#FF0000", rect=[0, 0, 2, 4]),
           solid("c", "#808080"), {"op": "set_clip", "layer": "c", "value": True},
           {"op": "set_blend", "layer": "c", "mode": "mul"},
           {"op": "set_fill", "layer": "base", "value": 0.0}]
    # clbl true: mul onto the (transparent, fill-0) interior = the grey itself
    out = render(ops)
    assert tuple(out[0, 0]) == (128, 128, 128, 255) and tuple(out[0, 3]) == (200, 100, 50, 255)
    # clbl false: mul acts on the backdrop inside the shape: (100, 50, 25)
    out = render(ops + [{"op": "set_clbl", "layer": "base", "value": False}])
    assert tuple(out[0, 0]) == (100, 50, 25, 255) and tuple(out[0, 3]) == (200, 100, 50, 255)


def test_clip_edge_cases():
    back = solid("k", "#0000FF")
    # clipped layer at index 0 -> rendered unclipped
    out = render([solid("c", "#00FF00"), {"op": "set_clip", "layer": "c", "value": True}])
    assert tuple(out[0, 3]) == (0, 255, 0, 255)
    # clipped layer above a group -> unclipped
    out = render([back, {"op": "add_group", "id": "g"}, solid("c", "#00FF00", rect=[3, 0, 1, 4]),
                  {"op": "set_clip", "layer": "c", "value": True}])
    assert tuple(out[0, 3]) == (0, 255, 0, 255)
    # hidden base hides its clipped layers
    out = render([back, solid("b", "#FF0000", rect=[0, 0, 2, 4]), solid("c", "#00FF00"),
                  {"op": "set_clip", "layer": "c", "value": True},
                  {"op": "set_visible", "layer": "b", "value": False}])
    assert (out == render([back])).all()
    # base mask is part of the shape: masked-out half shows the backdrop
    out = render([back, solid("b", "#FF0000"), solid("c", "#00FF00"),
                  {"op": "set_clip", "layer": "c", "value": True},
                  {"op": "add_mask", "layer": "b", "rect": [0, 0, 2, 4]}])
    assert tuple(out[0, 0]) == (0, 255, 0, 255) and tuple(out[0, 3]) == (0, 0, 255, 255)


# ---------------------------------------------------------------------------------------------
# masks, merges, flatten, undo
# ---------------------------------------------------------------------------------------------

def test_masks_and_merges():
    # apply_mask: A' = q(n(255) * n(128)) = 128
    doc = run_script(script([solid("l", "#FF0000"), {"op": "add_mask", "layer": "l", "value": 128},
                             {"op": "set_mask_enabled", "layer": "l", "value": False},
                             {"op": "apply_mask", "layer": "l"}]))
    L = doc.find("l")
    assert L.mask is None and tuple(L.rgba[0, 0]) == (255, 0, 0, 128)
    # merge_down plain: grey 100 + black at opacity .5 -> 50 ; L keeps opacity, fill reset
    doc = run_script(script([solid("l", "#646464"), {"op": "set_fill", "layer": "l", "value": 0.2},
                             solid("u", "#000000"), {"op": "set_opacity", "layer": "u", "value": 0.5},
                             {"op": "set_opacity", "layer": "l", "value": 0.3},
                             {"op": "merge_down", "layer": "u"}]))
    L = doc.find("l")
    # L' alpha = q(1*1*.2) = 51 ; black over it at .5: ao = .5 + .2*.5 = .6 -> 153 ;
    # co = (0 + (.2*100/255)*.5) / .6 = .065359 -> 16.67 -> 17
    assert tuple(L.rgba[0, 0]) == (17, 17, 17, 153), tuple(L.rgba[0, 0])
    assert L.fill == 1.0 and L.opacity == 0.3 and doc.find("u") is None
    # merge_visible: hidden bottom layer stays at index 0, merged layer at index 1 (k = 1)
    doc = run_script(script([solid("h", "#00FF00"), {"op": "set_visible", "layer": "h", "value": False},
                             solid("a", "#FF0000"), {"op": "merge_visible"}], bg="#FFFFFFFF"))
    assert [n.id for n in doc.root.children] == ["h", "merged"] and doc.bg == (255, 255, 255, 255)
    # flatten bakes a partial bg once and resets it
    doc = run_script(script([{"op": "flatten"}], bg="#FF000080"))
    assert doc.bg == (0, 0, 0, 0) and tuple(doc.find("flattened").rgba[0, 0]) == (255, 0, 0, 128)
    out = render([{"op": "flatten"}], bg="#FF000080")
    assert tuple(out[0, 0]) == (255, 0, 0, 128)


def test_undo():
    out = render([solid("a", "#FF0000"), solid("b", "#00FF00"), {"op": "undo"}])
    assert tuple(out[0, 0]) == (255, 0, 0, 255)
    out = render([solid("a", "#FF0000"), {"op": "set_opacity", "layer": "a", "value": 0.5},
                  {"op": "undo", "steps": 2}])
    assert (out == 0).all()
    # ids freed by undo can be reused
    render([solid("a", "#FF0000"), {"op": "undo"}, solid("a", "#00FF00")])
    expect_error([solid("a", "#FF0000"), {"op": "undo", "steps": 2}])
    expect_error([{"op": "undo"}])


# ---------------------------------------------------------------------------------------------
# script errors (C9)
# ---------------------------------------------------------------------------------------------

def test_script_errors():
    L = solid("a", "#FF0000")
    expect_error([{"op": "nope"}])
    expect_error([dict(L, colour="#FFFFFF")])                                     # unknown field
    expect_error([{"op": "add_layer", "id": "a", "fill": "solid", "from": "#000000"}])   # wrong-fill field
    expect_error([L, {"op": "set_blend", "layer": "a", "mode": "mul "}])            # trailing space
    expect_error([L, {"op": "set_blend", "layer": "a", "mode": "pass"}])            # pass on a layer
    expect_error([L, {"op": "set_blend", "layer": "a", "mode": "isolated"}])
    expect_error([{"op": "add_group", "id": "g"}, {"op": "set_fill", "layer": "g", "value": 0.5}])
    expect_error([L, {"op": "set_opacity", "layer": "a", "value": 1.5}])            # never clamped
    expect_error([L, {"op": "set_opacity", "layer": "a", "value": "0.5"}])
    expect_error([{"op": "add_layer", "id": "a", "fill": "checker", "cell": 2.0}])  # int must be int
    expect_error([L, {"op": "set_opacity", "layer": "zz", "value": 0.5}])           # unknown id
    expect_error([L, L])                                                            # duplicate id
    expect_error([{"op": "add_layer", "id": "root"}])
    expect_error([L, {"op": "set_opacity", "layer": "root", "value": 0.5}])
    expect_error([L, {"op": "add_layer", "id": "b", "parent": "a"}])                # parent not a group
    expect_error([L, {"op": "apply_mask", "layer": "a"}])                          # no mask
    expect_error([L, {"op": "merge_down", "layer": "a"}])                          # at index 0
    expect_error([{"op": "add_group", "id": "g"}, {"op": "add_group", "id": "h", "parent": "g"},
                  {"op": "move_layer", "id": "g", "parent": "h"}])                 # into descendant
    expect_error([L, {"op": "move_layer", "id": "a", "index": 2}])
    expect_error([{"op": "add_adjustment", "id": "i", "type": "invert", "params": {"x": 1}}])
    expect_error([{"op": "add_adjustment", "id": "i", "type": "sepia"}])
    expect_error([L, {"op": "set_blend", "layer": "a", "mode": "norm", "seed": -1}])
    expect_error([], w=16385)
    for bad in ['{"canvas":{"w":4,"h":4},"ops":[]}',                      # missing out
                '{"canvas":{"w":4,"h":4},"ops":[],"out":"png16"}',
                '{"canvas":{"w":4,"h":4,"w":5},"ops":[],"out":"png8"}',   # duplicate key
                '{"canvas":{"w":4,"h":4},"ops":[{"op":"add_layer","id":"a","rect":[0,0,NaN,1]}],"out":"png8"}',
                '{"canvas":{"w":4,"h":4},"ops":[],"out":"png8","extra":1}']:
        try:
            render_script_text(bad)
        except ScriptError:
            continue
        raise AssertionError("expected a script error for %s" % bad)


def test_cli():
    with tempfile.TemporaryDirectory() as td:
        good = os.path.join(td, "good.json")
        bad = os.path.join(td, "bad.json")
        png = os.path.join(td, "out.png")
        with open(good, "w") as fh:
            json.dump(script([solid("a", "#FF000080")]), fh)
        with open(bad, "w") as fh:
            json.dump(script([{"op": "add_layer", "id": "a", "bogus": 1}]), fh)
        cli = [sys.executable, os.path.join(HERE, "refcomp.py")]
        r = subprocess.run(cli + [bad, png], capture_output=True)
        assert r.returncode != 0 and not os.path.exists(png)
        r = subprocess.run(cli + [good, png], capture_output=True)
        assert r.returncode == 0, r.stderr
        from PIL import Image
        im = Image.open(png)
        assert im.mode == "RGBA" and tuple(np.asarray(im)[0, 0]) == (255, 0, 0, 128)


def test_speed_blend_rnd():
    """All 27 modes on the 256x256 `rnd` configuration: sanity + timing (should be ~seconds)."""
    from rc.blend import BLEND_MODES
    t0 = time.time()
    for m in BLEND_MODES:
        out = render([{"op": "add_layer", "id": "b", "fill": "noise", "seed": 21},
                      {"op": "add_layer", "id": "s", "fill": "noise", "seed": 22},
                      {"op": "set_opacity", "layer": "s", "value": 0.75},
                      {"op": "set_blend", "layer": "s", "mode": m, "seed": 7}], w=256, h=256)
        assert ((out[..., 3] != 0) | (out == 0).all(axis=-1)).all()     # canonical everywhere
    dt = time.time() - t0
    assert dt < 30, dt
    print("  27 modes x 256x256 rnd: %.2fs" % dt)


# ---------------------------------------------------------------------------------------------

def main():
    tests = [(k, v) for k, v in sorted(globals().items()) if k.startswith("test_") and callable(v)]
    failed = 0
    for name, fn in tests:
        try:
            r = fn()
            print("PASS %s%s" % (name, "" if r is None else "  (%.2fs)" % r))
        except Exception as e:           # noqa: BLE001
            failed += 1
            import traceback
            traceback.print_exc()
            print("FAIL %s: %s" % (name, e))
    print("%d/%d passed" % (len(tests) - failed, len(tests)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
