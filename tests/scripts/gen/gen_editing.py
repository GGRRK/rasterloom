#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Golden render scripts for docs/math/60-editing-ops.md (section 10).

Emits one file per golden id (DEL-*, NAME-*, DUP-*, ADJ-*, GM-*, SAL-*, HIST-*, their `*R`
equal_to targets, FIX-K) plus err_*.json scripts the doc defines as script errors
("expect": "error", tests/scripts/README.md).

Output directory:
  default     tests/scripts/.pending-editing/   (a dot-directory: run_goldens.py and the per-domain
                                                CTest glob skip it, so the gates stay green while
                                                the doc-60 ops are unimplemented)
  --activate  tests/scripts/editing/            (and the pending directory is removed); run this as
                                                the last step of the implementation lane, then
                                                re-run CMake so goldens_editing is registered.

Written from docs/math/ only (00, 10, 20, 30, 60). Deterministic: re-running produces
byte-identical files. Before writing anything, every script goes through a structural checker
(`Sim`): a pixel-free model of the doc-60 node tree that knows ids, node kinds, containers, clip
flags, masks and history. Valid goldens must pass it; every err_* script must fail it with the
specific reason recorded next to it, so an error golden cannot be "valid by accident" or fail for
an unrelated reason.
"""
import copy
import json
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPTS = os.path.normpath(os.path.join(HERE, ".."))
OUT_PENDING = os.path.join(SCRIPTS, ".pending-editing")
OUT_ACTIVE = os.path.join(SCRIPTS, "editing")

FILL_BLUE = "#1060e0ff"


# ------------------------------------------------------------------------------------ output
def dump_script(script, ascii_only=False):
    def j(v):
        return json.dumps(v, ensure_ascii=ascii_only)
    lines = ["{", '  "canvas": ' + j(script["canvas"]) + ",", '  "ops": [']
    ops = script["ops"]
    for i, op in enumerate(ops):
        lines.append("    " + j(op) + ("," if i + 1 < len(ops) else ""))
    lines.append("  ],")
    tail = [("out", script["out"])]
    for k in ("expect", "equal_to"):
        if k in script:
            tail.append((k, script[k]))
    for i, (k, v) in enumerate(tail):
        lines.append("  " + j(k) + ": " + j(v) + ("," if i + 1 < len(tail) else ""))
    lines.append("}")
    return "\n".join(lines) + "\n"


def script(ops, w=64, h=64, bg="#00000000", **extra):
    s = {"canvas": {"w": w, "h": h, "bg": bg}, "ops": ops, "out": "png8"}
    s.update(extra)
    return s


# ------------------------------------------------------------------------------ op builders
def layer(id_, parent=None, **kw):
    op = {"op": "add_layer", "id": id_}
    if parent is not None:
        op["parent"] = parent
    op.update(kw)
    return op


def solid(id_, color, rect=None, parent=None):
    kw = {"fill": "solid", "color": color}
    if rect is not None:
        kw["rect"] = rect
    return layer(id_, parent, **kw)


def group(id_, mode=None, parent=None):
    op = {"op": "add_group", "id": id_}
    if parent is not None:
        op["parent"] = parent
    if mode is not None:
        op["mode"] = mode
    return op


def adjustment(id_, type_, params=None, parent=None):
    op = {"op": "add_adjustment", "id": id_, "type": type_}
    if params is not None:
        op["params"] = params
    if parent is not None:
        op["parent"] = parent
    return op


def setp(op, layer_id, value, **kw):
    d = {"op": op, "layer": layer_id, "value": value}
    d.update(kw)
    return d


def blend(layer_id, mode, seed=None):
    d = {"op": "set_blend", "layer": layer_id, "mode": mode}
    if seed is not None:
        d["seed"] = seed
    return d


def undo(steps=None):
    return {"op": "undo"} if steps is None else {"op": "undo", "steps": steps}


def delete(l):
    return {"op": "delete_layer", "layer": l}


def set_name(l, name):
    return {"op": "set_name", "layer": l, "name": name}


def dup(l, id_, parent=None):
    d = {"op": "duplicate_layer", "layer": l, "id": id_}
    if parent is not None:
        d["parent"] = parent
    return d


def set_adj(l, params):
    return {"op": "set_adjustment", "layer": l, "params": params}


def gmode(l, mode):
    return {"op": "set_group_mode", "layer": l, "mode": mode}


def sel_alpha(l, mode=None):
    d = {"op": "select_alpha", "layer": l}
    if mode is not None:
        d["mode"] = mode
    return d


def sel_rect(x, y, w, h):
    return {"op": "select_rect", "x": float(x), "y": float(y), "w": float(w), "h": float(h)}


def sel_ellipse(x, y, w, h):
    return {"op": "select_ellipse", "x": float(x), "y": float(y), "w": float(w), "h": float(h),
            "antialias": True}


def fill_sel(l, color=FILL_BLUE, opacity=None):
    d = {"op": "fill_selection", "layer": l, "color": color}
    if opacity is not None:
        d["opacity"] = opacity
    return d


# ---------------------------------------------------------------------------------- fixtures
def K():
    """Doc 60 fixture K (doc 10 section 12.2's backdrop) on 64x64."""
    return [layer("k", fill="gradient", **{"from": "#2040C0FF", "to": "#F0C020FF"}, dir="h")]


def BASE(id_="base", rect=(16, 8, 40, 48)):
    """Red alpha-ramp clip base (doc 10 section 12.2's 'base', scaled to 64x64)."""
    return layer(id_, fill="gradient", **{"from": "#FF000000", "to": "#FF0000FF"}, dir="v",
                 rect=list(rect))


def CLIPNOISE(id_="c", alpha=255, rect=None):
    kw = {"fill": "noise", "seed": 5, "alpha": alpha}
    if rect is not None:
        kw["rect"] = rect
    return [layer(id_, **kw), setp("set_clip", id_, True)]


def SRC():
    """Doc 60 fixture SRC: hidden, masked, faded alpha-ramp source + empty target L."""
    return [layer("src", fill="gradient", **{"from": "#FF000000", "to": "#FF0000FF"}, dir="h",
                  rect=[8, 0, 48, 64]),
            {"op": "add_mask", "layer": "src", "fill": "noise", "seed": 3},
            setp("set_opacity", "src", 0.3),
            setp("set_visible", "src", False),
            layer("L")]


U = lambda: solid("u", "#10E080FF", [8, 8, 32, 32])                       # noqa: E731
T_MUL = lambda: [solid("t", "#E03020FF", [20, 20, 32, 32]), blend("t", "mul")]  # noqa: E731


# ------------------------------------------------------------------------ structural checker
class ScriptError(Exception):
    def __init__(self, reason):
        super().__init__(reason)
        self.reason = reason


REQ = object()
COMMON_MODES = {"norm", "diss", "dark", "mul", "idiv", "lbrn", "dkCl", "lite", "scrn", "div",
                "lddg", "lgCl", "over", "sLit", "hLit", "vLit", "lLit", "pLit", "hMix", "diff",
                "smud", "fsub", "fdiv", "hue", "sat", "colr", "lum"}
ADD_LAYER_FIELDS = {"id", "parent", "fill", "color", "from", "to", "dir", "a", "b", "cell", "seed",
                    "alpha", "rect"}
ADD_MASK_FIELDS = {"layer", "fill", "value", "from", "to", "dir", "seed", "rect", "outside"}


def valid_name(v):
    if not isinstance(v, str):
        return "name-type"
    if any(0xD800 <= ord(ch) <= 0xDFFF for ch in v):
        return "name-surrogate"
    if any(ord(ch) <= 0x1F or ord(ch) == 0x7F for ch in v):
        return "name-control"
    if len(v) > 255:
        return "name-length"
    return None


def check_params(type_, p):
    """Doc 20 params validation, for the adjustment types the goldens use."""
    if not isinstance(p, dict):
        raise ScriptError("params-type")

    def num(k, lo, hi, default, integer=False):
        v = p.get(k, default)
        if integer and (not isinstance(v, int) or isinstance(v, bool)):
            raise ScriptError("params-int")
        if not integer and (not isinstance(v, (int, float)) or isinstance(v, bool)):
            raise ScriptError("params-num")
        if not lo <= v <= hi:
            raise ScriptError("params-range")
        return v

    def keys(allowed):
        extra = set(p) - allowed
        if extra:
            raise ScriptError("params-unknown-key")

    if type_ == "invert":
        keys(set())
    elif type_ == "posterize":
        keys({"levels"})
        num("levels", 2, 255, 4, True)
    elif type_ == "threshold":
        keys({"level"})
        num("level", 1, 255, 128, True)
    elif type_ == "levels":
        keys({"rgb", "r", "g", "b"})
        for ch in p.values():
            if not isinstance(ch, dict):
                raise ScriptError("params-type")
            extra = set(ch) - {"in_black", "in_white", "gamma", "out_black", "out_white"}
            if extra:
                raise ScriptError("params-unknown-key")
            ib = ch.get("in_black", 0)
            iw = ch.get("in_white", 255)
            if not (isinstance(ib, int) and 0 <= ib <= 254 and isinstance(iw, int) and 1 <= iw <= 255):
                raise ScriptError("params-range")
            if not ib < iw:
                raise ScriptError("params-levels-order")
            g = ch.get("gamma", 1.0)
            if not 0.1 <= g <= 9.99:
                raise ScriptError("params-range")
    elif type_ == "hue_saturation":
        keys({"colorize", "hue", "saturation", "lightness"})
        c = p.get("colorize", False)
        if not isinstance(c, bool):
            raise ScriptError("params-type")
        if c:
            num("hue", 0, 360, 0.0), num("saturation", 0, 100, 25.0)
        else:
            num("hue", -180, 180, 0.0), num("saturation", -100, 100, 0.0)
        num("lightness", -100, 100, 0.0)
    else:
        raise ScriptError("params-type-unknown-to-checker")


class Node:
    def __init__(self, kind, id_, **props):
        self.kind, self.id, self.children = kind, id_, []
        self.clip = False
        self.mask = None            # None or {"enabled": bool}
        self.mode = "pass" if kind == "group" else "norm"
        self.name = ""
        self.visible = True
        self.lock = False
        self.adj = None             # (type, params) for adjustments
        self.__dict__.update(props)


class Sim:
    def __init__(self):
        self.root = Node("group", "root")
        self.history = []
        self.sel_nonempty = False   # coarse: only 'has a selection op run', enough for the checker
        self.saved = False

    # tree helpers
    def walk(self, n=None):
        n = n or self.root
        for c in n.children:
            yield c
            yield from self.walk(c)

    def ids(self):
        return {n.id for n in self.walk()}

    def find(self, id_):
        for n in self.walk():
            if n.id == id_:
                return n
        return None

    def parent_of(self, target, n=None):
        n = n or self.root
        for i, c in enumerate(n.children):
            if c is target:
                return n, i
            r = self.parent_of(target, c)
            if r:
                return r
        return None

    def node(self, op, key="layer", kinds=("raster", "adjustment", "group")):
        if key not in op:
            raise ScriptError("missing-" + key)
        v = op[key]
        if not isinstance(v, str):
            raise ScriptError(key + "-type")
        if v == "root":
            raise ScriptError(key + "-root")
        n = self.find(v)
        if n is None:
            raise ScriptError(key + "-unknown")
        if n.kind not in kinds:
            raise ScriptError(key + "-kind")
        return n

    def container(self, op):
        v = op.get("parent", "root")
        if not isinstance(v, str):
            raise ScriptError("parent-type")
        if v == "root":
            return self.root
        n = self.find(v)
        if n is None:
            raise ScriptError("parent-unknown")
        if n.kind != "group":
            raise ScriptError("parent-kind")
        return n

    def new_id(self, op):
        if "id" not in op:
            raise ScriptError("missing-id")
        v = op["id"]
        if not isinstance(v, str) or v == "":
            raise ScriptError("id-type")
        if v == "root":
            raise ScriptError("id-root")
        if v in self.ids():
            raise ScriptError("id-taken")
        return v

    @staticmethod
    def fields(op, allowed):
        extra = set(op) - {"op"} - set(allowed)
        if extra:
            raise ScriptError("unknown-field")

    # ops
    def run(self, op):
        name = op.get("op")
        if name == "undo":
            self.fields(op, {"steps"})
            steps = op.get("steps", 1)
            if steps > len(self.history):
                raise ScriptError("undo-past-start")
            state = self.history[-steps]
            del self.history[-steps:]
            self.root, self.sel_nonempty, self.saved = state
            return
        snapshot = (copy.deepcopy(self.root), self.sel_nonempty, self.saved)
        getattr(self, "op_" + name)(op)
        self.history.append(snapshot)

    def op_add_layer(self, op):
        self.fields(op, ADD_LAYER_FIELDS)
        c = self.container(op)
        c.children.append(Node("raster", self.new_id(op)))

    def op_add_group(self, op):
        self.fields(op, {"id", "parent", "mode"})
        c = self.container(op)
        mode = op.get("mode", "pass")
        if mode not in COMMON_MODES | {"pass", "isolated"}:
            raise ScriptError("mode-value")
        c.children.append(Node("group", self.new_id(op), mode="norm" if mode == "isolated" else mode))

    def op_add_adjustment(self, op):
        self.fields(op, {"id", "type", "params", "parent"})
        c = self.container(op)
        p = op.get("params", {})
        check_params(op["type"], p)
        c.children.append(Node("adjustment", self.new_id(op), adj=(op["type"], p)))

    def op_add_mask(self, op):
        self.fields(op, ADD_MASK_FIELDS)
        self.node(op).mask = {"enabled": True}

    def op_set_mask_enabled(self, op):
        self.fields(op, {"layer", "value"})
        n = self.node(op)
        if n.mask is None:
            raise ScriptError("no-mask")
        n.mask["enabled"] = op["value"]

    def op_set_blend(self, op):
        self.fields(op, {"layer", "mode", "seed"})
        n = self.node(op)
        m = op["mode"]
        ok = COMMON_MODES | ({"pass", "isolated"} if n.kind == "group" else set())
        if m not in ok:
            raise ScriptError("mode-value")
        n.mode = "norm" if m == "isolated" else m

    def op_set_opacity(self, op):
        self.fields(op, {"layer", "value"})
        self.node(op)

    def op_set_visible(self, op):
        self.fields(op, {"layer", "value"})
        self.node(op).visible = op["value"]

    def op_set_fill(self, op):
        self.fields(op, {"layer", "value"})
        self.node(op, kinds=("raster", "adjustment"))

    def op_set_clip(self, op):
        self.fields(op, {"layer", "value"})
        self.node(op, kinds=("raster", "adjustment")).clip = op["value"]

    def op_set_clbl(self, op):
        self.fields(op, {"layer", "value"})
        self.node(op, kinds=("raster", "adjustment"))

    def op_lock_transparency(self, op):
        self.fields(op, {"layer", "value"})
        self.node(op, kinds=("raster",)).lock = op.get("value", True)

    def op_merge_down(self, op):
        self.fields(op, {"layer"})
        u = self.node(op, kinds=("raster", "adjustment"))
        par, i = self.parent_of(u)
        if i == 0:
            raise ScriptError("merge-bottom")
        low = par.children[i - 1]
        if low.kind != "raster" or not (u.visible and low.visible):
            raise ScriptError("merge-lower")
        if i + 1 < len(par.children) and par.children[i + 1].kind != "group" and par.children[i + 1].clip:
            raise ScriptError("merge-clipped-above")
        par.children.pop(i)
        low.mask = None

    def op_select_rect(self, op):
        self.fields(op, {"x", "y", "w", "h", "antialias", "mode"})
        self.sel_nonempty = True

    op_select_ellipse = op_select_rect

    def op_deselect(self, op):
        self.fields(op, set())
        if self.sel_nonempty:
            self.saved = True
        self.sel_nonempty = False

    def op_reselect(self, op):
        self.fields(op, set())
        if self.saved:
            self.sel_nonempty = True

    def op_fill_selection(self, op):
        self.fields(op, {"layer", "color", "opacity"})
        self.node(op, kinds=("raster",))

    # ---- doc 60
    def op_delete_layer(self, op):
        self.fields(op, {"layer"})
        n = self.node(op)
        par, i = self.parent_of(n)
        par.children.pop(i)

    def op_set_name(self, op):
        self.fields(op, {"layer", "name"})
        n = self.node(op)
        if "name" not in op:
            raise ScriptError("missing-name")
        bad = valid_name(op["name"])
        if bad:
            raise ScriptError(bad)
        n.name = op["name"]

    def op_duplicate_layer(self, op):
        self.fields(op, {"layer", "id", "parent"})
        s = self.node(op)
        top = self.new_id(op)
        if "parent" in op:
            dest = self.container(op)
            inside = [s] + list(self.walk(s))
            if dest in inside:
                raise ScriptError("parent-inside-source")
            index = len(dest.children)
        else:
            dest, i = self.parent_of(s)
            index = i + 1
        taken = self.ids()
        c = copy.deepcopy(s)
        c.id = top
        for d in self.walk(c):
            d.id = top + "/" + d.id
            if d.id in taken:
                raise ScriptError("derived-id-taken")
        dest.children.insert(index, c)

    def op_set_adjustment(self, op):
        self.fields(op, {"layer", "params"})
        n = self.node(op, kinds=("adjustment",))
        if "params" not in op:
            raise ScriptError("missing-params")
        check_params(n.adj[0], op["params"])
        n.adj = (n.adj[0], op["params"])

    def op_set_group_mode(self, op):
        self.fields(op, {"layer", "mode"})
        g = self.node(op, kinds=("group",))
        if "mode" not in op:
            raise ScriptError("missing-mode")
        m = op["mode"]
        if m == "pass":
            g.mode = "pass"
        elif m == "isolated":
            if g.mode == "pass":
                g.mode = "norm"
        else:
            raise ScriptError("mode-value")

    def op_select_alpha(self, op):
        self.fields(op, {"layer", "mode"})
        self.node(op, kinds=("raster",))
        if op.get("mode", "new") not in ("new", "add", "subtract", "intersect"):
            raise ScriptError("mode-value")
        self.sel_nonempty = True


def check(name, s, reason):
    sim = Sim()
    try:
        for op in s["ops"]:
            sim.run(op)
    except ScriptError as e:
        if reason is None:
            raise SystemExit(f"{name}: valid golden fails the structural check: {e.reason}")
        if e.reason != reason:
            raise SystemExit(f"{name}: error golden fails for {e.reason!r}, expected {reason!r}")
        return
    except (KeyError, AttributeError, TypeError) as e:
        raise SystemExit(f"{name}: checker crashed ({e!r})")
    if reason is not None:
        raise SystemExit(f"{name}: error golden is valid under the structural check")


# ------------------------------------------------------------------------------------- build
def build():
    g = {}       # name -> script
    why = {}     # err name -> expected checker reason
    ascii_only = set()

    def err(name, ops, reason, **kw):
        g[name] = script(ops, expect="error", **kw)
        why[name] = reason

    g["FIX-K"] = script(K())

    # ---- 10.1 delete_layer
    g["DEL-01"] = script(K() + [U(), solid("v", "#E03020C0", [24, 24, 32, 32]), delete("u")])
    g["DEL-02"] = script(K() + [group("g"), solid("c1", "#10E080FF", [8, 8, 32, 32], "g"),
                                solid("c2", "#E03020FF", [24, 24, 32, 32], "g"), blend("c2", "mul"),
                                delete("g")], equal_to="FIX-K")
    g["DEL-03"] = script(K() + [solid("b0", "#3050FFFF", [4, 4, 28, 56]), BASE()] + CLIPNOISE()
                         + [delete("base")])
    g["DEL-04"] = script(K() + [group("g"), solid("x", "#FFFFFF60", [0, 0, 16, 16], "g"), BASE()]
                         + CLIPNOISE(alpha=128, rect=[8, 8, 48, 48]) + [delete("base")])
    g["DEL-05"] = script(K() + [BASE()] + CLIPNOISE("c1")
                         + [solid("c2", "#20A0FFFF", [0, 24, 64, 16]), setp("set_clip", "c2", True),
                            blend("c2", "mul"), delete("c1")])
    g["DEL-06"] = script(K() + [group("g", "isolated"), solid("c1", "#10E080FF", [8, 8, 32, 32], "g"),
                                solid("c2", "#E03020FF", [24, 24, 32, 32], "g"), blend("c2", "mul"),
                                delete("g"), undo(), setp("set_opacity", "c2", 0.5)])
    g["DEL-07"] = script(K() + [U(), delete("u"), solid("u", "#E03020FF", [30, 12, 24, 40])])
    g["DEL-08"] = script([solid("a", "#10E080FF", [8, 8, 32, 32]), group("g"),
                          solid("b", "#E03020FF", [24, 24, 32, 32], "g"), delete("a"), delete("g")],
                         bg="#808080FF")
    g["DEL-09"] = script(K() + [U(), blend("u", "mul"), solid("c", "#FFFFFFFF", [0, 0, 64, 32]),
                                setp("set_clip", "c", True), delete("c"), {"op": "merge_down", "layer": "u"}])

    # ---- 10.2 set_name
    g["NAME-01R"] = script(K() + [U()])
    g["NAME-01"] = script(K() + [U(), set_name("u", "Übermalung Ω"), undo()], equal_to="NAME-01R")
    g["NAME-02"] = script(K() + [U(), set_name("u", "k"), setp("set_opacity", "k", 0.5)])
    g["NAME-03"] = script(K() + [U(), group("g"), adjustment("a", "invert", parent="g"),
                                 setp("set_fill", "a", 0.5), set_name("u", "Layer 1"),
                                 set_name("g", "Group – ☀"), set_name("a", "  spaced  "),
                                 set_name("u", ""), setp("set_opacity", "u", 0.5)])
    long_name = "é" * 254 + "\U0001D11E"
    assert len(long_name) == 255 and len(long_name.encode("utf-8")) == 512
    assert len(long_name.encode("utf-16-le")) // 2 == 256
    g["NAME-04"] = script(K() + [U(), set_name("u", long_name), setp("set_opacity", "u", 0.5)])
    err("err_name_too_long", K() + [U(), set_name("u", "n" * 256)], "name-length")
    err("err_name_control", K() + [U(), set_name("u", "two\nlines")], "name-control")
    err("err_name_del", K() + [U(), set_name("u", "del\x7f")], "name-control")
    err("err_name_lone_surrogate", K() + [U(), set_name("u", "\ud800x")], "name-surrogate")
    ascii_only.add("err_name_lone_surrogate")   # must be written as the escape \ud800
    ascii_only.add("err_name_del")              # written as the escape \u007f (readable)
    err("err_name_root", K() + [set_name("root", "x")], "layer-root")
    err("err_name_not_string", K() + [U(), set_name("u", 123)], "name-type")
    err("err_name_missing", K() + [U(), {"op": "set_name", "layer": "u"}], "missing-name")

    # ---- 10.3 duplicate_layer
    g["DUP-01"] = script(K() + [U()] + T_MUL() + [dup("u", "u2")])
    dup02_setup = K() + [layer("u", fill="noise", seed=9, alpha="random"),
                         {"op": "add_mask", "layer": "u", "fill": "gradient", "from": 255, "to": 0,
                          "dir": "v"},
                         setp("set_mask_enabled", "u", False), blend("u", "diss", 77),
                         setp("set_fill", "u", 0.6), setp("set_opacity", "u", 0.7),
                         setp("lock_transparency", "u", True)]
    dup02_tail = lambda l: [setp("set_mask_enabled", l, True), sel_rect(0, 0, 32, 64),  # noqa: E731
                            fill_sel(l, "#102030ff"), {"op": "deselect"}]
    g["DUP-02R"] = script(dup02_setup + dup02_tail("u"))
    g["DUP-02"] = script(dup02_setup + [dup("u", "u2"), delete("u")] + dup02_tail("u2"),
                         equal_to="DUP-02R")
    g["DUP-03"] = script(K() + [group("g", "scrn"), setp("set_opacity", "g", 0.8),
                                {"op": "add_mask", "layer": "g", "fill": "solid", "value": 255,
                                 "rect": [0, 0, 48, 64]},
                                solid("c1", "#10E080FF", [4, 4, 40, 40], "g"),
                                layer("c2", "g", fill="noise", seed=4, alpha=255, rect=[12, 12, 48, 48]),
                                setp("set_clip", "c2", True),
                                adjustment("a", "invert", parent="g"), setp("set_fill", "a", 0.5),
                                group("h", "isolated", "g"), solid("d", "#E03020FF", [32, 24, 28, 28], "h"),
                                dup("g", "g2"), setp("set_visible", "g", False),
                                setp("set_opacity", "g2/c1", 0.6), setp("set_fill", "g2/a", 0.8),
                                blend("g2/d", "over"), setp("set_opacity", "g2/h", 0.7)])
    g["DUP-04"] = script(K() + [BASE()] + CLIPNOISE() + [dup("base", "b2"), setp("set_opacity", "b2", 0.5)])
    g["DUP-05"] = script(K() + [U(), setp("set_visible", "u", False), dup("u", "u2")], equal_to="FIX-K")
    g["DUP-06"] = script(K() + [group("g"), solid("x", "#3050FFFF", [16, 16, 32, 32], "g"),
                                solid("u", "#E03020FF", [8, 8, 32, 32]), dup("u", "u2", "g"),
                                setp("set_visible", "u", False)])
    g["DUP-07"] = script(K() + [U()] + T_MUL() + [dup("u", "u2", "root")])
    g["DUP-08"] = script(K() + [adjustment("a", "posterize", {"levels": 3}), setp("set_fill", "a", 0.5),
                                dup("a", "a2"), undo(), dup("a", "a2"),
                                set_adj("a2", {"levels": 6})])
    g["DUP-09"] = script(K() + [BASE()] + CLIPNOISE() + [blend("c", "mul"), dup("c", "c2"),
                                                          blend("c2", "scrn")])
    grp_g = [group("g"), solid("c1", "#10E080FF", [8, 8, 32, 32], "g"), group("h", parent="g"),
             solid("d", "#E03020FF", [24, 24, 32, 32], "h")]
    err("err_dup_id_taken", K() + [U(), dup("u", "k")], "id-taken")
    err("err_dup_derived_taken", K() + [solid("g2/c1", "#FFFFFFFF", [0, 0, 8, 8])] + grp_g
        + [dup("g", "g2")], "derived-id-taken")
    err("err_dup_into_self", K() + grp_g + [dup("g", "g2", "g")], "parent-inside-source")
    err("err_dup_into_descendant", K() + grp_g + [dup("g", "g2", "h")], "parent-inside-source")
    err("err_dup_parent_not_group", K() + [U(), dup("u", "u2", "k")], "parent-kind")
    err("err_dup_root", K() + [dup("root", "r2")], "layer-root")
    err("err_dup_id_root", K() + [U(), dup("u", "root")], "id-root")
    err("err_dup_missing_id", K() + [U(), {"op": "duplicate_layer", "layer": "u"}], "missing-id")

    # ---- 10.4 set_adjustment
    g["ADJ-01R"] = script(K() + [adjustment("a", "levels", {"rgb": {"in_white": 200}})])
    g["ADJ-01"] = script(K() + [adjustment("a", "levels", {"rgb": {"in_black": 40, "gamma": 1.6}}),
                                set_adj("a", {"rgb": {"in_white": 200}})], equal_to="ADJ-01R")
    g["ADJ-02R"] = script(K() + [adjustment("a", "hue_saturation", {"colorize": True, "hue": 30.0})])
    g["ADJ-02"] = script(K() + [adjustment("a", "hue_saturation", {"colorize": True, "hue": 200.0,
                                                                  "saturation": 60.0, "lightness": 10.0}),
                                set_adj("a", {"colorize": True, "hue": 30.0})], equal_to="ADJ-02R")

    def adj03(level, then=None):
        ops = K() + [group("g"), solid("x", "#3050FFFF", [8, 8, 40, 40], "g"),
                     adjustment("a", "threshold", {"level": level}, parent="g"),
                     setp("set_clip", "a", True), setp("set_fill", "a", 0.5), blend("a", "over"),
                     setp("set_opacity", "a", 0.8),
                     {"op": "add_mask", "layer": "a", "fill": "gradient", "from": 0, "to": 255, "dir": "h"}]
        if then is not None:
            ops.append(set_adj("a", {"level": then}))
        return ops
    g["ADJ-03R"] = script(adj03(180))
    g["ADJ-03"] = script(adj03(100, 180), equal_to="ADJ-03R")
    g["ADJ-04R"] = script(K() + [adjustment("a", "posterize", {"levels": 3})])
    g["ADJ-04"] = script(K() + [adjustment("a", "posterize", {"levels": 3}), set_adj("a", {"levels": 6}),
                                undo()], equal_to="ADJ-04R")
    post = [adjustment("a", "posterize", {"levels": 3})]
    err("err_setadj_raster", K() + [set_adj("k", {"levels": 3})], "layer-kind")
    err("err_setadj_group", K() + [group("g"), set_adj("g", {})], "layer-kind")
    err("err_setadj_type_field", K() + post + [{"op": "set_adjustment", "layer": "a",
                                                "type": "posterize", "params": {"levels": 5}}],
        "unknown-field")
    err("err_setadj_range", K() + post + [set_adj("a", {"levels": 1})], "params-range")
    err("err_setadj_foreign_key", K() + [adjustment("a", "threshold", {"level": 100}),
                                         set_adj("a", {"levels": 5})], "params-unknown-key")
    err("err_setadj_levels_order", K() + [adjustment("a", "levels", {}),
                                          set_adj("a", {"rgb": {"in_black": 200, "in_white": 200}})],
        "params-levels-order")
    err("err_setadj_missing_params", K() + post + [{"op": "set_adjustment", "layer": "a"}],
        "missing-params")

    # ---- 10.5 set_group_mode
    gm01 = K() + [group("g"), solid("c", "#E03020FF", [16, 16, 32, 32], "g"), blend("c", "mul")]
    g["GM-01R"] = script(K() + [group("g", "isolated"), solid("c", "#E03020FF", [16, 16, 32, 32], "g"),
                                blend("c", "mul")])
    g["GM-01"] = script(gm01 + [gmode("g", "isolated")], equal_to="GM-01R")

    def gm02(mode):
        return K() + [group("g", mode), setp("set_opacity", "g", 0.6),
                      solid("c1", "#10E080FF", [8, 8, 32, 32], "g"),
                      solid("c2", "#E03020C0", [24, 24, 32, 32], "g")]
    g["GM-02R"] = script(gm02("scrn"))
    g["GM-02"] = script(gm02("scrn") + [gmode("g", "isolated")], equal_to="GM-02R")
    g["GM-03R"] = script(gm02("pass"))
    g["GM-03"] = script(gm02("scrn") + [gmode("g", "pass")], equal_to="GM-03R")
    g["GM-04R"] = script(gm01)
    g["GM-04"] = script(gm01 + [gmode("g", "isolated"), undo()], equal_to="GM-04R")
    err("err_gm_raster", K() + [gmode("k", "pass")], "layer-kind")
    err("err_gm_adjustment", K() + [adjustment("a", "invert"), gmode("a", "isolated")], "layer-kind")
    err("err_gm_mode_norm", K() + [group("g"), gmode("g", "norm")], "mode-value")
    err("err_gm_mode_missing", K() + [group("g"), {"op": "set_group_mode", "layer": "g"}], "missing-mode")
    err("err_gm_root", K() + [gmode("root", "pass")], "layer-root")

    # ---- 10.6 select_alpha
    fill = [fill_sel("L")]
    g["SAL-01"] = script(SRC() + [sel_alpha("src")] + fill)
    for n, mode in (("SAL-02", "add"), ("SAL-03", "subtract"), ("SAL-04", "intersect")):
        g[n] = script(SRC() + [sel_ellipse(4, 4, 40, 40), sel_alpha("src", mode)] + fill)
    g["SAL-05"] = script(SRC() + [layer("e"), sel_rect(0, 0, 16, 16), sel_alpha("e")] + fill)
    g["SAL-06"] = script(SRC() + [sel_rect(8, 8, 24, 24), sel_alpha("src"), undo()] + fill)
    g["SAL-07"] = script(SRC() + [sel_rect(8, 8, 24, 24), {"op": "deselect"}, sel_alpha("src"),
                                  {"op": "reselect"}] + fill)
    err("err_sal_group", SRC() + [group("g"), sel_alpha("g")], "layer-kind")
    err("err_sal_adjustment", SRC() + [adjustment("a", "invert"), sel_alpha("a")], "layer-kind")
    err("err_sal_mode", SRC() + [sel_alpha("src", "xor")], "mode-value")
    err("err_sal_root", SRC() + [sel_alpha("root")], "layer-root")

    # ---- 10.7 history across all six ops
    hsetup = K() + [solid("u", "#10E080FF", [8, 8, 32, 32]), group("g"),
                    solid("x", "#E03020FF", [20, 20, 32, 32], "g"), blend("x", "mul"),
                    adjustment("a", "posterize", {"levels": 3}, parent="g")]
    six = [set_name("u", "renamed"), dup("u", "u2"), set_adj("a", {"levels": 5}),
           gmode("g", "isolated"), sel_alpha("u"), delete("u")]
    hfill = [layer("L"), fill_sel("L", "#1060e0ff", 0.5)]
    g["HIST-01R"] = script(hsetup + hfill)
    g["HIST-01"] = script(hsetup + six + [undo(6)] + hfill, equal_to="HIST-01R")
    g["HIST-02R"] = script(hsetup + six[:3] + hfill)
    g["HIST-02"] = script(hsetup + six + [undo(3)] + hfill, equal_to="HIST-02R")

    return g, why, ascii_only


def main(argv):
    activate = "--activate" in argv[1:]
    unknown = [a for a in argv[1:] if a != "--activate"]
    if unknown:
        print(f"usage: {os.path.basename(argv[0])} [--activate]", file=sys.stderr)
        return 2
    scripts, why, ascii_only = build()
    for name in sorted(scripts):
        s = scripts[name]
        check(name, s, why.get(name))
        tgt = s.get("equal_to")
        if tgt is not None and tgt not in scripts:
            raise SystemExit(f"{name}: equal_to target {tgt} is not generated")
    out = OUT_ACTIVE if activate else OUT_PENDING
    os.makedirs(out, exist_ok=True)
    for f in os.listdir(out):
        if f.endswith(".json"):
            os.remove(os.path.join(out, f))
    for name in sorted(scripts):
        with open(os.path.join(out, name + ".json"), "w", encoding="utf-8", newline="\n") as fh:
            fh.write(dump_script(scripts[name], ascii_only=name in ascii_only))
    if activate and os.path.isdir(OUT_PENDING):
        shutil.rmtree(OUT_PENDING)
    n_err = sum(1 for s in scripts.values() if s.get("expect") == "error")
    print(f"editing: {len(scripts) - n_err} goldens + {n_err} error cases -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
