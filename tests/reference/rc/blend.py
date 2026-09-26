"""Blend functions B(cb, cs) of doc 10 §3, vectorised over whole planes.

Every function takes `cb`, `cs` as float64 arrays of shape (h, w, 3) (decoded bytes, R G B in
the last axis) plus the source / backdrop bytes `sb`, `bb` (uint8, same shape) for the integer
modes, and returns B as float64 (h, w, 3). The caller applies clamp01 (§4.1 step 4).

np.where evaluates both branches; divisions in the unselected branch may produce inf/NaN, which
are discarded by the selection (numpy warnings are silenced by the caller's errstate).
"""

import numpy as np

from .core import clamp01

BLEND_MODES = (
    "norm", "diss", "dark", "mul", "idiv", "lbrn", "dkCl", "lite", "scrn", "div", "lddg",
    "lgCl", "over", "sLit", "hLit", "vLit", "lLit", "pLit", "hMix", "diff", "smud", "fsub",
    "fdiv", "hue", "sat", "colr", "lum",
)
assert len(BLEND_MODES) == 27


# -- §3.2 helper forms --------------------------------------------------------------------------

def Multiply(a, b):
    return a * b


def Screen(a, b):
    return 1.0 - ((1.0 - a) * (1.0 - b))


def ColorDodge(cb, s):
    # guard order normative: cb == 0 first, then s == 1
    return np.where(cb == 0.0, 0.0,
                    np.where(s == 1.0, 1.0, np.minimum(1.0, cb / (1.0 - s))))


def ColorBurn(cb, s):
    # guard order normative: cb == 1 first, then s == 0
    return np.where(cb == 1.0, 1.0,
                    np.where(s == 0.0, 0.0, 1.0 - np.minimum(1.0, (1.0 - cb) / s)))


def HardLight(cb, s):
    return np.where(s <= 0.5, Multiply(cb, 2.0 * s), Screen(cb, (2.0 * s) - 1.0))


# -- §3.2 / §3.3 separable modes ---------------------------------------------------------------

def _soft_light(cb, cs):
    D = np.where(cb <= 0.25, ((((16.0 * cb) - 12.0) * cb) + 4.0) * cb, np.sqrt(cb))
    return np.where(cs <= 0.5,
                    cb - (((1.0 - (2.0 * cs)) * cb) * (1.0 - cb)),
                    cb + (((2.0 * cs) - 1.0) * (D - cb)))


SEPARABLE = {
    "mul":  lambda cb, cs: Multiply(cb, cs),
    "scrn": lambda cb, cs: Screen(cb, cs),
    "over": lambda cb, cs: HardLight(cs, cb),
    "dark": lambda cb, cs: np.minimum(cb, cs),
    "lite": lambda cb, cs: np.maximum(cb, cs),
    "div":  lambda cb, cs: ColorDodge(cb, cs),
    "idiv": lambda cb, cs: ColorBurn(cb, cs),
    "hLit": lambda cb, cs: HardLight(cb, cs),
    "sLit": _soft_light,
    "diff": lambda cb, cs: np.abs(cb - cs),
    "smud": lambda cb, cs: (cb + cs) - ((2.0 * cb) * cs),
    "lbrn": lambda cb, cs: np.maximum(0.0, (cb + cs) - 1.0),
    "lddg": lambda cb, cs: np.minimum(1.0, cb + cs),
    "fsub": lambda cb, cs: np.maximum(0.0, cb - cs),
    "fdiv": lambda cb, cs: np.where(cb == 0.0, 0.0,
                                    np.where(cs == 0.0, 1.0, np.minimum(1.0, cb / cs))),
    "vLit": lambda cb, cs: np.where(cs <= 0.5, ColorBurn(cb, 2.0 * cs),
                                    ColorDodge(cb, (2.0 * cs) - 1.0)),
    "lLit": lambda cb, cs: clamp01((cb + (2.0 * cs)) - 1.0),
    "pLit": lambda cb, cs: np.where(cs <= 0.5, np.minimum(cb, 2.0 * cs),
                                    np.maximum(cb, (2.0 * cs) - 1.0)),
}


