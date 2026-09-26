"""Brush engine, eraser and clone stamp (docs/math/40-brush.md), written from docs/math/ only.

Ops: `brush_stroke`, `eraser_stroke`, `clone_stroke` (doc 40 §2). `undo` is the runner's (C10).

Pipeline per stroke op (doc 40 §3, in this order):
  begin  : validate, snapshot S0 (and SRC for clone), bake pressure LUTs, rotation scalars,
           zeroed stroke buffer B, walk state (frac = 0, step unset, dab_count = 0), clone offset.
  sample : one `add_sample` per JSON sample: effective time, EMA stabilizer, spacing walk (§3.4),
           each dab: per-dab parameters (§3.5), dab mask (§3.6), accumulation into B (§3.7).
  end    : one composite of the final B onto S0 (§3.8), written as the op's single history record.

C1: per-pixel arithmetic is NumPy float64 with + - * / sqrt and comparisons only, in the doc's
evaluation order; cos/sin come from Python `math` once per stroke; the walk runs on Python floats
(binary64). No mutation hooks of §10 are implemented: this is always the correct version.
"""

import math
import weakref

import numpy as np

from .core import ScriptError, dec, q
from .fields import is_number
from .registry import op

PI = 3.141592653589793
MAX_DABS = 1000000
COORD_LIM = 1e6

# Clone-stamp tool state (doc 40 §4): `clone_src`, `clone_off`. It is tool state, NOT document
# state, so undo must not change it; doc.ext is snapshotted by the runner's history, so it cannot
# live there. The runner has no per-script tool-state hook; the Document object keeps its identity
# across `undo` (Document.restore swaps __dict__), so the state is keyed weakly on that object and
# lives exactly as long as one script run.
_TOOL_STATE = weakref.WeakKeyDictionary()


def _clone_state(doc):
    st = _TOOL_STATE.get(doc)
    if st is None:
        st = {"clone_src": None, "clone_off": None}
        _TOOL_STATE[doc] = st
    return st


# ---------------------------------------------------------------------------------------------
# scalar helpers
# ---------------------------------------------------------------------------------------------

def rhu(x):
    """Doc 40 notation: round half up, fl = floor(x); fl + ((x - fl) >= 0.5). Python int."""
    fl = math.floor(x)
    return fl + (1 if (x - fl) >= 0.5 else 0)


def bake_lut(points):
    """§3.3: 256-entry uint16-valued table (Python ints 0..65535) for a validated curve."""
    n = len(points)
    lut = []
    for i in range(256):
        x = i / 255.0
        k = 0
        for j in range(n - 1):          # largest index in [0, n-2] with X_j <= x
            if points[j][0] <= x:
                k = j
        xk, yk = points[k]
        xk1, yk1 = points[k + 1]
        t = (x - xk) / (xk1 - xk)
        y = yk + (t * (yk1 - yk))
        lut.append(rhu(min(max(y, 0.0), 1.0) * 65535.0))
    return lut


def lut_factor(lut, p):
    """§3.3 lookup: i = q(p); factor = LUT[i] / 65535.0; a null curve is exactly 1.0."""
    if lut is None:
        return 1.0
    return lut[q(p)] / 65535.0


# ---------------------------------------------------------------------------------------------
# field parsing (§2)
# ---------------------------------------------------------------------------------------------

def _parse_curve(f, name):
    v = f.raw(name, None)
    if v is None:
        return None
    if not isinstance(v, list) or not (2 <= len(v) <= 16):
        raise f.err("%r must be null or an array of 2..16 [x, y] points" % name)
    pts = []
    for i, pt in enumerate(v):
        if not isinstance(pt, list) or len(pt) != 2 or not all(is_number(e) for e in pt):
            raise f.err("%r[%d] must be [x, y] numbers" % (name, i))
        x, y = float(pt[0]), float(pt[1])
        for c in (x, y):
            if c != c or not (0.0 <= c <= 1.0):
                raise f.err("%r[%d]: coordinates must be in [0, 1]" % (name, i))
        if pts and not (x > pts[-1][0]):
            raise f.err("%r: x must be strictly increasing" % name)
        pts.append((x, y))
    if pts[0][0] != 0.0:
        raise f.err("%r: first x must be exactly 0.0" % name)
    if pts[-1][0] != 1.0:
        raise f.err("%r: last x must be exactly 1.0" % name)
    return pts


