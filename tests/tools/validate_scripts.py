#!/usr/bin/env python3
"""validate_scripts.py - static check of render scripts against the doc-10 §11 grammar (plus the
doc-20 §A9 `add_adjustment` op), so a typo in a golden is found before any renderer runs.

    validate_scripts.py [PATH ...]      (files or directories; default tests/scripts/compositing)

Checks: script shape, every op's field names / types / ranges, fill-specific fields, colour and
rect syntax, id uniqueness and references, node kinds per op, move/merge preconditions that are
decidable from structure alone. Ops owned by other docs (20/30/40) other than add_adjustment are
reported as "not checked" rather than errors. Scripts with top-level "expect": "error" are
skipped (they are meant to be invalid). Exit 0 iff every checked script is valid.
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
MODES = {"norm", "diss", "dark", "mul", "idiv", "lbrn", "dkCl", "lite", "scrn", "div", "lddg",
         "lgCl", "over", "sLit", "hLit", "vLit", "lLit", "pLit", "hMix", "diff", "smud", "fsub",
         "fdiv", "hue", "sat", "colr", "lum"}
ADJ_TYPES = {"levels", "curves", "brightness_contrast", "hue_saturation", "black_white", "invert",
             "posterize", "threshold"}
COLOR_RE = re.compile(r"^#([0-9A-Fa-f]{6}|[0-9A-Fa-f]{8})$")
DOC10_OPS = {"add_layer", "add_group", "move_layer", "set_blend", "set_opacity", "set_fill",
             "set_visible", "set_clip", "set_clbl", "add_mask", "set_mask_enabled",
             "delete_mask", "apply_mask", "lock_transparency", "merge_down", "merge_visible",
             "flatten", "add_adjustment"}


class Bad(Exception):
    pass


def is_int(v):
    return isinstance(v, int) and not isinstance(v, bool)


def is_num(v):
    return (isinstance(v, (int, float))) and not isinstance(v, bool)


def need(cond, msg):
    if not cond:
        raise Bad(msg)


def check_color(v, f):
    need(isinstance(v, str) and COLOR_RE.match(v), f"{f}: bad colour {v!r}")


def check_rect(v, f):
    need(isinstance(v, list) and len(v) == 4 and all(is_int(x) for x in v), f"{f}: rect not 4 ints")
    need(v[2] >= 1 and v[3] >= 1, f"{f}: rect w,h must be >= 1")


def check_unit(v, f):
    need(is_num(v) and 0.0 <= v <= 1.0, f"{f}: need a number in [0,1], got {v!r}")


def check_byte(v, f):
    need(is_int(v) and 0 <= v <= 255, f"{f}: need int 0..255, got {v!r}")


def check_seed(v, f):
    need(is_int(v) and 0 <= v < 2 ** 53, f"{f}: need int seed in [0, 2^53)")


class Doc:
    def __init__(self):
        self.nodes = {"root": {"kind": "root", "children": [], "parent": None, "mask": False,
                               "clip": False, "visible": True}}

    def add(self, id_, kind, parent):
        need(isinstance(id_, str) and id_, "id must be a non-empty string")
        need(id_ != "root", "id 'root' is reserved")
        need(id_ not in self.nodes, f"duplicate id {id_!r}")
        need(parent in self.nodes, f"unknown parent {parent!r}")
        need(self.nodes[parent]["kind"] in ("root", "group"), f"parent {parent!r} not a group")
        self.nodes[id_] = {"kind": kind, "children": [], "parent": parent, "mask": False,
                           "clip": False, "visible": True}
        self.nodes[parent]["children"].append(id_)

    def get(self, id_, kinds):
        need(isinstance(id_, str) and id_ in self.nodes and id_ != "root",
             f"unknown layer {id_!r}")
        n = self.nodes[id_]
        need(n["kind"] in kinds, f"{id_!r} is a {n['kind']}, op needs {sorted(kinds)}")
        return n

    def remove_subtree(self, id_):
        n = self.nodes.pop(id_)
        for c in list(n["children"]):
            self.remove_subtree(c)


ALL = {"raster", "adjustment", "group"}
LAYERS = {"raster", "adjustment"}


def check_fields(op, allowed, required=()):
    extra = set(op) - set(allowed) - {"op"}
    need(not extra, f"{op['op']}: unknown field(s) {sorted(extra)}")
    for r in required:
        need(r in op, f"{op['op']}: missing required field {r!r}")


def check_op(doc: Doc, op: dict, W: int, H: int, notes: list):
    need(isinstance(op, dict) and isinstance(op.get("op"), str), "op must be an object with 'op'")
    name = op["op"]
    if name not in DOC10_OPS:
        notes.append(f"op {name!r} not checked (owned by another doc)")
        return
    if name in ("add_layer", "add_adjustment"):
        if name == "add_adjustment":
            check_fields(op, {"id", "parent", "type", "params"}, ("id", "type"))
            need(op["type"] in ADJ_TYPES, f"unknown adjustment type {op['type']!r}")
            if "params" in op:
                need(isinstance(op["params"], dict), "params must be an object")
                if op["type"] == "invert":
                    need(op["params"] == {}, "invert takes no params")
            doc.add(op["id"], "adjustment", op.get("parent", "root"))
            return
        fill = op.get("fill", "empty")
        per_fill = {"empty": set(), "solid": {"color"}, "gradient": {"from", "to", "dir"},
                    "checker": {"a", "b", "cell"}, "noise": {"seed", "alpha"}}
        need(fill in per_fill, f"add_layer: bad fill {fill!r}")
        check_fields(op, {"id", "parent", "fill", "rect"} | per_fill[fill], ("id",))
        for f in ("color", "from", "to", "a", "b"):
            if f in op:
                check_color(op[f], f)
        if "dir" in op:
            need(op["dir"] in ("h", "v"), "dir must be h|v")
        if "cell" in op:
            need(is_int(op["cell"]) and 1 <= op["cell"] <= 4096, "cell must be int 1..4096")
        if "seed" in op:
            check_seed(op["seed"], "seed")
        if "alpha" in op:
            need(op["alpha"] == "random" or (is_int(op["alpha"]) and 0 <= op["alpha"] <= 255),
                 "alpha must be 'random' or int 0..255")
        if "rect" in op:
            check_rect(op["rect"], "rect")
        doc.add(op["id"], "raster", op.get("parent", "root"))
    elif name == "add_group":
        check_fields(op, {"id", "parent", "mode"}, ("id",))
        mode = op.get("mode", "pass")
        need(mode in MODES or mode in ("pass", "isolated"), f"bad group mode {mode!r}")
        doc.add(op["id"], "group", op.get("parent", "root"))
    elif name == "move_layer":
        check_fields(op, {"id", "parent", "index"}, ("id",))
        n = doc.get(op["id"], ALL)
        par = op.get("parent", n["parent"])
        need(par in doc.nodes and doc.nodes[par]["kind"] in ("root", "group"), "bad parent")
        p = par
        while p is not None:
            need(p != op["id"], "moving a group into itself/descendant")
            p = doc.nodes[p]["parent"]
        doc.nodes[n["parent"]]["children"].remove(op["id"])
        kids = doc.nodes[par]["children"]
        idx = op.get("index", len(kids))
        need(is_int(idx) and 0 <= idx <= len(kids), "index out of range")
        kids.insert(idx, op["id"])
        n["parent"] = par
    elif name == "set_blend":
        check_fields(op, {"layer", "mode", "seed"}, ("layer", "mode"))
        n = doc.get(op["layer"], ALL)
        m = op["mode"]
        if n["kind"] == "group":
            need(m in MODES or m in ("pass", "isolated"), f"bad mode {m!r}")
        else:
            need(m in MODES, f"bad layer mode {m!r}")
        if "seed" in op:
            check_seed(op["seed"], "seed")
    elif name in ("set_opacity", "set_fill"):
        check_fields(op, {"layer", "value"}, ("layer", "value"))
        doc.get(op["layer"], ALL if name == "set_opacity" else LAYERS)
        check_unit(op["value"], name)
    elif name in ("set_visible", "set_clip", "set_clbl", "set_mask_enabled", "lock_transparency"):
        req = ("layer",) if name == "lock_transparency" else ("layer", "value")
        check_fields(op, {"layer", "value"}, req)
        kinds = {"set_visible": ALL, "set_clip": LAYERS, "set_clbl": LAYERS,
                 "set_mask_enabled": ALL, "lock_transparency": {"raster"}}[name]
        n = doc.get(op["layer"], kinds)
        if "value" in op:
            need(isinstance(op["value"], bool), f"{name}: value must be bool")
        if name == "set_mask_enabled":
            need(n["mask"], "set_mask_enabled: no mask")
        if name == "set_clip":
            n["clip"] = op["value"]
        if name == "set_visible":
            n["visible"] = op["value"]
    elif name == "add_mask":
        fill = op.get("fill", "solid")
        per_fill = {"solid": {"value"}, "gradient": {"from", "to", "dir"}, "noise": {"seed"}}
        need(fill in per_fill, f"add_mask: bad fill {fill!r}")
        check_fields(op, {"layer", "fill", "rect", "outside"} | per_fill[fill], ("layer",))
        n = doc.get(op["layer"], ALL)
        for f in ("value", "from", "to", "outside"):
            if f in op:
                check_byte(op[f], f)
        if "dir" in op:
            need(op["dir"] in ("h", "v"), "dir must be h|v")
        if "seed" in op:
            check_seed(op["seed"], "seed")
        if "rect" in op:
            check_rect(op["rect"], "rect")
        n["mask"] = True
    elif name in ("delete_mask", "apply_mask"):
        check_fields(op, {"layer"}, ("layer",))
        n = doc.get(op["layer"], ALL if name == "delete_mask" else {"raster"})
        need(n["mask"], f"{name}: no mask")
        n["mask"] = False
    elif name == "merge_down":
        check_fields(op, {"layer"}, ("layer",))
        u = doc.get(op["layer"], LAYERS)
        kids = doc.nodes[u["parent"]]["children"]
        i = kids.index(op["layer"])
        need(i > 0, "merge_down: U at index 0")
        lid = kids[i - 1]
        low = doc.nodes[lid]
        need(low["kind"] == "raster", "merge_down: L not raster")
        need(u["visible"] and low["visible"], "merge_down: both must be visible")
        if i + 1 < len(kids):
            need(not doc.nodes[kids[i + 1]]["clip"], "merge_down: node above U is clipped")
        kids.remove(op["layer"])
        doc.remove_subtree(op["layer"])
        low["mask"] = False
    elif name in ("merge_visible", "flatten"):
        check_fields(op, {"id"})
        nid = op.get("id", "merged" if name == "merge_visible" else "flattened")
        root = doc.nodes["root"]["children"]
        if name == "merge_visible":
            vis = [c for c in root if doc.nodes[c]["visible"]]
            if not vis:
                return
            for c in vis:
                root.remove(c)
                doc.remove_subtree(c)
        else:
            for c in list(root):
                root.remove(c)
                doc.remove_subtree(c)
        doc.add(nid, "raster", "root")
    else:  # pragma: no cover
        raise Bad(f"unhandled op {name}")


def check_script(obj) -> list[str]:
    notes: list[str] = []
    need(isinstance(obj, dict), "script must be an object")
    extra = set(obj) - {"canvas", "ops", "out"}
    need(not extra, f"unknown top-level field(s) {sorted(extra)}")
    need("canvas" in obj and "ops" in obj, "missing canvas/ops")
    c = obj["canvas"]
    need(isinstance(c, dict) and not (set(c) - {"w", "h", "bg"}), "bad canvas fields")
    need(is_int(c.get("w")) and 1 <= c["w"] <= 16384, "canvas.w must be int 1..16384")
    need(is_int(c.get("h")) and 1 <= c["h"] <= 16384, "canvas.h must be int 1..16384")
    if "bg" in c:
        check_color(c["bg"], "canvas.bg")
    need(obj.get("out", "png8") == "png8", "out must be png8")
    need(isinstance(obj["ops"], list), "ops must be a list")
    doc = Doc()
    for i, op in enumerate(obj["ops"]):
        try:
            check_op(doc, op, c["w"], c["h"], notes)
        except Bad as e:
            raise Bad(f"op #{i} ({op.get('op') if isinstance(op, dict) else op!r}): {e}")
    return notes


def main(argv=None) -> int:
    args = (argv if argv is not None else sys.argv[1:]) or [str(REPO / "tests/scripts/compositing")]
    files = []
    for a in args:
        p = Path(a)
        # "_*.json" files are harness manifests (e.g. stats/_expectations.json), not scripts.
        files += sorted(f for f in p.rglob("*.json") if not f.name.startswith("_")) \
            if p.is_dir() else [p]
    bad = checked = skipped = 0
    for f in files:
        try:
            obj = json.loads(f.read_text())
        except Exception as e:  # noqa: BLE001
            print(f"INVALID {f}: not JSON: {e}")
            bad += 1
            continue
        if isinstance(obj, dict) and obj.get("expect") == "error":
            skipped += 1
            continue
        if isinstance(obj, dict):
            obj.pop("equal_to", None)  # harness key (tests/scripts/README.md), not grammar
        checked += 1
        try:
            for n in check_script(obj):
                print(f"note {f.name}: {n}")
        except Bad as e:
            print(f"INVALID {f}: {e}")
            bad += 1
    print(f"validate_scripts: {checked} checked, {bad} invalid, {skipped} expect-error skipped")
    return 1 if bad or not checked else 0


if __name__ == "__main__":
    sys.exit(main())
