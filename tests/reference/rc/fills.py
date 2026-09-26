"""Content fills of doc 10 §11.1, shared by add_layer and add_mask."""

import numpy as np

from .core import canonicalize, dec, hash_byte_grid, q


def rect_region(rect, w, h):
    """Boolean (h, w) array: on-canvas pixels inside rect = [rx, ry, rw, rh]."""
    rx, ry, rw, rh = rect
    xs = np.arange(w)[None, :]
    ys = np.arange(h)[:, None]
    return (xs >= rx) & (xs < rx + rw) & (ys >= ry) & (ys < ry + rh)


def gradient_t(rect, w, h, direction):
    """t = 0.0 if len == 1 else (coord - start) / (len - 1), over the whole canvas (h, w).

    Only meaningful inside the rect; the double quotient of two integers."""
    rx, ry, rw, rh = rect
    if direction == "h":
        coord = np.arange(w, dtype=np.int64)[None, :] - rx
        n = rw
    else:
        coord = np.arange(h, dtype=np.int64)[:, None] - ry
        n = rh
    if n == 1:
        t = np.zeros_like(coord, dtype=np.float64)
    else:
        t = coord.astype(np.float64) / float(n - 1)
    return np.broadcast_to(t, (h, w))


def gradient_channel(t, f_byte, t_byte):
    """v = n(F) + (t * (n(T) - n(F))); byte = q(v)."""
    nf = dec(f_byte)
    nt = dec(t_byte)
    return q(nf + (t * (nt - nf)))


def layer_fill(kind, w, h, rect, **kw):
    """RGBA content for add_layer. kind in empty/solid/gradient/checker/noise. Canonical."""
    out = np.zeros((h, w, 4), dtype=np.uint8)
    if kind == "empty":
        return out
    inside = rect_region(rect, w, h)
    if kind == "solid":
        px = np.empty((h, w, 4), dtype=np.uint8)
        px[...] = np.array(kw["color"], dtype=np.uint8)
    elif kind == "gradient":
        t = gradient_t(rect, w, h, kw["dir"])
        px = np.stack([gradient_channel(t, kw["from"][k], kw["to"][k]) for k in range(4)], axis=-1)
    elif kind == "checker":
        rx, ry = rect[0], rect[1]
        cell = kw["cell"]
        # (x - rx) // cell is only used inside the rect, where it is non-negative
        cx = (np.arange(w, dtype=np.int64)[None, :] - rx) // cell
        cy = (np.arange(h, dtype=np.int64)[:, None] - ry) // cell
        even = ((cx + cy) % 2) == 0
        px = np.where(even[..., None], np.array(kw["a"], dtype=np.uint8),
                      np.array(kw["b"], dtype=np.uint8)).astype(np.uint8)
    elif kind == "noise":
        seed = kw["seed"]
        chans = [hash_byte_grid(seed, w, h, k) for k in range(3)]
        if kw["alpha"] == "random":
            chans.append(hash_byte_grid(seed, w, h, 3))
        else:
            chans.append(np.full((h, w), kw["alpha"], dtype=np.uint8))
        px = np.stack(chans, axis=-1)
    else:
        raise AssertionError(kind)
    out[inside] = px[inside]
    return canonicalize(out)


def mask_fill(kind, w, h, rect, outside, **kw):
    """uint8 (h, w) mask content for add_mask. kind in solid/gradient/noise."""
    out = np.full((h, w), outside, dtype=np.uint8)
    inside = rect_region(rect, w, h)
    if kind == "solid":
        px = np.full((h, w), kw["value"], dtype=np.uint8)
    elif kind == "gradient":
        px = gradient_channel(gradient_t(rect, w, h, kw["dir"]), kw["from"], kw["to"])
    elif kind == "noise":
        px = hash_byte_grid(kw["seed"], w, h, 4)
    else:
        raise AssertionError(kind)
    out[inside] = px[inside]
    return out
