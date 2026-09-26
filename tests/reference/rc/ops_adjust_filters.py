"""Doc 20 (docs/math/20-adjustments-filters.md): the seven adjustment types not registered by
ops_compositing.py (Levels, Curves, Brightness/Contrast, Hue/Saturation, Black & White,
Posterize, Threshold; Invert lives there) and the six filters with the B0 framework.

Written from docs/math/ only (independence rule). C1: arrays use only + - * /, sqrt, floor,
min/max, comparisons and where, in the doc's evaluation order; transcendentals (pow, tan, cos,
sin, log) are Python `math` calls on scalars (LUT builds and per-op precomputes, and the
per-pixel Box-Muller of B4, which the doc says the reference evaluates per pixel on scalars).
No mutations are implemented here (doc 20 Part D): this module is always the correct version.
"""

import math
import re

import numpy as np

from .core import ScriptError, canonicalize, q, pixel_hash_grid, unit_grid
from .fields import is_int
from .registry import op, register_adjustment

K_PI = 3.141592653589793
K_DEG = K_PI / 180.0          # one division, computed once (C7)
K6 = 1.0 / 6.0                # doc 20 §A2

_HEX6_RE = re.compile(r"^#[0-9A-Fa-f]{6}$")


# ---------------------------------------------------------------------------------------------
# small scalar helpers
# ---------------------------------------------------------------------------------------------

def rhaz(y):
    """Round half away from zero of a Python float -> int (doc 20 notation)."""
    if y < 0.0:
        return -rhaz(-y)
    f = math.floor(y)
    return int(f) + (1 if (y - f) >= 0.5 else 0)


def _clamp(x, lo, hi):
    return min(max(x, lo), hi)


def _lut_apply(lr, lg, lb, r, g, b):
    return lr[r], lg[g], lb[b]


# =============================================================================================
# Part A — adjustments
# =============================================================================================

# -- A1. Levels -------------------------------------------------------------------------------

_LEVELS_KEYS = ("rgb", "r", "g", "b")


def _parse_levels_setting(s):
    in_black = s.int("in_black", 0, 0, 254)
    in_white = s.int("in_white", 255, 1, 255)
    if not in_black < in_white:
        raise s.err("in_black (%d) must be < in_white (%d)" % (in_black, in_white))
    gamma = s.num("gamma", 1.0, 0.1, 9.99)
    out_black = s.int("out_black", 0, 0, 255)
    out_white = s.int("out_white", 255, 0, 255)
    return (in_black, in_white, gamma, out_black, out_white)


def parse_levels(p):
    return {k: _parse_levels_setting(p.obj(k, {})) for k in _LEVELS_KEYS}


def _levels_map(S):
    """Return the scalar map L_S (doc 20 §A1), with its precompute done once in the doc's order."""
    in_black, in_white, gamma, out_black, out_white = S
    lb = in_black / 255.0
    hb = in_white / 255.0
    ob = out_black / 255.0
    ow = out_white / 255.0
    inv = 1.0 / gamma

    def L(x):
        v = (x - lb) / (hb - lb)
        v = _clamp(v, 0.0, 1.0)
        if gamma != 1.0 and v > 0.0:
            v = math.pow(v, inv)
        return ob + (v * (ow - ob))
    return L


def levels_luts(params):
    Lrgb = _levels_map(params["rgb"])
    luts = []
    for ch in ("r", "g", "b"):
        Lc = _levels_map(params[ch])
        luts.append(np.array([q(Lrgb(Lc(i / 255.0))) for i in range(256)], dtype=np.uint8))
    return luts


def apply_levels(params, r, g, b):
    lr, lg, lb = levels_luts(params)
    return _lut_apply(lr, lg, lb, r, g, b)


# -- A2. Curves -------------------------------------------------------------------------------

_IDENTITY_CURVE = [[0, 0], [255, 255]]


