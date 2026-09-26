"""The compositor of doc 10 §4-§7, whole-canvas vectorised, one layer at a time (C5).

Buffers are uint8 (h, w, 4) straight RGBA, always canonical. Coverages are float64 (h, w).
"""

import numpy as np

from .blend import blend
from .core import assemble, clamp01, dec, pixel_hash_grid, q, unit_grid
from .registry import apply_adjustment


def _dissolve(c, seed, h, w):
    """§3.5: c' = 1.0 if unit(pixel_hash(seed, x, y, 5)) < c else 0.0."""
    u = unit_grid(pixel_hash_grid(seed, w, h, 5))
    return np.where(u < c, 1.0, 0.0)


def _full(c, h, w):
    """Broadcast a scalar coverage to an (h, w) float64 array."""
    return np.broadcast_to(np.asarray(c, dtype=np.float64), (h, w))


# ---------------------------------------------------------------------------------------------
# §4.1 COMPOSITE
# ---------------------------------------------------------------------------------------------

def COMPOSITE(D, src_rgb, c, mode, seed):
    """Composite source colour bytes `src_rgb` (h, w, 3) with coverage `c` onto backdrop D."""
    h, w = D.shape[:2]
    c = _full(c, h, w)
    # 1.
    if mode == "diss":
        c = _dissolve(c, seed, h, w)
        mode = "norm"
    # 2.
    ab = dec(D[..., 3])
    as_ = c
    # 3.
    ao = as_ + (ab * (1.0 - as_))
    zero = ao == 0.0
    # 4.
    cb = dec(D[..., :3])
    cs = dec(src_rgb)
    # 5.
    if mode == "norm":
        mixed = cs
    else:
        bl = clamp01(blend(mode, cb, cs, D[..., :3], src_rgb))
        mixed = ((1.0 - ab)[..., None] * cs) + (ab[..., None] * bl)
    t1 = as_[..., None] * mixed
    t2 = (ab[..., None] * cb) * (1.0 - as_)[..., None]
    ao_safe = np.where(zero, 1.0, ao)
    co = (t1 + t2) / ao_safe[..., None]
    # 6.
    out = assemble(q(co[..., 0]), q(co[..., 1]), q(co[..., 2]), q(ao))
    out[zero] = 0
    return out


# ---------------------------------------------------------------------------------------------
# §4.2 ADJUST
# ---------------------------------------------------------------------------------------------

def ADJUST(D, node, c, mode, seed):
    """Composite adjustment layer `node` with coverage `c` onto backdrop D (alpha unchanged)."""
    h, w = D.shape[:2]
    c = _full(c, h, w)
    Ab = D[..., 3]
    # 2.
    if mode == "diss":
        c = _dissolve(c, seed, h, w)
        mode = "norm"
    # 3.
    adj = apply_adjustment(node, D[..., 0], D[..., 1], D[..., 2])     # (h, w, 3) bytes
    cb = dec(D[..., :3])
    cs = dec(adj)
    if mode == "norm":
        bl = cs
    else:
        bl = clamp01(blend(mode, cb, cs, D[..., :3], adj))
    # 4.
    co = ((1.0 - c)[..., None] * cb) + (c[..., None] * bl)
    # 5. (and 1.: Ab == 0 -> (0,0,0,0), which assemble's canonicalisation does)
    return assemble(q(co[..., 0]), q(co[..., 1]), q(co[..., 2]), Ab)


# ---------------------------------------------------------------------------------------------
# §4.3 coverage
# ---------------------------------------------------------------------------------------------

def mask_factor(node, h, w):
    """m = n(mask byte) if the node has an enabled mask, else 1.0 (as an (h, w) array)."""
    if node.mask is not None and node.mask.enabled:
        return dec(node.mask.data)
    return np.ones((h, w), dtype=np.float64)


def coverage(node, h, w):
    """§4.3 coverage for a raster or adjustment layer."""
    m = mask_factor(node, h, w)
    if node.kind == "raster":
        return ((dec(node.rgba[..., 3]) * m) * node.fill) * node.opacity
    if node.kind == "adjustment":
        return ((1.0 * m) * node.fill) * node.opacity
    raise AssertionError("coverage() of a %s" % node.kind)


def apply_layer(D, node, c):
    """COMPOSITE for a raster layer, ADJUST for an adjustment layer, with coverage c."""
    if node.kind == "raster":
        return COMPOSITE(D, node.rgba[..., :3], c, node.mode, node.seed)
    return ADJUST(D, node, c, node.mode, node.seed)


