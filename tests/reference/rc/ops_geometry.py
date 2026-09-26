"""Doc 30: geometry, selections, transform, gradient, paint bucket (docs/math/30-geometry-selection.md).

Written from docs/math/ only (00-conventions + 30-geometry-selection). Section numbers in the
comments are doc 30's. Every formula is evaluated in the doc's order (C1): + - * / sqrt floor
min max where are vectorised with NumPy; cos/sin/tan are Python `math` scalars.

Readings where the doc is silent (also reported to the lead):
  * image_size with W2 == W and H2 == H: pixels untouched ("no-op"), selection still cleared (§2).
  * rotate_canvas (arbitrary angle), layer masks: a destination pixel whose source position fails
    the §12.4 horizon/range test is 255 (every tap would be outside = 1.0, §11.4).
  * transform / flip ignore the layer's lock_alpha (C8a lists only painting ops and filters).
"""

import math

import numpy as np

from .core import ScriptError, canonicalize, dec, q
from .fields import is_int, is_number
from .registry import op

PI = 3.141592653589793
DEG = PI / 180.0                 # C7 / §12.1: rad = deg * (pi / 180.0), the division first

MODES = ("new", "add", "subtract", "intersect")
ANCHORS = ("tl", "t", "tr", "l", "c", "r", "bl", "b", "br")

# §1.3: sample offsets o(i) = (2*i + 1) / 32.0, i = 0..15 (exact dyadic doubles)
OFFS = np.array([(2 * i + 1) / 32.0 for i in range(16)], dtype=np.float64)


# =============================================================================================
# field helpers
# =============================================================================================

def _finite(f, name, v):
    if not is_number(v):
        raise f.err("%r must contain numbers" % name)
    try:
        v = float(v)
    except OverflowError:
        raise f.err("%r: number out of range" % name)
    if v != v or v in (float("inf"), float("-inf")):
        raise f.err("%r: numbers must be finite" % name)
    return v


def _num_list(f, name, n, default=None):
    """A JSON array of exactly n numbers -> list of floats (required when default is None)."""
    if default is None:
        v = f.raw(name)
    else:
        if not f.has(name):
            f.mark_used(name)
            return list(default)
        v = f.raw(name)
    if not isinstance(v, list) or len(v) != n:
        raise f.err("%r must be an array of %d numbers" % (name, n))
    return [_finite(f, name, e) for e in v]


def _points(f, name, lo, hi):
    v = f.raw(name)
    if not isinstance(v, list) or not (lo <= len(v) <= hi):
        raise f.err("%r must be an array of %d..%d [x, y] points" % (name, lo, hi))
    out = []
    for p in v:
        if not isinstance(p, list) or len(p) != 2:
            raise f.err("%r: every point must be [num, num]" % name)
        out.append((_finite(f, name, p[0]), _finite(f, name, p[1])))
    return out


def _mode(f):
    return f.str("mode", "new", choices=MODES)


def _colour_doubles(c):
    return dec(c[0]), dec(c[1]), dec(c[2]), dec(c[3])


# =============================================================================================
# §4 boolean combine, §2 selection state
# =============================================================================================

def combine(S, B, mode):
    """§4, integer arithmetic on bytes."""
    Si = S.astype(np.int32)
    Bi = B.astype(np.int32)
    if mode == "new":
        R = Bi
    elif mode == "add":
        R = np.maximum(Si, Bi)
    elif mode == "subtract":
        R = np.maximum(Si - Bi, 0)
    elif mode == "intersect":
        R = np.minimum(Si, Bi)
    else:
        raise AssertionError(mode)
    return R.astype(np.uint8)


def _set_candidate(doc, B, mode):
    doc.sel = combine(doc.sel, B, mode)


def _clear_selection(doc):
    """§2: canvas-geometry ops set S to empty (new size) and Saved to absent."""
    doc.sel = np.zeros((doc.h, doc.w), dtype=np.uint8)
    doc.sel_saved = None


def effective_e(doc):
    """§2: e(x, y) = dec(E(x, y)), float64 (h, w)."""
    return dec(doc.selection_effective())


# =============================================================================================
# §1.3 / §3 shape rasterisation
# =============================================================================================

def _row_chunks(y0, y1, step=32):
    y = y0
    while y < y1:
        yield y, min(y + step, y1)
        y += step


def _bbox(xmin, ymin, xmax, ymax, W, H):
    """Integer pixel box [x0, x1) x [y0, y1) with a 1-pixel safety margin, clipped to the canvas.

    Skipping pixels outside it is exact (§1.3): every sample there is > 1/32 px from the shape's
    real extent, far beyond any last-ulp rounding of the inside tests."""
    x0 = max(int(math.floor(xmin)) - 1, 0)
    y0 = max(int(math.floor(ymin)) - 1, 0)
    x1 = min(int(math.floor(xmax)) + 2, W)
    y1 = min(int(math.floor(ymax)) + 2, H)
    return x0, y0, x1, y1