def _parse_curve(p, key):
    pts = p.raw(key, _IDENTITY_CURVE)
    if not isinstance(pts, list):
        raise p.err("%r must be an array of [in, out] points" % key)
    if not 2 <= len(pts) <= 16:
        raise p.err("%r: %d points (must be 2..16)" % (key, len(pts)))
    out = []
    for i, pt in enumerate(pts):
        if (not isinstance(pt, list) or len(pt) != 2 or not is_int(pt[0]) or not is_int(pt[1])):
            raise p.err("%r[%d] must be [in, out] integers" % (key, i))
        if not (0 <= pt[0] <= 255 and 0 <= pt[1] <= 255):
            raise p.err("%r[%d] = %r out of range 0..255" % (key, i, pt))
        if out and pt[0] <= out[-1][0]:
            raise p.err("%r: input levels must be strictly increasing" % key)
        out.append((pt[0], pt[1]))
    return out


def parse_curves(p):
    return {k: _parse_curve(p, k) for k in _LEVELS_KEYS}


def build_spline(points):
    """Natural cubic spline (doc 20 §A2 'Spline build'). Returns (X, Y, B, C, D) as float lists."""
    X = [float(x) for x, _ in points]
    Y = [float(y) for _, y in points]
    n = len(points)
    m = n - 1
    H = [X[i + 1] - X[i] for i in range(m)]
    C = [0.0] * (m + 1)
    s = m - 1
    if s >= 1:
        TB = [0.0] * s
        TF = [0.0] * s
        for k in range(s):
            TB[k] = 2.0 * (H[k] + H[k + 1])
            TF[k] = 6.0 * (((Y[k + 2] - Y[k + 1]) / H[k + 1]) - ((Y[k + 1] - Y[k]) / H[k]))
        TA = [H[k + 1] for k in range(s - 1)]
        U = [0.0] * s
        if s == 1:
            U[0] = TF[0] / TB[0]
        else:
            AL = [0.0] * s
            BE = [0.0] * s
            AL[1] = -TA[0] / TB[0]
            BE[1] = TF[0] / TB[0]
            for k in range(1, s - 1):
                den = (TA[k - 1] * AL[k]) + TB[k]
                AL[k + 1] = -TA[k] / den
                BE[k + 1] = (TF[k] - (TA[k - 1] * BE[k])) / den
            U[s - 1] = ((TF[s - 1] - (TA[s - 2] * BE[s - 1]))
                        / (TB[s - 1] + (TA[s - 2] * AL[s - 1])))
            for k in range(s - 2, -1, -1):
                U[k] = (AL[k + 1] * U[k + 1]) + BE[k + 1]
        for k in range(s):
            C[k + 1] = U[k]
    D = [0.0] * m
    B = [0.0] * m
    for i in range(m):
        D[i] = (C[i + 1] - C[i]) / H[i]
        B[i] = (((-0.5 * (C[i] * H[i])) - (K6 * ((D[i] * H[i]) * H[i])))
                + ((Y[i + 1] - Y[i]) / H[i]))
    return X, Y, B, C, D


def spline_eval(sp, x):
    """S(x) of doc 20 §A2 at a double x."""
    X, Y, B, C, D = sp
    m = len(X) - 1
    x = _clamp(x, X[0], X[m])
    i = 0
    for j in range(m):              # largest j in 0..m-1 with X[j] <= x
        if X[j] <= x:
            i = j
    t = x - X[i]
    y = ((Y[i] + (B[i] * t)) + (((0.5 * C[i]) * t) * t)) + ((((K6 * D[i]) * t) * t) * t)
    return _clamp(y, 0.0, 255.0)


def curves_luts(params):
    srgb = build_spline(params["rgb"])
    luts = []
    for ch in ("r", "g", "b"):
        sc = build_spline(params[ch])
        luts.append(np.array([q(spline_eval(srgb, spline_eval(sc, float(i))) / 255.0)
                              for i in range(256)], dtype=np.uint8))
    return luts


def apply_curves(params, r, g, b):
    lr, lg, lb = curves_luts(params)
    return _lut_apply(lr, lg, lb, r, g, b)


# -- A3. Brightness/Contrast ------------------------------------------------------------------

def parse_bc(p):
    return {"brightness": p.num("brightness", 0.0, -1.0, 1.0),
            "contrast": p.num("contrast", 0.0, -1.0, 1.0)}