def _parse_samples(f):
    readers = f.obj_list("samples")
    if len(readers) < 1:
        raise f.err("'samples' must contain at least 1 sample")
    out = []
    prev_t = 0.0
    for s in readers:
        x = s.num("x", lo=-COORD_LIM, hi=COORD_LIM)
        y = s.num("y", lo=-COORD_LIM, hi=COORD_LIM)
        p = s.num("pressure", 1.0)
        p = min(max(p, 0.0), 1.0)                      # the one clamp C9 allows
        s.num("tilt_x", 0.0, -90.0, 90.0)              # recorded, unused by v0.1 math
        s.num("tilt_y", 0.0, -90.0, 90.0)
        t = s.num("t_ms", prev_t, 0.0, None)
        prev_t = t
        out.append((x, y, p, t))
    return out


def _parse_common(f):
    """Fields shared by all three stroke ops (every one except layer/target/color/clone fields)."""
    P = {}
    P["size"] = f.num("size", 20.0, 1.0, 5000.0)
    P["hardness"] = f.num("hardness", 1.0, 0.0, 1.0)
    P["spacing"] = f.num("spacing", 0.25, 0.01, 10.0)
    P["opacity"] = f.num("opacity", 1.0, 0.0, 1.0)
    P["flow"] = f.num("flow", 1.0, 0.0, 1.0)
    P["angle"] = f.num("angle", 0.0, -360.0, 360.0)
    P["roundness"] = f.num("roundness", 1.0, 0.01, 1.0)
    P["mode"] = f.str("mode", "wash", choices=("wash", "buildup"))
    P["dabs_per_second"] = f.num("dabs_per_second", 0.0, 0.0, 1000.0)
    P["smoothing"] = f.num("smoothing", 0.0, 0.0, 0.99)
    P["size_curve"] = _parse_curve(f, "size_curve")
    P["opacity_curve"] = _parse_curve(f, "opacity_curve")
    f.num("view_zoom", 1.0, 0.0, 256.0, lo_open=True)  # informational only: enters no formula
    P["samples"] = _parse_samples(f)
    return P


# ---------------------------------------------------------------------------------------------
# the stroke engine (§3.1 - §3.7)
# ---------------------------------------------------------------------------------------------