def _supersample(W, H, box, inside_fn):
    """§1.3 AA coverage: inside_fn(px (1, nx*16), py (ny*16, 1)) -> bool (ny*16, nx*16)."""
    out = np.zeros((H, W), dtype=np.uint8)
    x0, y0, x1, y1 = box
    if x0 >= x1 or y0 >= y1:
        return out
    px = (np.arange(x0, x1, dtype=np.float64)[:, None] + OFFS[None, :]).reshape(1, -1)
    for ya, yb in _row_chunks(y0, y1):
        py = (np.arange(ya, yb, dtype=np.float64)[:, None] + OFFS[None, :]).reshape(-1, 1)
        ins = inside_fn(px, py)
        n = ins.reshape(yb - ya, 16, x1 - x0, 16).sum(axis=(1, 3))
        out[ya:yb, x0:x1] = q(n.astype(np.float64) / 256.0)
    return out


def _centre_sample(W, H, box, inside_fn):
    """Non-AA coverage: the single sample at the pixel centre, 255 or 0."""
    out = np.zeros((H, W), dtype=np.uint8)
    x0, y0, x1, y1 = box
    if x0 >= x1 or y0 >= y1:
        return out
    px = (np.arange(x0, x1, dtype=np.float64) + 0.5)[None, :]
    py = (np.arange(y0, y1, dtype=np.float64) + 0.5)[:, None]
    out[y0:y1, x0:x1] = np.where(inside_fn(px, py), 255, 0).astype(np.uint8)
    return out


def raster_rect(W, H, x, y, w, h, aa):
    """§3.1. The inside test is a conjunction of an x-test and a y-test, so the AA sample count is
    exactly (x-samples inside) * (y-samples inside)."""
    x1 = x + w
    y1 = y + h
    if not aa:
        px = np.arange(W, dtype=np.float64) + 0.5
        py = np.arange(H, dtype=np.float64) + 0.5
        ix = (x <= px) & (px < x1)
        iy = (y <= py) & (py < y1)
        return np.where(iy[:, None] & ix[None, :], 255, 0).astype(np.uint8)
    px = np.arange(W, dtype=np.float64)[:, None] + OFFS[None, :]      # (W, 16)
    py = np.arange(H, dtype=np.float64)[:, None] + OFFS[None, :]      # (H, 16)
    cx = ((x <= px) & (px < x1)).sum(axis=1).astype(np.int64)
    cy = ((y <= py) & (py < y1)).sum(axis=1).astype(np.int64)
    n = cy[:, None] * cx[None, :]
    return q(n.astype(np.float64) / 256.0)


def raster_ellipse(W, H, x, y, w, h, aa):
    """§3.2."""
    rx = w / 2.0
    ry = h / 2.0
    cx = x + rx
    cy = y + ry

    def inside(px, py):
        dx = (px - cx) / rx
        dy = (py - cy) / ry
        return ((dx * dx) + (dy * dy)) <= 1.0

    box = _bbox(x, y, x + w, y + h, W, H)
    return (_supersample if aa else _centre_sample)(W, H, box, inside)


def raster_polygon(W, H, pts, aa):
    """§3.3 PNPOLY even-odd. For one edge, the crossing test and xc depend only on py, so they
    are evaluated once per sample row (identical doubles to evaluating them per sample)."""
    n = len(pts)

    def inside(px, py):
        c = np.zeros((py.shape[0], px.shape[1]), dtype=bool)
        for k in range(n):                       # ascending
            xi, yi = pts[k]
            xj, yj = pts[(k + n - 1) % n]        # previous vertex
            cond = (yi > py) != (yj > py)        # (rows, 1)
            if not cond.any():
                continue                         # (also every horizontal edge: yj == yi)
            den = yj - yi                        # nonzero wherever cond holds
            xc = (((xj - xi) * (py - yi)) / den) + xi
            c ^= cond & (px < xc)
        return c

    xs = [p[0] for p in pts]
    ys = [p[1] for p in pts]
    box = _bbox(min(xs), min(ys), max(xs), max(ys), W, H)
    return (_supersample if aa else _centre_sample)(W, H, box, inside)


# =============================================================================================
# §6 region (magic wand, paint bucket)
# =============================================================================================

def _neighbour8_any(C):
    """True where at least one of the 8 in-canvas neighbours is True."""
    H, W = C.shape
    P = np.zeros((H + 2, W + 2), dtype=bool)
    P[1:-1, 1:-1] = C
    out = np.zeros((H, W), dtype=bool)
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            if dx == 0 and dy == 0:
                continue
            out |= P[1 + dy:1 + dy + H, 1 + dx:1 + dx + W]
    return out


def _component8(M, sx, sy):
    """The 8-connected component of M containing (sx, sy) (a set; order-independent)."""
    H, W = M.shape
    Mf = M.ravel()
    C = np.zeros(H * W, dtype=bool)
    start = sy * W + sx
    C[start] = True
    stack = [start]
    while stack:
        i = stack.pop()
        y, x = divmod(i, W)
        for dy in (-1, 0, 1):
            yy = y + dy
            if yy < 0 or yy >= H:
                continue
            for dx in (-1, 0, 1):
                xx = x + dx
                if (dx == 0 and dy == 0) or xx < 0 or xx >= W:
                    continue
                j = yy * W + xx
                if Mf[j] and not C[j]:
                    C[j] = True
                    stack.append(j)
    return C.reshape(H, W)