def bc_lut(params):
    bh = params["brightness"] / 2.0
    if params["contrast"] == 0.0:
        slant = 1.0
    else:
        slant = math.tan((params["contrast"] + 1.0) * 0.7853981633974483)
    lut = []
    for i in range(256):
        v = i / 255.0
        if bh < 0.0:
            v = v * (1.0 + bh)
        else:
            v = v + ((1.0 - v) * bh)
        v = ((v - 0.5) * slant) + 0.5
        lut.append(q(v))
    return np.array(lut, dtype=np.uint8)


def apply_bc(params, r, g, b):
    lut = bc_lut(params)
    return lut[r], lut[g], lut[b]


# -- A4. HSL (shared with A5) -----------------------------------------------------------------

def rgb_to_hsl_scalar(r, g, b):
    mx = max(max(r, g), b)
    mn = min(min(r, g), b)
    l = (mx + mn) / 2.0
    d = mx - mn
    if d == 0.0:
        return (0.0, 0.0, l)
    if l <= 0.5:
        s = d / (mx + mn)
    else:
        s = d / ((2.0 - mx) - mn)
    if r == mx:
        h6 = (g - b) / d
    elif g == mx:
        h6 = 2.0 + ((b - r) / d)
    else:
        h6 = 4.0 + ((r - g) / d)
    if h6 < 0.0:
        h6 = h6 + 6.0
    return (h6, s, l)


def hsl_value_scalar(n1, n2, h):
    if h > 6.0:
        h = h - 6.0
    elif h < 0.0:
        h = h + 6.0
    if h < 1.0:
        return n1 + ((n2 - n1) * h)
    elif h < 3.0:
        return n2
    elif h < 4.0:
        return n1 + ((n2 - n1) * (4.0 - h))
    return n1


def hsl_to_rgb_scalar(h6, s, l):
    if s == 0.0:
        return (l, l, l)
    if l <= 0.5:
        m2 = l * (1.0 + s)
    else:
        m2 = (l + s) - (l * s)
    m1 = (2.0 * l) - m2
    return (hsl_value_scalar(m1, m2, h6 + 2.0),
            hsl_value_scalar(m1, m2, h6),
            hsl_value_scalar(m1, m2, h6 - 2.0))


def rgb_to_hsl_np(r, g, b):
    """Vectorised rgb_to_hsl: branches selected by masks; NaN/inf from unselected lanes discarded."""
    mx = np.maximum(np.maximum(r, g), b)
    mn = np.minimum(np.minimum(r, g), b)
    l = (mx + mn) / 2.0
    d = mx - mn
    zero = d == 0.0
    with np.errstate(divide="ignore", invalid="ignore"):
        s = np.where(l <= 0.5, d / (mx + mn), d / ((2.0 - mx) - mn))
        hr = (g - b) / d
        hg = 2.0 + ((b - r) / d)
        hb = 4.0 + ((r - g) / d)
    h6 = np.where(r == mx, hr, np.where(g == mx, hg, hb))
    h6 = np.where(h6 < 0.0, h6 + 6.0, h6)
    h6 = np.where(zero, 0.0, h6)
    s = np.where(zero, 0.0, s)
    return h6, s, l


def hsl_value_np(n1, n2, h):
    h = np.where(h > 6.0, h - 6.0, np.where(h < 0.0, h + 6.0, h))
    rise = n1 + ((n2 - n1) * h)
    fall = n1 + ((n2 - n1) * (4.0 - h))
    return np.where(h < 1.0, rise, np.where(h < 3.0, n2, np.where(h < 4.0, fall, n1)))


def hsl_to_rgb_np(h6, s, l):
    """Vectorised hsl_to_rgb; h6, s, l broadcastable float64 arrays (or scalars)."""
    h6, s, l = np.broadcast_arrays(np.asarray(h6, dtype=np.float64),
                                   np.asarray(s, dtype=np.float64),
                                   np.asarray(l, dtype=np.float64))
    m2 = np.where(l <= 0.5, l * (1.0 + s), (l + s) - (l * s))
    m1 = (2.0 * l) - m2
    grey = s == 0.0
    r = np.where(grey, l, hsl_value_np(m1, m2, h6 + 2.0))
    g = np.where(grey, l, hsl_value_np(m1, m2, h6))
    b = np.where(grey, l, hsl_value_np(m1, m2, h6 - 2.0))
    return r, g, b