class Stroke:
    """begin_stroke / add_sample / end-state holder. `B` is the canvas-sized float64 stroke buffer.

    `dab_log`, when a list, receives (x, y, p, d) for every placed dab (self-checks only)."""

    def __init__(self, P, w, h, dab_log=None):
        self.w, self.h = w, h
        self.size = P["size"]
        self.hardness = P["hardness"]
        self.spacing = P["spacing"]
        self.opacity = P["opacity"]
        self.flow = P["flow"]
        self.roundness = P["roundness"]
        self.buildup = P["mode"] == "buildup"
        self.r_air = P["dabs_per_second"]
        # §3.3 LUTs, baked once per stroke
        self.lut_s = None if P["size_curve"] is None else bake_lut(P["size_curve"])
        self.lut_o = None if P["opacity_curve"] is None else bake_lut(P["opacity_curve"])
        # §3.1 step 4: rotation scalars (libm scalars, once per stroke)
        theta = P["angle"] * (PI / 180.0)
        self.ca = math.cos(theta)
        self.sa = math.sin(theta)
        # §3.1 step 5: stroke buffer
        self.B = np.zeros((h, w), dtype=np.float64)
        # §3.1 step 6: walk state
        self.frac = 0.0
        self.step = None
        self.dab_count = 0
        # §3.2 stabilizer constants
        self.m = P["smoothing"]
        self.a = 1.0 - self.m
        self.k = 0
        self.prev = None          # (xs, ys, p, te) of the previous sample
        self.dab_log = dab_log

    # -- §3.2 + §3.4 ------------------------------------------------------------------------
    def add_sample(self, xr, yr, p, traw):
        if self.k == 0:
            te = traw
            xs, ys = xr, yr
            d = self._place_dab(xs, ys, p)
            self.step = self._next_step(d)
            self.frac = 0.0
        else:
            xs0, ys0, p0, te0 = self.prev
            te = max(te0, traw)
            xs = (self.a * xr) + (self.m * xs0)
            ys = (self.a * yr) + (self.m * ys0)
            dx = xs - xs0
            dy = ys - ys0
            L = math.sqrt((dx * dx) + (dy * dy))       # correctly rounded, like np.sqrt
            D = (te - te0) / 1000.0
            u = 0.0
            while True:
                w = 1.0 - u
                todo = ((L * w) / self.step) + ((D * w) * self.r_air)
                if (self.frac + todo) < 1.0:
                    break
                need = 1.0 - self.frac
                u = u + (w * (need / todo))
                if u > 1.0:
                    u = 1.0
                x = xs0 + (u * dx)
                y = ys0 + (u * dy)
                pp = p0 + (u * (p - p0))
                d = self._place_dab(x, y, pp)
                self.frac = 0.0
                self.step = self._next_step(d)
            self.frac = self.frac + todo
        self.prev = (xs, ys, p, te)
        self.k += 1

    def _next_step(self, d):
        return self.spacing * max(d, 1.0)

    # -- §3.5 - §3.7 ------------------------------------------------------------------------
    def _place_dab(self, cx, cy, p):
        """Place one dab; returns its unclamped diameter d (which feeds next_step)."""
        if self.dab_count + 1 > MAX_DABS:
            raise ScriptError("stroke places more than %d dabs" % MAX_DABS)
        self.dab_count += 1
        fs = lut_factor(self.lut_s, p)
        fo = lut_factor(self.lut_o, p)
        d = self.size * fs
        if self.dab_log is not None:
            self.dab_log.append((cx, cy, p, d))
        if d <= 0.0:
            return d                                   # counted, touches no pixels
        d_draw = max(d, 1.0)
        k_small = min(d, 1.0)
        O = (self.opacity * fo) * k_small
        R = d_draw / 2.0
        Rm = R * self.roundness
        w = (1.0 / Rm) if Rm > 1.0 else 1.0
        he = min(self.hardness, 1.0 - w)
        f = self.flow

        # §3.6 touched pixels: centres in the closed square, intersected with the canvas
        x_lo = max(math.ceil((cx - R) - 0.5), 0)
        x_hi = min(math.floor((cx + R) - 0.5), self.w - 1)
        y_lo = max(math.ceil((cy - R) - 0.5), 0)
        y_hi = min(math.floor((cy + R) - 0.5), self.h - 1)
        if x_lo > x_hi or y_lo > y_hi:
            return d
        pxc = np.arange(x_lo, x_hi + 1, dtype=np.float64) + 0.5     # (px + 0.5), exact
        pyc = np.arange(y_lo, y_hi + 1, dtype=np.float64) + 0.5
        ddx = (pxc - cx)[None, :]
        ddy = (pyc - cy)[:, None]
        uu = (ddx * self.ca) - (ddy * self.sa)
        vv = (ddx * self.sa) + (ddy * self.ca)
        nu = uu / R
        nv = vv / Rm
        r = np.sqrt((nu * nu) + (nv * nv))
        t = (r - he) / (1.0 - he)                      # 1 - he >= w > 0: never divides by zero
        band = 1.0 - ((t * t) * (3.0 - (2.0 * t)))
        m = np.where(r >= 1.0, 0.0, np.where(r <= he, 1.0, band))

        # §3.7 accumulation, revised 2026-09-26: the mask scales the rate, the ceiling is O
        # (Wash) or 1.0 (Build-up). Pixels are independent within a dab.
        Bs = self.B[y_lo:y_hi + 1, x_lo:x_hi + 1]
        if self.buildup:
            kb = f * O                                 # once per dab
            Bs[...] = Bs + ((kb * m) * (1.0 - Bs))
        else:
            Bs[...] = np.where(O > Bs, Bs + ((f * m) * (O - Bs)), Bs)
        return d


