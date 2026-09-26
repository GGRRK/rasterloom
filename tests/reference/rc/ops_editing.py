# SPDX-License-Identifier: GPL-3.0-or-later
"""Render-script ops of doc 60 (editing ops): delete_layer, set_name, duplicate_layer,
set_adjustment, set_group_mode, select_alpha.

Written from docs/math/60-editing-ops.md only (independence rule: no C++ was read).

Node names (doc 60 §1.1) are stored as the attribute `name` on nodes. Nodes created by other
modules do not carry the attribute; `node_name()` reads it with the default "" (§1.1: every op
that creates a node creates it with name ""). Names never enter any pixel formula; they are kept
so that duplicate naming (§4.4) and undo (§1.3) can be self-checked.
"""

import copy

import numpy as np

from .core import ScriptError
from .registry import op, parse_adjustment

NAME_MAX = 255                                  # §1.1 rule 3, in code points
SELECT_MODES = ("new", "add", "subtract", "intersect")
GROUP_MODES = ("pass", "isolated")


# ---------------------------------------------------------------------------------------------
# names (§1.1)
# ---------------------------------------------------------------------------------------------

def node_name(node):
    return getattr(node, "name", "")


def display_name(node):
    n = node_name(node)
    return n if n != "" else node.id


def name_problem(s):
    """None if `s` is a valid name (§1.1), else a short reason.

    Python's json accepts lone-surrogate escapes and keeps them in the str (doc 60 §13), so the
    scalar-value check is explicit; once it passes, len() counts code points."""
    for ch in s:
        c = ord(ch)
        if 0xD800 <= c <= 0xDFFF:
            return "contains a lone surrogate (not a Unicode scalar value)"
        if c <= 0x1F or c == 0x7F:
            return "contains the control code point U+%04X" % c
    if len(s) > NAME_MAX:
        return "has %d code points (at most %d)" % (len(s), NAME_MAX)
    return None


def _is_valid_name(s):
    return name_problem(s) is None


def _subtree(node):
    """`node` and every descendant, depth-first."""
    yield node
    if node.kind == "group":
        for ch in node.children:
            yield from _subtree(ch)


# ---------------------------------------------------------------------------------------------
# §2 delete_layer
# ---------------------------------------------------------------------------------------------

@op("delete_layer")
def delete_layer(doc, f):
    node = doc.get_node(f.str("layer"))
    f.check_unused()                   # every field validated before the document changes
    doc.parent_of(node).children.remove(node)
    # No flag changes: clip groups are re-derived at render time (doc 10 §5). The subtree's ids
    # are free because the nodes are no longer in the tree (doc.find no longer sees them).


# ---------------------------------------------------------------------------------------------
# §3 set_name
# ---------------------------------------------------------------------------------------------

@op("set_name")
def set_name(doc, f):
    node = doc.get_node(f.str("layer"))
    name = f.str("name")
    why = name_problem(name)
    if why is not None:
        raise f.err("'name' %s" % why)
    f.check_unused()
    node.name = name


# ---------------------------------------------------------------------------------------------
# §4 duplicate_layer
# ---------------------------------------------------------------------------------------------

@op("duplicate_layer")
def duplicate_layer(doc, f):
    src = doc.get_node(f.str("layer"))
    new_id = f.str("id")
    if new_id == "":
        raise f.err("'id' must be non-empty")
    if new_id == "root":
        raise f.err("'root' is a reserved id")

    # placement (§4.2)
    if f.has("parent"):
        container = doc.get_container(f.str("parent"))
        if container is src or any(container is d for d in _subtree(src)):
            raise f.err("'parent' %r is the source or inside it" % container.id)
        index = None                                   # top of `container`
    else:
        container = doc.parent_of(src)
        index = container.children.index(src) + 1     # directly above the source

    # ids (§4.3): all checked against the current document before anything changes
    originals = list(_subtree(src))
    new_ids = [new_id] + [new_id + "/" + d.id for d in originals[1:]]
    for nid in new_ids:
        if doc.find(nid) is not None:
            raise f.err("id %r is taken" % nid)
    f.check_unused()

    # deep copy (§4.1): independent storage for pixels, masks and params
    dup = copy.deepcopy(src)
    copies = list(_subtree(dup))
    assert len(copies) == len(originals)
    for c, o, nid in zip(copies, originals, new_ids):
        c.id = nid
    # names (§4.4)
    top = display_name(src) + " copy"
    dup.name = top if _is_valid_name(top) else ""
    for c, o in zip(copies[1:], originals[1:]):
        dn = display_name(o)
        c.name = dn if _is_valid_name(dn) else ""

    if index is None:
        container.children.append(dup)
    else:
        container.children.insert(index, dup)


# ---------------------------------------------------------------------------------------------
# §5 set_adjustment
# ---------------------------------------------------------------------------------------------

@op("set_adjustment")
def set_adjustment(doc, f):
    node = doc.get_node(f.str("layer"), ("adjustment",))
    # validated exactly as add_adjustment's params for the node's own type; omitted keys take
    # the type's defaults (the parser's defaults), never the previous values
    params = parse_adjustment(node.adj_type, f.obj("params"))
    f.check_unused()
    node.params = params


# ---------------------------------------------------------------------------------------------
# §6 set_group_mode
# ---------------------------------------------------------------------------------------------

@op("set_group_mode")
def set_group_mode(doc, f):
    node = doc.get_node(f.str("layer"), ("group",))
    m = f.str("mode", choices=GROUP_MODES)
    f.check_unused()
    if m == "pass":
        node.mode = "pass"
    elif node.mode == "pass":
        node.mode = "norm"
    # else: already isolated; its blend mode is kept


# ---------------------------------------------------------------------------------------------
# §7 select_alpha
# ---------------------------------------------------------------------------------------------

def combine_selection(S, B, mode):
    """doc 30 §4 combine on bytes (§7): integers, exact."""
    s = S.astype(np.int64)
    b = B.astype(np.int64)
    if mode == "new":
        r = b
    elif mode == "add":
        r = np.maximum(s, b)
    elif mode == "subtract":
        r = np.maximum(s - b, 0)
    elif mode == "intersect":
        r = np.minimum(s, b)
    else:
        raise AssertionError(mode)
    return r.astype(np.uint8)


@op("select_alpha")
def select_alpha(doc, f):
    layer = doc.get_raster(f.str("layer"))
    mode = f.str("mode", "new", choices=SELECT_MODES)
    f.check_unused()
    # own stored alpha only: no mask, visibility, opacity, fill, mode, clipping or lock
    B = layer.rgba[:, :, 3]
    doc.sel = combine_selection(doc.sel, B, mode)
    # Saved (doc.sel_saved) is untouched; an all-zero result simply means "no selection"