def _dec3(r, g, b):
    return (r.astype(np.float64) / 255.0, g.astype(np.float64) / 255.0,
            b.astype(np.float64) / 255.0)


def parse_hs(p):
    colorize = p.bool("colorize", False)
    if colorize:
        hue = p.num("hue", 0.0, 0.0, 360.0)
        sat = p.num("saturation", 25.0, 0.0, 100.0)
    else:
        hue = p.num("hue", 0.0, -180.0, 180.0)
        sat = p.num("saturation", 0.0, -100.0, 100.0)
    light = p.num("lightness", 0.0, -100.0, 100.0)
    return {"colorize": colorize, "hue": hue, "saturation": sat, "lightness": light}


def _lightness(l, vl):
    if vl < 0.0:
        return l * (vl + 1.0)
    return l + (vl * (1.0 - l))


def apply_hs(params, r, g, b):
    rf, gf, bf = _dec3(r, g, b)
    vl = params["lightness"] / 100.0
    if not params["colorize"]:
        dh = params["hue"] / 60.0
        ks = 1.0 + (params["saturation"] / 100.0)
        h6, s, l = rgb_to_hsl_np(rf, gf, bf)
        h6 = h6 + dh
        h6 = np.where(h6 >= 6.0, h6 - 6.0, np.where(h6 < 0.0, h6 + 6.0, h6))
        l = _lightness(l, vl)
        s = np.minimum(np.maximum(s * ks, 0.0), 1.0)
        ro, go, bo = hsl_to_rgb_np(h6, s, l)
    else:
        hc = params["hue"] / 60.0
        if hc >= 6.0:
            hc = hc - 6.0
        sc = params["saturation"] / 100.0
        _, _, l = rgb_to_hsl_np(rf, gf, bf)
        l = _lightness(l, vl)
        ro, go, bo = hsl_to_rgb_np(hc, sc, l)
    return q(ro), q(go), q(bo)


# -- A5. Black & White ------------------------------------------------------------------------

_BW_FIELDS = (("reds", 40.0), ("yellows", 60.0), ("greens", 40.0), ("cyans", 60.0),
              ("blues", 20.0), ("magentas", 80.0))


def parse_bw(p):
    out = {name: p.num(name, dflt, -200.0, 300.0) for name, dflt in _BW_FIELDS}
    tint = p.raw("tint", None)
    if tint is not None:
        if not isinstance(tint, str) or not _HEX6_RE.match(tint):
            raise p.err("'tint' must be null or \"#RRGGBB\" (no alpha), got %r" % (tint,))
        tint = tuple(int(tint[i:i + 2], 16) for i in (1, 3, 5))
    out["tint"] = tint
    return out


def apply_bw(params, r, g, b):
    wR = params["reds"] / 100.0
    wY = params["yellows"] / 100.0
    wG = params["greens"] / 100.0
    wC = params["cyans"] / 100.0
    wB = params["blues"] / 100.0
    wM = params["magentas"] / 100.0
    R = r.astype(np.int64)
    G = g.astype(np.int64)
    B = b.astype(np.int64)
    mx = np.maximum(np.maximum(R, G), B)
    mn = np.minimum(np.minimum(R, G), B)
    md = ((R + G) + B) - mx - mn
    wp = np.where(R == mx, wR, np.where(G == mx, wG, wB))
    ws = np.where(B == mn, wY, np.where(R == mn, wC, wM))
    Y = (mn.astype(np.float64) + ((md - mn).astype(np.float64) * ws)) \
        + ((mx - md).astype(np.float64) * wp)
    gv = Y / 255.0
    if params["tint"] is None:
        qg = q(gv)
        return qg, qg.copy(), qg.copy()
    T = params["tint"]
    th6, ts, _ = rgb_to_hsl_scalar(T[0] / 255.0, T[1] / 255.0, T[2] / 255.0)
    gl = np.minimum(np.maximum(gv, 0.0), 1.0)
    ro, go, bo = hsl_to_rgb_np(th6, ts, gl)
    return q(ro), q(go), q(bo)