def run_stroke(P, w, h, dab_log=None):
    """Drive the engine through the same begin/add_sample sequence as the core API."""
    st = Stroke(P, w, h, dab_log)
    for (x, y, p, t) in P["samples"]:
        st.add_sample(x, y, p, t)
    return st


# ---------------------------------------------------------------------------------------------
# §3.8 composites (vectorised over the pixels with a_s > 0; everything else is a byte copy)
# ---------------------------------------------------------------------------------------------

def _paint_rgb(S0, a_s, Cs, lock):
    """Brush / clone pixel formula. S0 uint8 (h, w, 4); a_s float64 (h, w) (0 = byte copy);
    Cs: float64 (h, w, 3) or a 3-tuple of floats. Returns a new canonical uint8 array."""
    out = S0.copy()
    A = S0[..., 3]
    sel = a_s > 0.0
    if lock:
        sel = sel & (A != 0)
    if not sel.any():
        return out
    ys, xs = np.nonzero(sel)
    a = a_s[ys, xs]
    ad = dec(A[ys, xs])
    if isinstance(Cs, np.ndarray):
        cs = [Cs[ys, xs, c] for c in range(3)]
    else:
        cs = [np.full(a.shape, Cs[c], dtype=np.float64) for c in range(3)]
    if lock:
        for c in range(3):
            Cd = dec(S0[ys, xs, c])
            Co = (Cd * (1.0 - a)) + (cs[c] * a)
            out[ys, xs, c] = q(Co)
        # alpha byte copied unchanged (non-zero here, so already canonical)
    else:
        a_o = a + (ad * (1.0 - a))
        for c in range(3):
            Cd = dec(S0[ys, xs, c])
            Co = ((cs[c] * a) + ((Cd * ad) * (1.0 - a))) / a_o
            out[ys, xs, c] = q(Co)
        qa = q(a_o)
        out[ys, xs, 3] = qa
        z = qa == 0
        if z.any():
            out[ys[z], xs[z]] = 0                      # C3 canonical transparent
    return out


def _erase(S0, a_s):
    out = S0.copy()
    sel = a_s > 0.0
    if not sel.any():
        return out
    ys, xs = np.nonzero(sel)
    a = a_s[ys, xs]
    ad = dec(S0[ys, xs, 3])
    a_o = ad * (1.0 - a)
    qa = q(a_o)
    out[ys, xs, 3] = qa
    z = qa == 0
    if z.any():
        out[ys[z], xs[z]] = 0
    return out


def _paint_mask(M0, a_s, rgb):
    out = M0.copy()
    sel = a_s > 0.0
    if not sel.any():
        return out
    Cr, Cg, Cb = (v / 255.0 for v in rgb)
    g = ((0.30 * Cr) + (0.59 * Cg)) + (0.11 * Cb)
    ys, xs = np.nonzero(sel)
    Mv = dec(M0[ys, xs])
    out[ys, xs] = q(Mv + (a_s[ys, xs] * (g - Mv)))
    return out


def _shifted(SRC, oxi, oyi):
    """SRC(px + oxi, py + oyi) for every destination pixel; out-of-canvas reads (0,0,0,0) (C4)."""
    h, w = SRC.shape[:2]
    out = np.zeros_like(SRC)
    dx_lo, dx_hi = max(0, -oxi), min(w, w - oxi)
    dy_lo, dy_hi = max(0, -oyi), min(h, h - oyi)
    if dx_lo < dx_hi and dy_lo < dy_hi:
        out[dy_lo:dy_hi, dx_lo:dx_hi] = SRC[dy_lo + oyi:dy_hi + oyi, dx_lo + oxi:dx_hi + oxi]
    return out