def region(layer_rgba, sx, sy, t, contiguous, aa):
    """§6.3 -> byte array Rg (h, w)."""
    H, W = layer_rgba.shape[:2]
    Rg = np.zeros((H, W), dtype=np.uint8)
    if not (0 <= sx < W and 0 <= sy < H):
        return Rg
    L = layer_rgba.astype(np.int32)
    seed = L[sy, sx]
    d = np.abs(L - seed[None, None, :]).max(axis=2)          # §6.2, integers 0..255
    M = d <= t
    C = _component8(M, sx, sy) if contiguous else M
    Rg[C] = 255
    if aa and t > 0:
        fringe = (~C) & _neighbour8_any(C)
        a = 1.5 - (d.astype(np.float64) / float(t))
        val = np.where(a <= 0.0, 0.0, a * 2.0)
        qv = q(val)
        Rg[fringe] = np.where(a[fringe] <= 0.0, 0, qv[fringe]).astype(np.uint8)
    return Rg


# =============================================================================================
# §1.2 paint_over
# =============================================================================================

def paint_over(layer, sr, sg, sb, as_):
    """§1.2 on a whole layer. sr/sg/sb: scalars or (h, w) float64; as_: (h, w) float64."""
    rgba = layer.rgba
    H, W = rgba.shape[:2]
    as_ = np.broadcast_to(np.asarray(as_, dtype=np.float64), (H, W))
    s = [np.broadcast_to(np.asarray(v, dtype=np.float64), (H, W)) for v in (sr, sg, sb)]
    Ab = rgba[..., 3]
    ab = dec(Ab)
    touch = as_ != 0.0
    out = rgba.copy()
    one_minus = 1.0 - as_
    if layer.lock_alpha:
        touch = touch & (Ab != 0)
        for k in range(3):
            ck = (dec(rgba[..., k]) * one_minus) + (s[k] * as_)
            out[..., k] = np.where(touch, q(ck), rgba[..., k])
        # alpha byte unchanged
    else:
        ao = as_ + (ab * one_minus)
        safe = np.where(touch, ao, 1.0)                       # ao > 0 wherever as > 0
        for k in range(3):
            ck = ((s[k] * as_) + ((dec(rgba[..., k]) * ab) * one_minus)) / safe
            out[..., k] = np.where(touch, q(np.where(touch, ck, 0.0)), rgba[..., k])
        out[..., 3] = np.where(touch, q(np.where(touch, ao, 0.0)), Ab)
    layer.rgba = canonicalize(out)


# =============================================================================================
# §11 kernel
# =============================================================================================

def K_scalar(d):
    """§11.1 Keys a = -0.5, Pillow form, on a Python float."""
    x = abs(d)
    if x < 1.0:
        return ((((1.5 * x) - 2.5) * x) * x) + 1.0
    if x < 2.0:
        return (((((x - 5.0) * x) + 8.0) * x) - 4.0) * (-0.5)
    return 0.0


def K_array(d):
    """§11.1 on a float64 array (identical operation sequence per element)."""
    x = np.abs(d)
    k1 = ((((1.5 * x) - 2.5) * x) * x) + 1.0
    k2 = (((((x - 5.0) * x) + 8.0) * x) - 4.0) * (-0.5)
    return np.where(x < 1.0, k1, np.where(x < 2.0, k2, 0.0))


def premultiply(rgba):
    """§11.2: (h, w, 4) float64 with (pr, pg, pb, pa)."""
    pa = dec(rgba[..., 3])
    out = np.empty(rgba.shape, dtype=np.float64)
    for k in range(3):
        out[..., k] = dec(rgba[..., k]) * pa
    out[..., 3] = pa
    return out


def finish(acc):
    """§11.3: (h, w, 4) accumulated doubles -> canonical uint8 RGBA."""
    A = np.minimum(np.maximum(acc[..., 3], 0.0), 1.0)
    zero = A == 0.0
    safeA = np.where(zero, 1.0, A)
    out = np.zeros(acc.shape, dtype=np.uint8)
    for k in range(3):
        P = np.minimum(np.maximum(acc[..., k], 0.0), A)
        out[..., k] = q(P / safeA)
    out[..., 3] = q(A)
    out[zero] = 0
    return canonicalize(out)


# =============================================================================================
# §12 transform
# =============================================================================================

def mat_mul(A, B):
    """§12.1: C[i][j] = ((A[i][0]*B[0][j]) + (A[i][1]*B[1][j])) + (A[i][2]*B[2][j]); 9-lists."""
    C = [0.0] * 9
    for i in range(3):
        for j in range(3):
            C[3 * i + j] = (((A[3 * i + 0] * B[0 + j]) + (A[3 * i + 1] * B[3 + j]))
                            + (A[3 * i + 2] * B[6 + j]))
    return C


def Tr(tx, ty):
    return [1.0, 0.0, tx, 0.0, 1.0, ty, 0.0, 0.0, 1.0]


def Sc(sx, sy):
    return [sx, 0.0, 0.0, 0.0, sy, 0.0, 0.0, 0.0, 1.0]


def Ro(deg):
    rad = deg * DEG
    c = math.cos(rad)
    s = math.sin(rad)
    return [c, -s, 0.0, s, c, 0.0, 0.0, 0.0, 1.0]


def Sk(kx, ky):
    hx = math.tan(kx * DEG)
    hy = math.tan(ky * DEG)
    return [1.0, hx, 0.0, hy, 1.0, 0.0, 0.0, 0.0, 1.0]


def params_matrix(tx, ty, sx, sy, deg, kx, ky, px, py):
    """§12.2 form 2."""
    M = Tr(-px, -py)
    M = mat_mul(Sc(sx, sy), M)
    M = mat_mul(Sk(kx, ky), M)
    M = mat_mul(Ro(deg), M)
    M = mat_mul(Tr(px + tx, py + ty), M)
    return M