# -- A7. Posterize ----------------------------------------------------------------------------

def parse_posterize(p):
    return {"levels": p.int("levels", 4, 2, 255)}


def posterize_lut(params):
    kd = float(params["levels"] - 1)
    lut = []
    for i in range(256):
        t = (i / 255.0) * kd
        f = math.floor(t)
        j = f + (1.0 if (t - f) >= 0.5 else 0.0)
        lut.append(q(j / kd))
    return np.array(lut, dtype=np.uint8)


def apply_posterize(params, r, g, b):
    lut = posterize_lut(params)
    return lut[r], lut[g], lut[b]


# -- A8. Threshold ----------------------------------------------------------------------------

def parse_threshold(p):
    return {"level": p.int("level", 128, 1, 255)}


def apply_threshold(params, r, g, b):
    Y8 = ((((299 * r.astype(np.int64)) + (587 * g.astype(np.int64)))
           + (114 * b.astype(np.int64))) + 500) // 1000
    out = np.where(Y8 >= params["level"], 255, 0).astype(np.uint8)
    return out, out.copy(), out.copy()


register_adjustment("levels", parse_levels, apply_levels)
register_adjustment("curves", parse_curves, apply_curves)
register_adjustment("brightness_contrast", parse_bc, apply_bc)
register_adjustment("hue_saturation", parse_hs, apply_hs)
register_adjustment("black_white", parse_bw, apply_bw)
register_adjustment("posterize", parse_posterize, apply_posterize)
register_adjustment("threshold", parse_threshold, apply_threshold)


# =============================================================================================
# Part B — filters
# =============================================================================================

EDGE_MODES = ("clamp", "transparent")


# -- B0.2 coverage sources --------------------------------------------------------------------

def parse_coverage(doc, f):
    """-> None (M = 255 everywhere) or a uint8 (h, w) coverage array M."""
    if not f.has("coverage"):
        f.mark_used("coverage")
        return None
    c = f.obj("coverage")
    src = c.str("src", choices=("selection", "rect", "ramp"))
    W, H = doc.w, doc.h
    if src == "selection":
        return doc.selection_effective()
    if src == "rect":
        x = c.int("x")
        y = c.int("y")
        w = c.int("w", lo=0)
        h = c.int("h", lo=0)
        value = c.int("value", 255, 0, 255)
        M = np.zeros((H, W), dtype=np.uint8)
        x0, x1 = max(x, 0), min(x + w, W)
        y0, y1 = max(y, 0), min(y + h, H)
        if x0 < x1 and y0 < y1:
            M[y0:y1, x0:x1] = value
        return M
    d = c.str("dir", choices=("h", "v"))
    if d == "h":
        if W == 1:
            return np.full((H, W), 255, dtype=np.uint8)
        row = (np.arange(W, dtype=np.int64) * 255) // (W - 1)
        return np.broadcast_to(row[None, :], (H, W)).astype(np.uint8)
    if H == 1:
        return np.full((H, W), 255, dtype=np.uint8)
    col = (np.arange(H, dtype=np.int64) * 255) // (H - 1)
    return np.broadcast_to(col[:, None], (H, W)).astype(np.uint8)


# -- B0.1 coverage lerp -----------------------------------------------------------------------

