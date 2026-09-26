"""Document model (doc 10 §1, C8a, doc 30 §2 selection storage).

Node kinds: 'raster', 'adjustment', 'group'. The root is a Group with id "root" that is not
addressable as a layer. Child lists are bottom-first (index 0 = bottom).
"""

import copy

import numpy as np

from .core import ScriptError

KINDS = ("raster", "adjustment", "group")


class Mask:
    """A canvas-sized 8-bit layer mask. `outside` is the add_mask `outside` value (kept for reference)."""

    def __init__(self, data, enabled=True, outside=0):
        self.data = data          # np.uint8 (h, w)
        self.enabled = enabled
        self.outside = outside


class Node:
    kind = None

    def __init__(self, node_id):
        self.id = node_id
        self.visible = True
        self.opacity = 1.0
        self.mask = None          # Mask or None
        self.seed = 0             # Dissolve seed


class RasterLayer(Node):
    kind = "raster"

    def __init__(self, node_id, rgba):
        super().__init__(node_id)
        self.rgba = rgba          # np.uint8 (h, w, 4), straight RGBA, canonical
        self.mode = "norm"
        self.fill = 1.0
        self.clip = False
        self.clbl = True
        self.lock_alpha = False


class AdjustmentLayer(Node):
    kind = "adjustment"

    def __init__(self, node_id, adj_type, params):
        super().__init__(node_id)
        self.adj_type = adj_type  # e.g. "invert"
        self.params = params      # whatever the type's parser returned
        self.mode = "norm"
        self.fill = 1.0
        self.clip = False
        self.clbl = True


class Group(Node):
    kind = "group"

    def __init__(self, node_id, mode="pass"):
        super().__init__(node_id)
        self.mode = mode          # "pass" or a blend-mode JSON key ("isolated" is stored as "norm")
        self.children = []        # bottom first


class Document:
    def __init__(self, w, h, bg):
        self.w = w
        self.h = h
        self.bg = bg                                   # (r, g, b, a) ints, canonical
        self.root = Group("root", "pass")
        self.sel = np.zeros((h, w), dtype=np.uint8)    # doc 30 §2 `S`; all-zero = no selection
        self.sel_saved = None                          # doc 30 §2 `Saved` (None = absent)
        self.ext = {}                                  # per-module private state (snapshotted)

    # -- snapshots (C10) -------------------------------------------------------------------
    def snapshot(self):
        return copy.deepcopy(self)

    def restore(self, snap):
        """Become `snap` (a snapshot popped off the history; it is not reused afterwards)."""
        self.__dict__.clear()
        self.__dict__.update(snap.__dict__)

    # -- tree queries ----------------------------------------------------------------------
    def iter_nodes(self, container=None):
        """Every node (not the root), depth-first, bottom-to-top within each container."""
        container = container or self.root
        for n in container.children:
            yield n
            if n.kind == "group":
                yield from self.iter_nodes(n)

    def find(self, node_id):
        for n in self.iter_nodes():
            if n.id == node_id:
                return n
        return None

    def get_node(self, node_id, kinds=KINDS, what="layer"):
        """Resolve an id to a node of one of `kinds`; unknown id / wrong kind / "root" -> ScriptError."""
        if not isinstance(node_id, str):
            raise ScriptError("%s id must be a string" % what)
        if node_id == "root":
            raise ScriptError("%r: 'root' is not a %s" % (node_id, what))
        n = self.find(node_id)
        if n is None:
            raise ScriptError("unknown id %r" % node_id)
        if n.kind not in kinds:
            raise ScriptError("%r is a %s; this op needs %s" % (node_id, n.kind, "/".join(kinds)))
        return n

    def get_raster(self, node_id):
        return self.get_node(node_id, ("raster",))

    def get_container(self, node_id):
        """'root' or a group id -> the container (Group)."""
        if node_id == "root":
            return self.root
        return self.get_node(node_id, ("group",), "container")

    def parent_of(self, node):
        """The container (root or a Group) whose child list holds `node`."""
        def walk(c):
            for ch in c.children:
                if ch is node:
                    return c
                if ch.kind == "group":
                    r = walk(ch)
                    if r is not None:
                        return r
            return None
        p = walk(self.root)
        if p is None:
            raise AssertionError("node %r is not in the tree" % node.id)
        return p

    def check_new_id(self, node_id):
        if not isinstance(node_id, str) or node_id == "":
            raise ScriptError("ids must be non-empty strings")
        if node_id == "root":
            raise ScriptError("'root' is a reserved id")
        if self.find(node_id) is not None:
            raise ScriptError("duplicate id %r" % node_id)

    # -- selection (doc 30 §2; storage only) ------------------------------------------------
    def selection_is_empty(self):
        return not self.sel.any()

    def selection_effective(self):
        """E(x, y): 255 everywhere if the selection is empty, else S (uint8 (h, w), a new array)."""
        if self.selection_is_empty():
            return np.full((self.h, self.w), 255, dtype=np.uint8)
        return self.sel.copy()

    # -- construction helpers ----------------------------------------------------------------
    def empty_rgba(self):
        return np.zeros((self.h, self.w, 4), dtype=np.uint8)