def quad_matrix(rect, quad):
    """§12.2 form 3 (Heckbert square-to-quad, then rect -> unit square). Raises on den == 0."""
    rx, ry, rw, rh = rect
    (x0, y0), (x1, y1), (x2, y2), (x3, y3) = quad
    sx = ((x0 - x1) + x2) - x3
    sy = ((y0 - y1) + y2) - y3
    if sx == 0.0 and sy == 0.0:
        g = 0.0
        h = 0.0
        a = x1 - x0
        b = x3 - x0
        c = x0
        d = y1 - y0
        e = y3 - y0
        f = y0
    else:
        dx1 = x1 - x2
        dx2 = x3 - x2
        dy1 = y1 - y2
        dy2 = y3 - y2
        den = (dx1 * dy2) - (dx2 * dy1)
        if den == 0.0:
            raise ScriptError("transform: degenerate quad (den == 0)")
        g = ((sx * dy2) - (dx2 * sy)) / den
        h = ((dx1 * sy) - (sx * dy1)) / den
        a = (x1 - x0) + (g * x1)
        b = (x3 - x0) + (h * x3)
        c = x0
        d = (y1 - y0) + (g * y1)
        e = (y3 - y0) + (h * y3)
        f = y0
    Q = [a, b, c, d, e, f, g, h, 1.0]
    N = [1.0 / rw, 0.0, (-rx) / rw, 0.0, 1.0 / rh, (-ry) / rh, 0.0, 0.0, 1.0]
    return mat_mul(Q, N)


def invert(m):
    """§12.3 adjugate / determinant; singular -> script error."""
    m0, m1, m2, m3, m4, m5, m6, m7, m8 = m
    i0 = (m4 * m8) - (m5 * m7)
    i1 = (m2 * m7) - (m1 * m8)
    i2 = (m1 * m5) - (m2 * m4)
    i3 = (m5 * m6) - (m3 * m8)
    i4 = (m0 * m8) - (m2 * m6)
    i5 = (m2 * m3) - (m0 * m5)
    i6 = (m3 * m7) - (m4 * m6)
    i7 = (m1 * m6) - (m0 * m7)
    i8 = (m0 * m4) - (m1 * m3)
    det = ((m0 * i0) + (m1 * i3)) + (m2 * i6)
    if not (abs(det) >= 1e-12):
        raise ScriptError("transform: singular matrix (det = %r)" % det)
    return [i / det for i in (i0, i1, i2, i3, i4, i5, i6, i7, i8)]


def _map(n, Wd, Hd, Ws, Hs):
    """§12.4 mapping for every destination pixel. Returns (valid, sx, sy)."""
    n0, n1, n2, n3, n4, n5, n6, n7, n8 = n
    X = (np.arange(Wd, dtype=np.float64) + 0.5)[None, :]
    Y = (np.arange(Hd, dtype=np.float64) + 0.5)[:, None]
    u = ((n0 * X) + (n1 * Y)) + n2
    v = ((n3 * X) + (n4 * Y)) + n5
    w = ((n6 * X) + (n7 * Y)) + n8
    u, v, w = np.broadcast_arrays(u, v, w)
    with np.errstate(all="ignore"):
        front = w > 0.0
        wd = np.where(front, w, 1.0)
        sx = u / wd
        sy = v / wd
        ok = front & (sx >= -4.0) & (sx <= Ws + 4.0) & (sy >= -4.0) & (sy <= Hs + 4.0)
    sx = np.where(ok, sx, 0.0)
    sy = np.where(ok, sy, 0.0)
    return ok, sx, sy


PAD = 8   # tap indices lie in [-6, W + 6) for sx in [-4, W + 4]


def _bicubic_taps(ok, sx, sy):
    fx = sx - 0.5
    fy = sy - 0.5
    ix = np.floor(fx).astype(np.int64)
    iy = np.floor(fy).astype(np.int64)
    cx = [ix - 1 + k for k in range(4)]
    cy = [iy - 1 + k for k in range(4)]
    wx = [K_array(fx - c.astype(np.float64)) for c in cx]
    wy = [K_array(fy - c.astype(np.float64)) for c in cy]
    return cx, cy, wx, wy


def resample_rgba(src, n, Wd, Hd, interp):
    """§12.4 for an RGBA layer: returns the (Hd, Wd, 4) canonical uint8 result."""
    Hs, Ws = src.shape[:2]
    ok, sx, sy = _map(n, Wd, Hd, Ws, Hs)
    if interp == "nearest":
        ix = np.floor(sx).astype(np.int64)
        iy = np.floor(sy).astype(np.int64)
        inb = ok & (ix >= 0) & (ix < Ws) & (iy >= 0) & (iy < Hs)
        out = src[np.where(inb, iy, 0), np.where(inb, ix, 0)].copy()
        out[~inb] = 0
        return canonicalize(out)
    P = np.zeros((Hs + 2 * PAD, Ws + 2 * PAD, 4), dtype=np.float64)   # outside = transparent
    P[PAD:PAD + Hs, PAD:PAD + Ws] = premultiply(src)
    cx, cy, wx, wy = _bicubic_taps(ok, sx, sy)
    out = np.zeros((Hd, Wd, 4), dtype=np.float64)
    for ky in range(4):
        row = np.zeros((Hd, Wd, 4), dtype=np.float64)
        for kx in range(4):
            row = row + (wx[kx][..., None] * P[cy[ky] + PAD, cx[kx] + PAD])
        out = out + (wy[ky][..., None] * row)
    out[~ok] = 0.0
    res = finish(out)
    res[~ok] = 0
    return res