def coverage_lerp(O, F, M):
    """R = coverage_lerp(O, F, M) of doc 20 §B0.1, canonical. O, F uint8 (h, w, 4); M uint8 (h, w)."""
    m = M.astype(np.float64) / 255.0
    aO = O[..., 3].astype(np.float64) / 255.0
    aF = F[..., 3].astype(np.float64) / 255.0
    a = (aO * (1.0 - m)) + (aF * m)
    R = np.empty_like(O)
    pos = a > 0.0
    safe_a = np.where(pos, a, 1.0)
    for ch in range(3):
        p = (((O[..., ch].astype(np.float64) / 255.0) * aO) * (1.0 - m)) \
            + (((F[..., ch].astype(np.float64) / 255.0) * aF) * m)
        C = np.where(pos, p / safe_a, 0.0)
        R[..., ch] = q(C)
    R[..., 3] = q(a)
    R = np.where((M == 0)[..., None], O, np.where((M == 255)[..., None], F, R))
    return canonicalize(np.ascontiguousarray(R, dtype=np.uint8))


def finish_filter(doc, layer, F, M):
    """The B0 pipeline after `F = filter(O, params)`: lock, coverage, write."""
    O = layer.rgba
    F = np.array(F, dtype=np.uint8, copy=True)
    if layer.lock_alpha:
        F[..., 3] = O[..., 3]
        F[O[..., 3] == 0] = 0
    if M is None:
        R = canonicalize(F)
    else:
        R = coverage_lerp(O, F, M)
    layer.rgba = canonicalize(R)


# -- B1 Gaussian blur -------------------------------------------------------------------------

def box_widths(s):
    wi = math.sqrt((((12.0 * s) * s) / 3.0) + 1.0)
    wl = int(math.floor(wi))
    if wl % 2 == 0:
        wl = wl - 1
    wu = wl + 2
    mi = (((((12.0 * s) * s) - (3 * wl * wl)) - (12 * wl)) - 9) / ((-4.0 * wl) - 4.0)
    m = _clamp(rhaz(mi), 0, 3)
    return [wl if j < m else wu for j in range(3)]


def box_pass(P, w, axis, edge):
    """One 1-D box pass of odd width w on an int64 array along `axis` (0 = y, 1 = x)."""
    if w == 1:
        return P
    r = (w - 1) // 2
    pad = [(0, 0)] * P.ndim
    pad[axis] = (r, r)
    Pp = np.pad(P, pad, mode="edge" if edge == "clamp" else "constant")
    cs = np.cumsum(Pp, axis=axis, dtype=np.int64)
    zshape = list(cs.shape)
    zshape[axis] = 1
    cs = np.concatenate([np.zeros(zshape, dtype=np.int64), cs], axis=axis)
    n = P.shape[axis]
    hi = np.take(cs, np.arange(w, w + n), axis=axis)
    lo = np.take(cs, np.arange(0, n), axis=axis)
    return ((hi - lo) + r) // w


def gaussian_blur(O, radius, edge):
    """Complete B1 output bytes (Stages 1-4, canonical), no coverage, no lock."""
    widths = box_widths(radius)
    A8 = O[..., 3].astype(np.int64)
    planes = [O[..., c].astype(np.int64) * A8 for c in range(3)] + [A8 * 255]
    P = np.stack(planes, axis=-1)                     # (h, w, 4) int64
    for w in widths:                                  # Stage 2: horizontal
        P = box_pass(P, w, 1, edge)
    for w in widths:                                  # Stage 3: vertical
        P = box_pass(P, w, 0, edge)
    Pa = P[..., 3]
    a = Pa.astype(np.float64) / 65025.0
    pos = Pa > 0
    den = np.where(pos, Pa, 1).astype(np.float64)
    out = np.empty(O.shape, dtype=np.uint8)
    for c in range(3):
        C = np.where(pos, P[..., c].astype(np.float64) / den, 0.0)
        out[..., c] = q(C)
    out[..., 3] = q(a)
    return canonicalize(out)


@op("filter_gaussian_blur")
def filter_gaussian_blur(doc, f):
    layer = doc.get_raster(f.str("layer"))
    radius = f.num("radius", 1.0, 0.1, 250.0)
    edge = f.str("edge", "clamp", choices=EDGE_MODES)
    M = parse_coverage(doc, f)
    finish_filter(doc, layer, gaussian_blur(layer.rgba, radius, edge), M)


# -- B2 Motion blur ---------------------------------------------------------------------------

