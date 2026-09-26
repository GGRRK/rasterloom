"""Core numeric helpers shared by every reference module (docs/math/00-conventions.md).

Everything here is binary64 (numpy.float64 / Python float). Only + - * / sqrt, floor,
min/max, comparisons and where are used on arrays (C1); transcendentals, where another
module needs them, are Python `math` calls on scalars (LUTs or per-row precomputes).
"""

import numpy as np

U64 = np.uint64
MASK64 = 0xFFFFFFFFFFFFFFFF


class ScriptError(Exception):
    """A C9 script error: the renderer exits non-zero and writes no PNG."""


# ---------------------------------------------------------------------------------------------
# C2: decode / clamp / quantise
# ---------------------------------------------------------------------------------------------

def dec(v):
    """C2 decode n(v) = v / 255.0 (a division). Works on Python ints and uint8 arrays."""
    if isinstance(v, np.ndarray):
        return v.astype(np.float64) / 255.0
    return float(v) / 255.0


def clamp01(x):
    """clamp(x, 0.0, 1.0) = min(max(x, 0.0), 1.0)."""
    if isinstance(x, np.ndarray):
        return np.minimum(np.maximum(x, 0.0), 1.0)
    return min(max(x, 0.0), 1.0)


def q(x):
    """C2 quantiser: y = clamp01(x) * 255.0; round half away from zero (y >= 0).

    Arrays return uint8 arrays; Python floats return a Python int. NaN is a bug (C2) and raises.
    """
    if isinstance(x, np.ndarray):
        if x.dtype != np.float64:
            raise TypeError("q() expects float64, got %s" % x.dtype)
        if np.isnan(x).any():
            raise AssertionError("NaN reached q() (C2 violation)")
        y = np.minimum(np.maximum(x, 0.0), 1.0) * 255.0
        f = np.floor(y)
        return (f + ((y - f) >= 0.5)).astype(np.uint8)
    x = float(x)
    if x != x:
        raise AssertionError("NaN reached q() (C2 violation)")
    y = min(max(x, 0.0), 1.0) * 255.0
    f = float(np.floor(y))
    return int(f + (1.0 if (y - f) >= 0.5 else 0.0))


def canonicalize(rgba):
    """C3: every pixel whose alpha byte is 0 becomes (0,0,0,0). In place; also returns the array."""
    rgba[rgba[..., 3] == 0] = 0
    return rgba


def assemble(r, g, b, a):
    """Stack four uint8 (h, w) planes into a canonical (h, w, 4) RGBA array."""
    out = np.stack([r, g, b, a], axis=-1).astype(np.uint8)
    return canonicalize(out)


# ---------------------------------------------------------------------------------------------
# C6: deterministic randomness
# ---------------------------------------------------------------------------------------------

def splitmix64(z):
    """Scalar splitmix64 on Python ints (every step masked to 64 bits)."""
    z = (z + 0x9E3779B97F4A7C15) & MASK64
    z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & MASK64
    z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & MASK64
    return z ^ (z >> 31)


def pixel_hash(seed, x, y, k):
    """Scalar pixel_hash(seed, x, y, k) per C6."""
    inner = splitmix64(((y << 32) | x) & MASK64)
    return splitmix64((seed ^ inner ^ ((k * 0xD1B54A32D192ED03) & MASK64)) & MASK64)


def unit(h):
    """Scalar unit(h) = (h >> 11) * 2^-53, exact."""
    return float(h >> 11) * (2.0 ** -53)


def splitmix64_np(z):
    """Vectorised splitmix64 on a uint64 array (numpy uint64 arithmetic wraps modulo 2^64)."""
    z = z + U64(0x9E3779B97F4A7C15)
    z = (z ^ (z >> U64(30))) * U64(0xBF58476D1CE4E5B9)
    z = (z ^ (z >> U64(27))) * U64(0x94D049BB133111EB)
    return z ^ (z >> U64(31))


_coord_cache = {}


def _coord_hash(w, h):
    """splitmix64((y << 32) OR x) for every canvas pixel, shape (h, w), cached per size."""
    key = (w, h)
    if key not in _coord_cache:
        ys = np.arange(h, dtype=np.uint64)[:, None]
        xs = np.arange(w, dtype=np.uint64)[None, :]
        _coord_cache.clear()
        _coord_cache[key] = splitmix64_np((ys << U64(32)) | xs)
    return _coord_cache[key]


def pixel_hash_grid(seed, w, h, k):
    """pixel_hash(seed, x, y, k) for every canvas pixel (x in 0..w-1, y in 0..h-1); uint64 (h, w)."""
    kk = U64((k * 0xD1B54A32D192ED03) & MASK64)
    return splitmix64_np(U64(seed & MASK64) ^ _coord_hash(w, h) ^ kk)


def unit_grid(hashes):
    """unit(h) for a uint64 array: a float64 array in [0, 1), exact."""
    return (hashes >> U64(11)).astype(np.float64) * (2.0 ** -53)


def hash_byte_grid(seed, w, h, k):
    """The top 8 bits (h >> 56) of pixel_hash for every canvas pixel, uint8 (h, w)."""
    return (pixel_hash_grid(seed, w, h, k) >> U64(56)).astype(np.uint8)