def resample_mask(src, n, Wd, Hd):
    """§11.4 grey channel through §12.4 bicubic, taps outside the source = 1.0."""
    Hs, Ws = src.shape
    ok, sx, sy = _map(n, Wd, Hd, Ws, Hs)
    P = np.ones((Hs + 2 * PAD, Ws + 2 * PAD), dtype=np.float64)
    P[PAD:PAD + Hs, PAD:PAD + Ws] = dec(src)
    cx, cy, wx, wy = _bicubic_taps(ok, sx, sy)
    out = np.zeros((Hd, Wd), dtype=np.float64)
    for ky in range(4):
        row = np.zeros((Hd, Wd), dtype=np.float64)
        for kx in range(4):
            row = row + (wx[kx] * P[cy[ky] + PAD, cx[kx] + PAD])
        out = out + (wy[ky] * row)
    res = q(np.minimum(np.maximum(out, 0.0), 1.0))
    res[~ok] = 255          # reading (module docstring): every tap outside = reveal
    return res


# =============================================================================================
# §13 image size
# =============================================================================================

def pillow_coeffs(Nin, Nout):
    """§13 per-axis weights -> (idx (Nout, T) int, wt (Nout, T) float64), zero-padded taps last."""
    scale = Nin / Nout
    fs = max(scale, 1.0)
    supp = 2.0 * fs
    inv_fs = 1.0 / fs
    rows = []
    for o in range(Nout):
        center = (o + 0.5) * scale
        lo = max(int(math.floor((center - supp) + 0.5)), 0)
        hi = min(int(math.floor((center + supp) + 0.5)), Nin)
        ks = [K_scalar(((i - center) + 0.5) * inv_fs) for i in range(lo, hi)]
        ww = 0.0
        for k in ks:
            ww = ww + k
        wts = [(k / ww) if ww != 0.0 else k for k in ks]
        rows.append((lo, wts))
    T = max(len(w) for _, w in rows)
    idx = np.zeros((Nout, T), dtype=np.int64)
    wt = np.zeros((Nout, T), dtype=np.float64)
    for o, (lo, wts) in enumerate(rows):
        m = len(wts)
        idx[o, :m] = np.arange(lo, lo + m)
        idx[o, m:] = lo if m else 0
        wt[o, :m] = wts
    return idx, wt


def _pass(data, idx, wt, axis):
    """acc = 0.0; acc = acc + (wt_i * p(i)) ascending i. Padded taps have weight 0.0 and are last
    (adding +-0.0 to a finite sum does not change it)."""
    Nout, T = idx.shape
    if axis == 1:
        shape = (data.shape[0], Nout) + data.shape[2:]
    else:
        shape = (Nout,) + data.shape[1:]
    acc = np.zeros(shape, dtype=np.float64)
    extra = data.ndim - 2
    for t in range(T):
        if axis == 1:
            w = wt[:, t].reshape((1, Nout) + (1,) * extra)
            acc = acc + (w * data[:, idx[:, t]])
        else:
            w = wt[:, t].reshape((Nout, 1) + (1,) * extra)
            acc = acc + (w * data[idx[:, t]])
    return acc


def _image_size_bicubic(doc, W2, H2):
    W, H = doc.w, doc.h
    cw = pillow_coeffs(W, W2) if W2 != W else None
    ch = pillow_coeffs(H, H2) if H2 != H else None

    def resample(data):
        if cw is not None:
            data = _pass(data, cw[0], cw[1], 1)
        if ch is not None:
            data = _pass(data, ch[0], ch[1], 0)
        return data

    for node in doc.iter_nodes():
        if node.kind == "raster":
            node.rgba = finish(resample(premultiply(node.rgba)))
        if node.mask is not None:
            acc = resample(dec(node.mask.data))
            node.mask.data = q(np.minimum(np.maximum(acc, 0.0), 1.0))


def _image_size_nearest(doc, W2, H2):
    W, H = doc.w, doc.h
    rx = W / W2
    ry = H / H2
    xs = np.minimum(np.floor((np.arange(W2, dtype=np.float64) + 0.5) * rx).astype(np.int64), W - 1)
    ys = np.minimum(np.floor((np.arange(H2, dtype=np.float64) + 0.5) * ry).astype(np.int64), H - 1)
    for node in doc.iter_nodes():
        if node.kind == "raster":
            node.rgba = canonicalize(node.rgba[ys[:, None], xs[None, :]].copy())
        if node.mask is not None:
            node.mask.data = node.mask.data[ys[:, None], xs[None, :]].copy()


# =============================================================================================
# canvas index mappings (§14, §15 exact paths, §16)
# =============================================================================================

def _map_all(doc, W2, H2, fn_rgba, fn_mask):
    for node in doc.iter_nodes():
        if node.kind == "raster":
            node.rgba = canonicalize(fn_rgba(node.rgba))
        if node.mask is not None:
            node.mask.data = fn_mask(node.mask.data)
    doc.w, doc.h = W2, H2
    _clear_selection(doc)