# -- integer modes (§2, §3.3) ------------------------------------------------------------------

def _hard_mix(bb, sb):
    return np.where((sb.astype(np.int64) + bb.astype(np.int64)) >= 255, 1.0, 0.0)


def _int_luma(v):
    v = v.astype(np.int64)
    return ((30 * v[..., 0]) + (59 * v[..., 1])) + (11 * v[..., 2])


def _darker_color(cb, cs, bb, sb):
    take_src = (_int_luma(sb) < _int_luma(bb))[..., None]   # tie keeps the backdrop
    return np.where(take_src, cs, cb)


def _lighter_color(cb, cs, bb, sb):
    take_src = (_int_luma(sb) > _int_luma(bb))[..., None]   # tie keeps the backdrop
    return np.where(take_src, cs, cb)


# -- §3.4 non-separable ------------------------------------------------------------------------

def Lum(C):
    return ((0.3 * C[..., 0]) + (0.59 * C[..., 1])) + (0.11 * C[..., 2])


def ClipColor(C):
    L = Lum(C)[..., None]
    n = np.minimum(np.minimum(C[..., 0], C[..., 1]), C[..., 2])[..., None]
    x = np.maximum(np.maximum(C[..., 0], C[..., 1]), C[..., 2])[..., None]
    d1 = L - n
    C = np.where((n < 0.0) & (d1 > 0.0), L + (((C - L) * L) / d1), C)
    d2 = x - L
    C = np.where((x > 1.0) & (d2 > 0.0), L + (((C - L) * (1.0 - L)) / d2), C)
    return C


def SetLum(C, l):
    d = (l - Lum(C))[..., None]
    return ClipColor(C + d)


def Sat(C):
    return (np.maximum(np.maximum(C[..., 0], C[..., 1]), C[..., 2])
            - np.minimum(np.minimum(C[..., 0], C[..., 1]), C[..., 2]))


def SetSat(C, s):
    mx = np.maximum(np.maximum(C[..., 0], C[..., 1]), C[..., 2])
    mn = np.minimum(np.minimum(C[..., 0], C[..., 1]), C[..., 2])
    valid = mx > mn
    imax = np.argmax(C, axis=-1)        # first index equal to the max (r, g, b order)
    imin = np.argmin(C, axis=-1)        # first index equal to the min
    imid = 3 - imax - imin
    imid = np.where(valid, imid, 0)
    cmid = np.take_along_axis(C, imid[..., None], axis=-1)[..., 0]
    cmin = np.take_along_axis(C, imin[..., None], axis=-1)[..., 0]
    cmax = np.take_along_axis(C, imax[..., None], axis=-1)[..., 0]
    mid_val = ((cmid - cmin) * s) / (cmax - cmin)
    out = np.zeros_like(C)
    for ch in range(3):
        v = np.where(imax == ch, s, np.where(imin == ch, 0.0, mid_val))
        out[..., ch] = np.where(valid, v, 0.0)
    return out


NONSEPARABLE = {
    "hue":  lambda Cb, Cs: SetLum(SetSat(Cs, Sat(Cb)), Lum(Cb)),
    "sat":  lambda Cb, Cs: SetLum(SetSat(Cb, Sat(Cs)), Lum(Cb)),
    "colr": lambda Cb, Cs: SetLum(Cs, Lum(Cb)),
    "lum":  lambda Cb, Cs: SetLum(Cb, Lum(Cs)),
}


def blend(mode, cb, cs, bb, sb):
    """B(cb, cs) for every mode except norm/diss (the caller handles those). Not yet clamped."""
    with np.errstate(all="ignore"):
        if mode in SEPARABLE:
            return SEPARABLE[mode](cb, cs)
        if mode == "hMix":
            return _hard_mix(bb, sb)
        if mode == "dkCl":
            return _darker_color(cb, cs, bb, sb)
        if mode == "lgCl":
            return _lighter_color(cb, cs, bb, sb)
        if mode in NONSEPARABLE:
            return NONSEPARABLE[mode](cb, cs)
    raise AssertionError("blend(): unhandled mode %r" % mode)