def motion_taps(angle, distance):
    th = angle * K_DEG
    ox = distance * math.cos(th)
    oy = -(distance * math.sin(th))
    if abs(ox) < 1e-9:
        ox = 0.0
    if abs(oy) < 1e-9:
        oy = 0.0
    N = int(math.ceil(distance)) + 1
    taps = []
    for s in range(N):
        t = (float(s) / float(N - 1)) - 0.5
        tx = t * ox
        ty = t * oy
        fx = math.floor(tx)
        fy = math.floor(ty)
        taps.append((int(fx), tx - fx, int(fy), ty - fy))
    return N, taps


def _shift_read(plane, dx, dy, edge):
    """E(x + dx, y + dy) for every (x, y): plane float64 (h, w)."""
    H, W = plane.shape
    xs = np.arange(W) + dx
    ys = np.arange(H) + dy
    xc = np.clip(xs, 0, W - 1)
    yc = np.clip(ys, 0, H - 1)
    out = plane[yc][:, xc]
    if edge == "transparent":
        ok = ((ys >= 0) & (ys < H))[:, None] & ((xs >= 0) & (xs < W))[None, :]
        out = np.where(ok, out, 0.0)
    return out


def motion_blur(O, angle, distance, edge):
    N, taps = motion_taps(angle, distance)
    A8 = O[..., 3].astype(np.int64)
    planes = [(O[..., c].astype(np.int64) * A8).astype(np.float64) / 65025.0 for c in range(3)]
    planes.append(A8.astype(np.float64) / 255.0)
    accs = []
    for pl in planes:
        acc = np.zeros(pl.shape, dtype=np.float64)
        for IX, FX, IY, FY in taps:
            p0 = _shift_read(pl, IX, IY, edge)
            p1 = _shift_read(pl, IX + 1, IY, edge)
            p2 = _shift_read(pl, IX, IY + 1, edge)
            p3 = _shift_read(pl, IX + 1, IY + 1, edge)
            m0 = (FY * (p2 - p0)) + p0
            m1 = (FY * (p3 - p1)) + p1
            acc = acc + ((FX * (m1 - m0)) + m0)
        accs.append(acc)
    Nd = float(N)
    a = accs[3] / Nd
    pos = a > 0.0
    den = np.where(pos, a, 1.0)
    out = np.empty(O.shape, dtype=np.uint8)
    for c in range(3):
        C = np.where(pos, (accs[c] / Nd) / den, 0.0)
        out[..., c] = q(C)
    out[..., 3] = q(a)
    return canonicalize(out)


@op("filter_motion_blur")
def filter_motion_blur(doc, f):
    layer = doc.get_raster(f.str("layer"))
    angle = f.num("angle", 0.0, -360.0, 360.0)
    distance = f.num("distance", 10.0, 1.0, 2000.0)
    edge = f.str("edge", "clamp", choices=EDGE_MODES)
    M = parse_coverage(doc, f)
    finish_filter(doc, layer, motion_blur(layer.rgba, angle, distance, edge), M)


# -- B3 Unsharp mask --------------------------------------------------------------------------

def unsharp_mask(O, amount, radius, threshold, edge):
    Bl = gaussian_blur(O, radius, edge)
    k = amount / 100.0
    F = O.copy()
    for c in range(3):
        Oc = O[..., c].astype(np.int64)
        d = Oc - Bl[..., c].astype(np.int64)
        sharp = q((Oc.astype(np.float64) + (k * d.astype(np.float64))) / 255.0)
        F[..., c] = np.where(np.abs(d) < threshold, O[..., c], sharp)
    F[..., 3] = O[..., 3]
    return canonicalize(F)


@op("filter_unsharp_mask")
def filter_unsharp_mask(doc, f):
    layer = doc.get_raster(f.str("layer"))
    amount = f.num("amount", 50.0, 1.0, 500.0)
    radius = f.num("radius", 1.0, 0.1, 250.0)
    threshold = f.int("threshold", 0, 0, 255)
    edge = f.str("edge", "clamp", choices=EDGE_MODES)
    M = parse_coverage(doc, f)
    finish_filter(doc, layer, unsharp_mask(layer.rgba, amount, radius, threshold, edge), M)