def _crop_array(a, cx, cy, cw, ch, fill):
    H, W = a.shape[:2]
    out = np.full((ch, cw) + a.shape[2:], fill, dtype=np.uint8)
    xs0 = max(cx, 0)
    ys0 = max(cy, 0)
    xs1 = min(cx + cw, W)
    ys1 = min(cy + ch, H)
    if xs0 < xs1 and ys0 < ys1:
        out[ys0 - cy:ys1 - cy, xs0 - cx:xs1 - cx] = a[ys0:ys1, xs0:xs1]
    return out


def do_crop(doc, cx, cy, cw, ch):
    _map_all(doc, cw, ch,
             lambda a: _crop_array(a, cx, cy, cw, ch, 0),
             lambda m: _crop_array(m, cx, cy, cw, ch, 255))


def _rot90(a):      # new size H x W ; new(x, y) = old(y, H - 1 - x)
    H, W = a.shape[:2]
    xs = np.arange(H)[None, :]      # new width = H
    ys = np.arange(W)[:, None]      # new height = W
    return a[H - 1 - xs, ys].copy()


def _rot180(a):     # new(x, y) = old(W - 1 - x, H - 1 - y)
    return a[::-1, ::-1].copy()


def _rot270(a):     # new size H x W ; new(x, y) = old(W - 1 - y, x)
    H, W = a.shape[:2]
    xs = np.arange(H)[None, :]
    ys = np.arange(W)[:, None]
    return a[xs, W - 1 - ys].copy()


def _flip_h(a):
    return a[:, ::-1].copy()


def _flip_v(a):
    return a[::-1, :].copy()


# =============================================================================================
# ops: selections
# =============================================================================================

@op("select_rect")
def select_rect(doc, f):
    x = f.num("x")
    y = f.num("y")
    w = f.num("w", lo=0.0, hi=65536.0, lo_open=True)
    h = f.num("h", lo=0.0, hi=65536.0, lo_open=True)
    aa = f.bool("antialias", False)
    mode = _mode(f)
    _set_candidate(doc, raster_rect(doc.w, doc.h, x, y, w, h, aa), mode)


@op("select_ellipse")
def select_ellipse(doc, f):
    x = f.num("x")
    y = f.num("y")
    w = f.num("w", lo=0.0, hi=65536.0, lo_open=True)
    h = f.num("h", lo=0.0, hi=65536.0, lo_open=True)
    aa = f.bool("antialias", True)
    mode = _mode(f)
    _set_candidate(doc, raster_ellipse(doc.w, doc.h, x, y, w, h, aa), mode)


@op("select_polygon")
def select_polygon(doc, f):
    pts = _points(f, "points", 3, 4096)
    aa = f.bool("antialias", True)
    mode = _mode(f)
    _set_candidate(doc, raster_polygon(doc.w, doc.h, pts, aa), mode)


@op("select_wand")
def select_wand(doc, f):
    layer = doc.get_raster(f.str("layer"))
    x = f.int("x")
    y = f.int("y")
    t = f.int("tolerance", 32, 0, 255)
    contiguous = f.bool("contiguous", True)
    aa = f.bool("antialias", True)
    mode = _mode(f)
    _set_candidate(doc, region(layer.rgba, x, y, t, contiguous, aa), mode)


@op("select_all")
def select_all(doc, f):
    doc.sel = np.full((doc.h, doc.w), 255, dtype=np.uint8)


@op("deselect")
def deselect(doc, f):
    if not doc.selection_is_empty():
        doc.sel_saved = doc.sel.copy()
    doc.sel = np.zeros((doc.h, doc.w), dtype=np.uint8)


@op("reselect")
def reselect(doc, f):
    if doc.sel_saved is not None:
        doc.sel = doc.sel_saved.copy()


@op("select_inverse")
def select_inverse(doc, f):
    doc.sel = (255 - doc.sel.astype(np.int32)).astype(np.uint8)


def box_widths(sigma):
    """§7 three-box widths (Kovesi / Kutskir), every step fixed."""
    s12 = (12.0 * sigma) * sigma
    wIdeal = math.sqrt((s12 / 3.0) + 1.0)
    wl = int(math.floor(wIdeal))
    if wl % 2 == 0:
        wl = wl - 1
    wu = wl + 2
    mIdeal = (((s12 - float(3 * wl * wl)) - float(12 * wl)) - 9.0) / float((-4 * wl) - 4)
    m = int(math.floor(mIdeal + 0.5))
    m = min(max(m, 0), 3)
    return [wl if i < m else wu for i in range(3)]


def _box_sum(T, h, axis):
    """sum over k = -h..h of T(clamp(i + k)) along `axis` (edge replicate), exact int64.

    Prefix sums may wrap past 2^63 on huge canvases; int64 arithmetic is exact modulo 2^64 and
    every window sum itself fits (< 1.3e17, §7), so the differences are still exact."""
    if h == 0:
        return T
    pad = [(0, 0), (0, 0)]
    pad[axis] = (h, h)
    P = np.pad(T, pad, mode="edge")
    c = np.cumsum(P, axis=axis, dtype=np.int64)
    zshape = list(c.shape)
    zshape[axis] = 1
    c = np.concatenate([np.zeros(zshape, dtype=np.int64), c], axis=axis)
    n = T.shape[axis]
    w = 2 * h + 1
    if axis == 1:
        return c[:, w:w + n] - c[:, 0:n]
    return c[w:w + n, :] - c[0:n, :]