def _selection_s(doc):
    """s = sel / 255.0 per pixel; exactly 1.0 everywhere with no selection (doc 30 §2, C8a)."""
    return dec(doc.selection_effective())


# ---------------------------------------------------------------------------------------------
# the ops (§2.2 - §2.4)
# ---------------------------------------------------------------------------------------------

@op("brush_stroke")
def brush_stroke(doc, f):
    layer = doc.get_raster(f.str("layer"))
    target = f.str("target", "pixels", choices=("pixels", "mask"))
    col = f.color("color", "#000000")
    if col[3] != 255:
        raise f.err("'color' alpha must be FF")
    P = _parse_common(f)
    if target == "mask" and layer.mask is None:
        raise f.err("target 'mask' but layer %r has no mask" % layer.id)

    if target == "mask":
        S0 = layer.mask.data.copy()
    else:
        S0 = layer.rgba.copy()
    st = run_stroke(P, doc.w, doc.h)
    a_s = st.B * _selection_s(doc)
    if target == "mask":
        layer.mask.data = _paint_mask(S0, a_s, col[:3])
    else:
        Cs = tuple(v / 255.0 for v in col[:3])
        layer.rgba = _paint_rgb(S0, a_s, Cs, layer.lock_alpha)


@op("eraser_stroke")
def eraser_stroke(doc, f):
    layer = doc.get_raster(f.str("layer"))
    P = _parse_common(f)
    S0 = layer.rgba.copy()
    st = run_stroke(P, doc.w, doc.h)                   # the walk runs (and may error) regardless
    if layer.lock_alpha:
        layer.rgba = S0                                # byte copy: no-op on a locked layer
        return
    a_s = st.B * _selection_s(doc)
    layer.rgba = _erase(S0, a_s)


def _parse_point(f, name):
    v = f.raw(name)
    if not isinstance(v, list) or len(v) != 2 or not all(is_number(e) for e in v):
        raise f.err("%r must be [x, y] numbers" % name)
    pt = []
    for e in v:
        try:
            e = float(e)
        except OverflowError:
            raise f.err("%r out of range" % name)
        if e != e or not (-COORD_LIM <= e <= COORD_LIM):
            raise f.err("%r: coordinates must be finite and in [-1e6, 1e6]" % name)
        pt.append(e)
    return (pt[0], pt[1])


@op("clone_stroke")
def clone_stroke(doc, f):
    layer_id = f.str("layer")
    layer = doc.get_raster(layer_id)
    has_source = f.has("source")
    source = _parse_point(f, "source") if has_source else None
    aligned = f.bool("aligned", True)
    src_layer = doc.get_raster(f.str("source_layer", layer_id))
    P = _parse_common(f)

    # §3.1 step 2: snapshots
    S0 = layer.rgba.copy()
    SRC = S0 if src_layer is layer else src_layer.rgba.copy()

    # §4 clone source state (tool state, not undone)
    cs = _clone_state(doc)
    x0, y0 = P["samples"][0][0], P["samples"][0][1]     # first sample's RAW position
    if has_source:
        cs["clone_src"] = source
        cs["clone_off"] = None
    if cs["clone_src"] is None:
        raise f.err("no clone source set (no 'source' here or in an earlier clone_stroke)")
    if not (aligned and cs["clone_off"] is not None):
        cs["clone_off"] = (rhu(cs["clone_src"][0] - x0), rhu(cs["clone_src"][1] - y0))
    oxi, oyi = cs["clone_off"]

    st = run_stroke(P, doc.w, doc.h)
    Csrc = _shifted(SRC, oxi, oyi)
    ac = dec(Csrc[..., 3])
    a_s = (st.B * _selection_s(doc)) * ac
    Cc = dec(Csrc[..., :3])
    layer.rgba = _paint_rgb(S0, a_s, Cc, layer.lock_alpha)
