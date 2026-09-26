"""Render-script ops owned by doc 10 (§11.2), plus add_adjustment (doc 20 §A9) and the
Invert adjustment (doc 20 §A6). Other adjustment types register from their own ops_*.py module.
"""

import numpy as np

from .blend import BLEND_MODES
from .composite import (ADJUST, COMPOSITE, clip_interior, clip_shape, coverage, mask_factor,
                        render_document)
from .core import ScriptError, assemble, canonicalize, dec, q
from .fills import layer_fill, mask_fill
from .model import AdjustmentLayer, Group, Mask, RasterLayer
from .registry import op, parse_adjustment, register_adjustment

GROUP_MODES = BLEND_MODES + ("pass", "isolated")
LAYER_FILLS = ("empty", "solid", "gradient", "checker", "noise")
MASK_FILLS = ("solid", "gradient", "noise")


def _insert_top(doc, f, node):
    """Placement shared by add_layer / add_group / add_adjustment: top of `parent`."""
    parent = doc.get_container(f.str("parent", "root"))
    parent.children.append(node)


# ---------------------------------------------------------------------------------------------
# creation
# ---------------------------------------------------------------------------------------------

@op("add_layer")
def add_layer(doc, f):
    node_id = f.str("id")
    doc.check_new_id(node_id)
    kind = f.str("fill", "empty", choices=LAYER_FILLS)
    rect = f.rect("rect", doc.w, doc.h)
    kw = {}
    if kind == "solid":
        kw["color"] = f.color("color", "#000000FF")
    elif kind == "gradient":
        kw["from"] = f.color("from", "#000000FF")
        kw["to"] = f.color("to", "#FFFFFFFF")
        kw["dir"] = f.str("dir", "h", choices=("h", "v"))
    elif kind == "checker":
        kw["a"] = f.color("a", "#FFFFFFFF")
        kw["b"] = f.color("b", "#CCCCCCFF")
        kw["cell"] = f.int("cell", 8, 1, 4096)
    elif kind == "noise":
        kw["seed"] = f.seed("seed", 0)
        alpha = f.raw("alpha", "random")
        if alpha != "random":
            alpha = f.grey("alpha")
        kw["alpha"] = alpha
    node = RasterLayer(node_id, layer_fill(kind, doc.w, doc.h, rect, **kw))
    _insert_top(doc, f, node)


@op("add_group")
def add_group(doc, f):
    node_id = f.str("id")
    doc.check_new_id(node_id)
    mode = f.str("mode", "pass", choices=GROUP_MODES)
    if mode == "isolated":
        mode = "norm"
    _insert_top(doc, f, Group(node_id, mode))


@op("add_adjustment")
def add_adjustment(doc, f):
    node_id = f.str("id")
    doc.check_new_id(node_id)
    adj_type = f.str("type")
    params = parse_adjustment(adj_type, f.obj("params", {}))
    _insert_top(doc, f, AdjustmentLayer(node_id, adj_type, params))


# -- Invert (doc 20 §A6): LUT[i] = 255 - i, params {} ------------------------------------------

_INVERT_LUT = (255 - np.arange(256)).astype(np.uint8)


def _parse_invert(p):
    return None          # no keys accepted; any key is reported by the unknown-field check


def _apply_invert(params, r, g, b):
    return _INVERT_LUT[r], _INVERT_LUT[g], _INVERT_LUT[b]


register_adjustment("invert", _parse_invert, _apply_invert)


# ---------------------------------------------------------------------------------------------
# structure
# ---------------------------------------------------------------------------------------------

@op("move_layer")
def move_layer(doc, f):
    node = doc.get_node(f.str("id"))
    old_parent = doc.parent_of(node)
    if f.has("parent"):
        new_parent = doc.get_container(f.str("parent"))
    else:
        new_parent = old_parent
    if node.kind == "group":
        p = new_parent
        # new_parent must not be `node` or one of its descendants
        if p is node or (p is not doc.root and _is_descendant(node, p)):
            raise ScriptError("move_layer: cannot move group %r into itself or a descendant" % node.id)
    old_parent.children.remove(node)
    n = len(new_parent.children)
    index = f.int("index", n, 0, n)
    new_parent.children.insert(index, node)


def _is_descendant(group, candidate):
    for ch in group.children:
        if ch is candidate:
            return True
        if ch.kind == "group" and _is_descendant(ch, candidate):
            return True
    return False


# ---------------------------------------------------------------------------------------------
# properties
# ---------------------------------------------------------------------------------------------

@op("set_blend")
def set_blend(doc, f):
    node = doc.get_node(f.str("layer"))
    if node.kind == "group":
        mode = f.str("mode", choices=GROUP_MODES)
        if mode == "isolated":
            mode = "norm"
    else:
        mode = f.str("mode", choices=BLEND_MODES)
    node.mode = mode
    if f.has("seed"):
        node.seed = f.seed("seed")


@op("set_opacity")
def set_opacity(doc, f):
    node = doc.get_node(f.str("layer"))
    node.opacity = f.unit_num("value")


@op("set_fill")
def set_fill(doc, f):
    node = doc.get_node(f.str("layer"), ("raster", "adjustment"))
    node.fill = f.unit_num("value")


