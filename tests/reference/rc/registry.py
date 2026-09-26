"""Op and adjustment registries, with auto-discovery of rc/ops_*.py modules.

    from rc.registry import op, register_adjustment

    @op("my_op")                      # handler(doc, f) ; f is a rc.fields.FieldReader
    def my_op(doc, f): ...

    register_adjustment("levels", parse, apply)

See tests/reference/README.md for the full contract.
"""

import importlib
import pkgutil

import numpy as np

from .core import ScriptError

OPS = {}           # name -> OpSpec
ADJUSTMENTS = {}   # type name -> (parse, apply)

# doc 20 §A9: the complete set of valid adjustment type names (implemented or not).
ADJUSTMENT_TYPES = ("levels", "curves", "brightness_contrast", "hue_saturation", "black_white",
                    "invert", "posterize", "threshold")


class OpSpec:
    def __init__(self, name, handler, history):
        self.name = name
        self.handler = handler
        self.history = history


def op(name, history=True):
    """Decorator registering `handler(doc, f)` as render-script op `name`.

    history=True (the default): the runner pushes one C10 history record (a snapshot of the
    document taken before the handler runs). Only `undo` itself uses history=False."""
    def deco(fn):
        if name in OPS:
            raise RuntimeError("op %r registered twice (%s and %s)"
                               % (name, OPS[name].handler.__module__, fn.__module__))
        OPS[name] = OpSpec(name, fn, history)
        return fn
    return deco


def register_adjustment(type_name, parse, apply):
    """Register an adjustment type.

    parse(p)            -> params object. `p` is a FieldReader over the op's `params` object;
                           read every accepted key through it (unknown keys then error) and raise
                           ScriptError (or p.err(...)) for bad values.
    apply(params, r, g, b) -> (r2, g2, b2): uint8 (h, w) planes in, uint8 (h, w) planes out.
                           This is doc 20's f (bytes -> bytes); it must not read alpha. The
                           compositor handles alpha, canonicalisation and blending (doc 10 §4.2).
    """
    if type_name not in ADJUSTMENT_TYPES:
        raise RuntimeError("unknown adjustment type %r" % type_name)
    if type_name in ADJUSTMENTS:
        raise RuntimeError("adjustment %r registered twice" % type_name)
    ADJUSTMENTS[type_name] = (parse, apply)


def parse_adjustment(type_name, p):
    if type_name not in ADJUSTMENT_TYPES:
        raise ScriptError("unknown adjustment type %r" % type_name)
    if type_name not in ADJUSTMENTS:
        raise ScriptError("adjustment type %r is not implemented in the reference" % type_name)
    return ADJUSTMENTS[type_name][0](p)


def apply_adjustment(node, r, g, b):
    """ADJ(node, Rb, Gb, Bb) -> (h, w, 3) uint8 array."""
    _, fn = ADJUSTMENTS[node.adj_type]
    r2, g2, b2 = fn(node.params, r, g, b)
    out = np.stack([r2, g2, b2], axis=-1)
    if out.dtype != np.uint8 or out.shape != r.shape + (3,):
        raise AssertionError("adjustment %r returned %s %s" % (node.adj_type, out.dtype, out.shape))
    return out


_loaded = False


def load_all():
    """Import every rc/ops_*.py module (sorted by name) so each registers its ops."""
    global _loaded
    if _loaded:
        return
    pkg = importlib.import_module(__package__)
    names = sorted(m.name for m in pkgutil.iter_modules(pkg.__path__) if m.name.startswith("ops_"))
    for name in names:
        importlib.import_module("%s.%s" % (__package__, name))
    _loaded = True