def feather_bytes(S, r):
    """§7 on a byte array (caller handles r == 0 and the empty selection)."""
    sigma = r / 2.0
    ws = box_widths(sigma)
    T = S.astype(np.int64)
    for w in ws:
        h = (w - 1) // 2
        T = _box_sum(T, h, 1)
        T = _box_sum(T, h, 0)
    p = ws[0] * ws[1] * ws[2]
    D = p * p
    return ((2 * T + D) // (2 * D)).astype(np.uint8)


@op("feather")
def feather(doc, f):
    r = f.num("radius", lo=0.0, hi=250.0)
    if r == 0.0 or doc.selection_is_empty():
        return
    doc.sel = feather_bytes(doc.sel, r)


def morph(S, N, dilate):
    """§8 grey-scale dilation (max, outside 0) / erosion (min, outside 255) by disc(N)."""
    H, W = S.shape
    fill = 0 if dilate else 255
    red = np.maximum if dilate else np.minimum
    P = np.full((H + 2 * N, W + 2 * N), fill, dtype=np.uint8)
    P[N:N + H, N:N + W] = S
    # hm[h][j, i] = reduce over dx in -h..h of P[j, i + dx], for canvas columns i (padded coords)
    hm = {}
    cur = P[:, N:N + W].copy()
    hm[0] = cur
    for h in range(1, N + 1):
        cur = red(cur, red(P[:, N - h:N - h + W], P[:, N + h:N + h + W]))
        hm[h] = cur
    out = None
    for dy in range(-N, N + 1):
        half = math.isqrt(N * N - dy * dy)
        rows = hm[half][N + dy:N + dy + H]
        out = rows.copy() if out is None else red(out, rows)
    return out


@op("expand")
def expand(doc, f):
    N = f.int("by", lo=1, hi=100)
    if doc.selection_is_empty():
        return
    doc.sel = morph(doc.sel, N, True)


@op("contract")
def contract(doc, f):
    N = f.int("by", lo=1, hi=100)
    if doc.selection_is_empty():
        return
    doc.sel = morph(doc.sel, N, False)


# =============================================================================================
# ops: painting (§10, §17, §18)
# =============================================================================================

@op("fill_selection")
def fill_selection(doc, f):
    layer = doc.get_raster(f.str("layer"))
    col = f.color("color")
    opacity = f.unit_num("opacity", 1.0)
    sr, sg, sb, ca = _colour_doubles(col)
    as_ = (ca * opacity) * effective_e(doc)
    paint_over(layer, sr, sg, sb, as_)


@op("gradient")
def gradient(doc, f):
    layer = doc.get_raster(f.str("layer"))
    gtype = f.str("type", "linear", choices=("linear", "radial"))
    x0, y0 = _num_list(f, "p0", 2)
    x1, y1 = _num_list(f, "p1", 2)
    c0 = _colour_doubles(f.color("c0"))
    c1 = _colour_doubles(f.color("c1"))
    opacity = f.unit_num("opacity", 1.0)
    reverse = f.bool("reverse", False)
    dx = x1 - x0
    dy = y1 - y0
    L2 = (dx * dx) + (dy * dy)
    if L2 == 0.0:
        return
    H, W = doc.h, doc.w
    ex = (np.arange(W, dtype=np.float64)[None, :] + 0.5) - x0
    ey = (np.arange(H, dtype=np.float64)[:, None] + 0.5) - y0
    ex, ey = np.broadcast_arrays(ex, ey)
    if gtype == "linear":
        t = ((ex * dx) + (ey * dy)) / L2
    else:
        radius = math.sqrt(L2)
        t = np.sqrt((ex * ex) + (ey * ey)) / radius
    t = np.minimum(np.maximum(t, 0.0), 1.0)
    if reverse:
        t = 1.0 - t
    g = [c0[k] + ((c1[k] - c0[k]) * t) for k in range(4)]
    as_ = (g[3] * opacity) * effective_e(doc)
    paint_over(layer, g[0], g[1], g[2], as_)


@op("bucket_fill")
def bucket_fill(doc, f):
    layer = doc.get_raster(f.str("layer"))
    x = f.int("x")
    y = f.int("y")
    col = f.color("color")
    opacity = f.unit_num("opacity", 1.0)
    t = f.int("tolerance", 32, 0, 255)
    contiguous = f.bool("contiguous", True)
    aa = f.bool("antialias", True)
    Rg = region(layer.rgba, x, y, t, contiguous, aa)     # before any write
    cr, cg, cb, ca = _colour_doubles(col)
    as_ = ((ca * opacity) * dec(Rg)) * effective_e(doc)
    paint_over(layer, cr, cg, cb, as_)


# =============================================================================================
# ops: transform (§12)
# =============================================================================================

MATRIX_FIELDS = ("matrix",)
PARAM_FIELDS = ("translate", "scale", "rotate", "skew", "pivot")
QUAD_FIELDS = ("quad", "rect")


def transform_matrix(doc, f):
    """Parse the one form (§12.2) and return the forward matrix M."""
    forms = [name for name, fields in (("matrix", MATRIX_FIELDS), ("params", PARAM_FIELDS),
                                       ("quad", QUAD_FIELDS))
             if any(f.has(k) for k in fields)]
    if len(forms) > 1:
        raise f.err("fields of more than one transform form (%s)" % ", ".join(forms))
    form = forms[0] if forms else "params"
    if form == "matrix":
        return _num_list(f, "matrix", 9)
    if form == "quad":
        v = f.raw("quad")
        if not isinstance(v, list) or len(v) != 4:
            raise f.err("'quad' must be 4 [x, y] points")
        quad = []
        for p in v:
            if not isinstance(p, list) or len(p) != 2:
                raise f.err("'quad' must be 4 [x, y] points")
            quad.append((_finite(f, "quad", p[0]), _finite(f, "quad", p[1])))
        rect = _num_list(f, "rect", 4, [0.0, 0.0, float(doc.w), float(doc.h)])
        if not (rect[2] > 0.0 and rect[3] > 0.0):
            raise f.err("'rect' w and h must be > 0")
        return quad_matrix(rect, quad)
    tx, ty = _num_list(f, "translate", 2, [0.0, 0.0])
    sx, sy = _num_list(f, "scale", 2, [1.0, 1.0])
    for s in (sx, sy):
        if s == 0.0 or abs(s) > 1000.0:
            raise f.err("'scale' components must be nonzero with abs <= 1000")
    deg = f.num("rotate", 0.0, -3600.0, 3600.0)
    kx, ky = _num_list(f, "skew", 2, [0.0, 0.0])
    for k in (kx, ky):
        if not (-89.0 <= k <= 89.0):
            raise f.err("'skew' components must be in [-89, 89]")
    px, py = _num_list(f, "pivot", 2, [doc.w / 2.0, doc.h / 2.0])
    return params_matrix(tx, ty, sx, sy, deg, kx, ky, px, py)


@op("transform")
def transform(doc, f):
    layer = doc.get_raster(f.str("layer"))
    interp = f.str("interp", "bicubic", choices=("bicubic", "nearest"))
    M = transform_matrix(doc, f)
    n = invert(M)
    layer.rgba = resample_rgba(layer.rgba, n, doc.w, doc.h, interp)


# =============================================================================================
# ops: Image menu (§13-§16)
# =============================================================================================

@op("image_size")
def image_size(doc, f):
    W2 = f.int("w", lo=1, hi=16384)
    H2 = f.int("h", lo=1, hi=16384)
    interp = f.str("interp", "bicubic", choices=("bicubic", "nearest"))
    if interp == "nearest":
        _image_size_nearest(doc, W2, H2)
    elif W2 != doc.w or H2 != doc.h:
        _image_size_bicubic(doc, W2, H2)
    doc.w, doc.h = W2, H2
    _clear_selection(doc)


@op("canvas_size")
def canvas_size(doc, f):
    W2 = f.int("w", lo=1, hi=16384)
    H2 = f.int("h", lo=1, hi=16384)
    anchor = f.str("anchor", "c", choices=ANCHORS)
    W, H = doc.w, doc.h
    if anchor in ("tl", "l", "bl"):
        ox = 0
    elif anchor in ("t", "c", "b"):
        ox = (W2 - W) // 2              # Python // is floor division
    else:
        ox = W2 - W
    if anchor in ("tl", "t", "tr"):
        oy = 0
    elif anchor in ("l", "c", "r"):
        oy = (H2 - H) // 2
    else:
        oy = H2 - H
    do_crop(doc, -ox, -oy, W2, H2)


@op("crop")
def crop(doc, f):
    cx = f.int("x")
    cy = f.int("y")
    cw = f.int("w", lo=1, hi=16384)
    ch = f.int("h", lo=1, hi=16384)
    do_crop(doc, cx, cy, cw, ch)


@op("rotate_canvas")
def rotate_canvas(doc, f):
    angle = f.num("angle", lo=-3600.0, hi=3600.0)
    a = angle - (360.0 * math.floor(angle / 360.0))
    W, H = doc.w, doc.h
    if a == 0.0:
        _clear_selection(doc)
        return
    if a == 90.0:
        _map_all(doc, H, W, _rot90, _rot90)
        return
    if a == 180.0:
        _map_all(doc, W, H, _rot180, _rot180)
        return
    if a == 270.0:
        _map_all(doc, H, W, _rot270, _rot270)
        return
    rad = a * DEG
    c = math.cos(rad)
    s = math.sin(rad)
    bw = abs(W * c) + abs(H * s)
    bh = abs(W * s) + abs(H * c)
    W2 = max(math.ceil(bw - 1e-6), 1)
    H2 = max(math.ceil(bh - 1e-6), 1)
    if W2 > 16384 or H2 > 16384:
        raise f.err("rotated canvas %dx%d is larger than 16384 px per axis" % (W2, H2))
    M = mat_mul(Ro(a), Tr(-(W / 2.0), -(H / 2.0)))
    M = mat_mul(Tr(W2 / 2.0, H2 / 2.0), M)
    n = invert(M)
    _map_all(doc, W2, H2,
             lambda arr: resample_rgba(arr, n, W2, H2, "bicubic"),
             lambda m: resample_mask(m, n, W2, H2))


@op("flip")
def flip(doc, f):
    axis = f.str("axis", choices=("h", "v"))
    fn = _flip_h if axis == "h" else _flip_v
    if f.has("layer"):
        layer = doc.get_raster(f.str("layer"))
        layer.rgba = fn(layer.rgba)
        return
    _map_all(doc, doc.w, doc.h, fn, fn)