@op("set_visible")
def set_visible(doc, f):
    node = doc.get_node(f.str("layer"))
    node.visible = f.bool("value")


@op("set_clip")
def set_clip(doc, f):
    node = doc.get_node(f.str("layer"), ("raster", "adjustment"))
    node.clip = f.bool("value")


@op("set_clbl")
def set_clbl(doc, f):
    node = doc.get_node(f.str("layer"), ("raster", "adjustment"))
    node.clbl = f.bool("value")


@op("lock_transparency")
def lock_transparency(doc, f):
    node = doc.get_raster(f.str("layer"))
    node.lock_alpha = f.bool("value", True)


# ---------------------------------------------------------------------------------------------
# masks (§8)
# ---------------------------------------------------------------------------------------------

@op("add_mask")
def add_mask(doc, f):
    node = doc.get_node(f.str("layer"))
    kind = f.str("fill", "solid", choices=MASK_FILLS)
    rect = f.rect("rect", doc.w, doc.h)
    outside = f.grey("outside", 0)
    kw = {}
    if kind == "solid":
        kw["value"] = f.grey("value", 255)
    elif kind == "gradient":
        kw["from"] = f.grey("from", 0)
        kw["to"] = f.grey("to", 255)
        kw["dir"] = f.str("dir", "h", choices=("h", "v"))
    elif kind == "noise":
        kw["seed"] = f.seed("seed", 0)
    node.mask = Mask(mask_fill(kind, doc.w, doc.h, rect, outside, **kw), True, outside)


def _node_with_mask(doc, f, kinds=("raster", "adjustment", "group")):
    node = doc.get_node(f.str("layer"), kinds)
    if node.mask is None:
        raise ScriptError("%r has no mask" % node.id)
    return node


@op("set_mask_enabled")
def set_mask_enabled(doc, f):
    node = _node_with_mask(doc, f)
    node.mask.enabled = f.bool("value")


@op("delete_mask")
def delete_mask(doc, f):
    node = _node_with_mask(doc, f)
    node.mask = None


@op("apply_mask")
def apply_mask(doc, f):
    node = _node_with_mask(doc, f, ("raster",))
    a = q(dec(node.rgba[..., 3]) * dec(node.mask.data))     # applied even if disabled; ignores lock
    rgba = node.rgba.copy()
    rgba[..., 3] = a
    node.rgba = canonicalize(rgba)
    node.mask = None


# ---------------------------------------------------------------------------------------------
# merges (§9)
# ---------------------------------------------------------------------------------------------

@op("merge_down")
def merge_down(doc, f):
    U = doc.get_node(f.str("layer"))
    if U.kind not in ("raster", "adjustment"):
        raise ScriptError("merge_down: %r must be a raster or adjustment layer" % U.id)
    parent = doc.parent_of(U)
    idx = parent.children.index(U)
    if idx == 0:
        raise ScriptError("merge_down: %r is at the bottom of its container" % U.id)
    L = parent.children[idx - 1]
    if L.kind != "raster":
        raise ScriptError("merge_down: the node below %r is not a raster layer" % U.id)
    if not (U.visible and L.visible):
        raise ScriptError("merge_down: both layers must be visible")
    if idx + 1 < len(parent.children):
        above = parent.children[idx + 1]
        if getattr(above, "clip", False):
            raise ScriptError("merge_down: the node above %r is clipped" % U.id)

    h, w = doc.h, doc.w
    mL = mask_factor(L, h, w)
    if U.clip and not L.clip:
        # §6.1 steps 1-2 with base L (original A_L, m_L, f_L) and vis = [U]
        G = clip_interior(L, [U], h, w)
        S = clip_shape(L, h, w)
        result = assemble(G[..., 0], G[..., 1], G[..., 2], q(S * dec(G[..., 3])))
    else:
        # step 1: bake L's mask and fill
        Lp = L.rgba.copy()
        Lp[..., 3] = q((dec(L.rgba[..., 3]) * mL) * L.fill)
        canonicalize(Lp)
        cU = coverage(U, h, w)
        if U.kind == "raster":
            result = COMPOSITE(Lp, U.rgba[..., :3], cU, U.mode, U.seed)
        else:
            result = ADJUST(Lp, U, cU, U.mode, U.seed)
    L.rgba = result
    L.fill = 1.0
    L.mask = None
    parent.children.remove(U)


def _new_flat_layer(node_id, pixels):
    return RasterLayer(node_id, pixels)      # §9.2 step 3 properties are RasterLayer defaults


@op("merge_visible")
def merge_visible(doc, f):
    node_id = f.str("id", "merged")
    kids = doc.root.children
    visible_idx = [i for i, n in enumerate(kids) if n.visible]
    if not visible_idx:
        return
    P = render_document(doc, onto_bg=False)
    k = visible_idx[0]          # hidden top-level nodes below the bottommost removed node
    doc.root.children = [n for n in kids if not n.visible]
    doc.check_new_id(node_id)
    doc.root.children.insert(k, _new_flat_layer(node_id, P))


@op("flatten")
def flatten(doc, f):
    node_id = f.str("id", "flattened")
    P = render_document(doc, onto_bg=True)
    doc.root.children = []
    doc.check_new_id(node_id)
    doc.root.children.append(_new_flat_layer(node_id, P))
    doc.bg = (0, 0, 0, 0)