# ---------------------------------------------------------------------------------------------
# §5 RENDER
# ---------------------------------------------------------------------------------------------

def effclip(node):
    return node.kind in ("raster", "adjustment") and node.clip


def RENDER(children, D):
    """Composite a child list (index 0 = bottom) onto byte buffer D; returns the new buffer."""
    i = 0
    n = len(children)
    while i < n:
        node = children[i]
        if effclip(node):
            if node.visible:
                D = LONE(node, D)
            i = i + 1
            continue
        j = i + 1
        run = []
        if node.kind == "raster":
            while j < n and effclip(children[j]):
                run.append(children[j])
                j = j + 1
        if not node.visible:
            i = j
            continue
        vis = [r for r in run if r.visible]
        if vis:
            D = CLIPGROUP(node, vis, D)
        else:
            D = LONE(node, D)
        i = j
    return D


def LONE(node, D):
    h, w = D.shape[:2]
    if node.kind == "group":
        return GROUP(node, D)
    return apply_layer(D, node, coverage(node, h, w))


# ---------------------------------------------------------------------------------------------
# §6 clip groups
# ---------------------------------------------------------------------------------------------

def clip_interior(base, vis, h, w):
    """§6.1 steps 1-2: the interior buffer G (alpha = relative coverage inside the base shape)."""
    af = q(base.fill)
    G = base.rgba.copy()
    G[..., 3] = af
    G = np.ascontiguousarray(G)
    # canonicalise(R0, G0, B0, q(f0)): if q(f0) == 0 every pixel is (0,0,0,0)
    if af == 0:
        G[...] = 0
    for Li in vis:
        G = apply_layer(G, Li, coverage(Li, h, w))
    return G


def clip_shape(base, h, w):
    """S = n(A0) * m0."""
    return dec(base.rgba[..., 3]) * mask_factor(base, h, w)


def CLIPGROUP(base, vis, D):
    h, w = D.shape[:2]
    S = clip_shape(base, h, w)
    o0 = base.opacity
    if base.clbl:
        G = clip_interior(base, vis, h, w)
        cg = (S * dec(G[..., 3])) * o0
        return COMPOSITE(D, G[..., :3], cg, base.mode, base.seed)
    # §6.2 clbl = false
    D = LONE(base, D)
    for Li in vis:
        ci = coverage(Li, h, w)
        ci2 = (ci * S) * o0
        D = apply_layer(D, Li, ci2)
    return D


# ---------------------------------------------------------------------------------------------
# §7 groups
# ---------------------------------------------------------------------------------------------

def pass_lerp(B0, R, wgt):
    """§7.1 step 3: premultiplied lerp of the rendered R against the pre-group backdrop B0."""
    a0 = dec(B0[..., 3])
    ar = dec(R[..., 3])
    ao = ((1.0 - wgt) * a0) + (wgt * ar)
    zero = ao == 0.0
    c0 = dec(B0[..., :3])
    cr = dec(R[..., :3])
    p = ((((1.0 - wgt) * a0)[..., None]) * c0) + (((wgt * ar)[..., None]) * cr)
    co = p / np.where(zero, 1.0, ao)[..., None]
    out = assemble(q(co[..., 0]), q(co[..., 1]), q(co[..., 2]), q(ao))
    out[zero] = 0
    return out


def GROUP(g, D):
    h, w = D.shape[:2]
    mg = mask_factor(g, h, w)
    og = g.opacity
    if g.mode == "pass":
        B0 = D
        R = RENDER(g.children, B0)
        return pass_lerp(B0, R, mg * og)
    # §7.2 isolated
    T = RENDER(g.children, np.zeros_like(D))
    c = (dec(T[..., 3]) * mg) * og
    return COMPOSITE(D, T[..., :3], c, g.mode, g.seed)


# ---------------------------------------------------------------------------------------------
# document render
# ---------------------------------------------------------------------------------------------

def bg_buffer(doc):
    D = np.empty((doc.h, doc.w, 4), dtype=np.uint8)
    D[...] = np.array(doc.bg, dtype=np.uint8)
    D[D[..., 3] == 0] = 0
    return D


def render_document(doc, onto_bg=True):
    """RENDER(root.children, BG) (onto_bg) or onto a transparent buffer (merge_visible)."""
    D = bg_buffer(doc) if onto_bg else np.zeros((doc.h, doc.w, 4), dtype=np.uint8)
    return RENDER(doc.root.children, D)