# -- B4 Add noise -----------------------------------------------------------------------------

def _gauss_z(u1, u2):
    """Box-Muller cosine branch per pixel on Python scalars (libm log/cos, correctly rounded sqrt)."""
    flat1 = u1.ravel().tolist()
    flat2 = u2.ravel().tolist()
    log, cos, sqrt = math.log, math.cos, math.sqrt
    z = [sqrt(-2.0 * log(1.0 - a)) * cos(6.283185307179586 * b) for a, b in zip(flat1, flat2)]
    return np.array(z, dtype=np.float64).reshape(u1.shape)


def add_noise(O, amount, distribution, mono, seed):
    H, W = O.shape[:2]
    A = (amount * 255.0) / 100.0
    sigma = A / 1.7320508075688772

    def noise(c):
        if distribution == "uniform":
            k = 0 if mono else c
            u = unit_grid(pixel_hash_grid(seed, W, H, k))
            return A * ((2.0 * u) - 1.0)
        k1, k2 = (0, 1) if mono else (2 * c, (2 * c) + 1)
        u1 = unit_grid(pixel_hash_grid(seed, W, H, k1))
        u2 = unit_grid(pixel_hash_grid(seed, W, H, k2))
        return sigma * _gauss_z(u1, u2)

    F = O.copy()
    shared = noise(0) if mono else None
    for c in range(3):
        n = shared if mono else noise(c)
        F[..., c] = q((O[..., c].astype(np.float64) + n) / 255.0)
    F[..., 3] = O[..., 3]
    return canonicalize(F)


@op("filter_add_noise")
def filter_add_noise(doc, f):
    layer = doc.get_raster(f.str("layer"))
    amount = f.num("amount", 10.0, 0.0, 400.0)
    distribution = f.str("distribution", "uniform", choices=("uniform", "gaussian"))
    mono = f.bool("monochromatic", False)
    seed = f.int("seed", 0, 0, 9007199254740991)
    M = parse_coverage(doc, f)
    finish_filter(doc, layer, add_noise(layer.rgba, amount, distribution, mono, seed), M)


# -- B5 High pass -----------------------------------------------------------------------------

def high_pass(O, radius, edge):
    Bl = gaussian_blur(O, radius, edge)
    F = O.copy()
    for c in range(3):
        d = O[..., c].astype(np.int64) - Bl[..., c].astype(np.int64)
        F[..., c] = q((d.astype(np.float64) / 255.0) + 0.5)
    F[..., 3] = O[..., 3]
    return canonicalize(F)


@op("filter_high_pass")
def filter_high_pass(doc, f):
    layer = doc.get_raster(f.str("layer"))
    radius = f.num("radius", 10.0, 0.1, 250.0)
    edge = f.str("edge", "clamp", choices=EDGE_MODES)
    M = parse_coverage(doc, f)
    finish_filter(doc, layer, high_pass(layer.rgba, radius, edge), M)


# -- B6 Offset --------------------------------------------------------------------------------

def offset(O, dx, dy, mode):
    H, W = O.shape[:2]
    sx = np.arange(W) - dx
    sy = np.arange(H) - dy
    if mode == "wrap":
        return O[np.mod(sy, H)][:, np.mod(sx, W)].copy()
    xc = np.clip(sx, 0, W - 1)
    yc = np.clip(sy, 0, H - 1)
    F = O[yc][:, xc].copy()
    if mode == "transparent":
        ok = ((sy >= 0) & (sy < H))[:, None] & ((sx >= 0) & (sx < W))[None, :]
        F[~ok] = 0
    return F


@op("filter_offset")
def filter_offset(doc, f):
    layer = doc.get_raster(f.str("layer"))
    dx = f.int("dx", 0, -65536, 65536)
    dy = f.int("dy", 0, -65536, 65536)
    mode = f.str("mode", "transparent", choices=("transparent", "repeat", "wrap"))
    M = parse_coverage(doc, f)
    finish_filter(doc, layer, offset(layer.rgba, dx, dy, mode), M)
