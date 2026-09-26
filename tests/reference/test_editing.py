#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Self-checks for the doc 60 reference module (rc/ops_editing.py).

Run:  python3 tests/reference/test_editing.py [--goldens OUTDIR]
  or: python3 -m pytest tests/reference/test_editing.py

Expected values are worked out by hand from docs/math/60-editing-ops.md (arithmetic in the
comments); none was produced by running the reference. The golden pass renders every doc-60
script (tests/scripts/editing/, or tests/scripts/.pending-editing/ while the set is staged),
checks that each `expect: error` script renders fine without its last op and fails with it (so it
fails at the op it targets), checks every `equal_to` pair, and runs two script-level mutation
probes for the doc 60 §11 hooks 40 and 41 (the goldens must tell the mutant from the real op).
"""

import copy
import glob
import json
import os
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from rc import ops_editing as E  # noqa: E402
from rc.composite import render_document  # noqa: E402
from rc.core import ScriptError  # noqa: E402
from rc.fields import FieldReader  # noqa: E402
from rc.registry import OPS, load_all  # noqa: E402
from rc.runner import run_script  # noqa: E402

load_all()

SCRIPTS = os.path.join(HERE, "..", "scripts")
GOLDEN_DIR = os.path.join(SCRIPTS, "editing")
PENDING_DIR = os.path.join(SCRIPTS, ".pending-editing")
HARNESS_KEYS = ("expect", "equal_to")


def script(ops, w=4, h=1, bg="#00000000"):
    return {"canvas": {"w": w, "h": h, "bg": bg}, "ops": ops, "out": "png8"}


def doc_of(ops, **kw):
    return run_script(json.loads(json.dumps(script(ops, **kw))))


def expect_error(ops, **kw):
    try:
        doc_of(ops, **kw)
    except ScriptError:
        return
    raise AssertionError("expected a script error for %r" % (ops[-1],))


def ids(container):
    return [n.id for n in container.children]


def apply_op(doc, obj):
    """Run one op handler directly on `doc` (no history), with the runner's unknown-field check."""
    o = dict(obj)
    name = o.pop("op")
    f = FieldReader(o, "ops[x] (%s)" % name)
    OPS[name].handler(doc, f)
    f.check_unused()


def solid(i, color, rect=None, **kw):
    d = {"op": "add_layer", "id": i, "fill": "solid", "color": color}
    if rect is not None:
        d["rect"] = rect
    d.update(kw)
    return d


# ---------------------------------------------------------------------------------------------
# §1.1 names
# ---------------------------------------------------------------------------------------------

def test_name_rules():
    ok = ["", " a  b ", "Sky – warm", "é" * 254 + "\U0001D11E", "x" * 255]
    bad = ["x" * 256, "a\nb", "\x00", "\x1f", "a\x7fb", "\ud800x", "\udc00", "\udd1e\ud834",
           "é" * 255 + "\U0001D11E"]
    for s in ok:
        assert E.name_problem(s) is None, repr(s)
    for s in bad:
        assert E.name_problem(s) is not None, repr(s)
    # U+0080 (C1) and U+00A0 are not in the forbidden set of §1.1 rule 2
    assert E.name_problem("\x80\xa0") is None
    # 254 x é + 𝄞 = 255 code points although it is 256 UTF-16 units and 510 + 4 bytes
    s = "é" * 254 + "\U0001D11E"
    assert len(s) == 255 and len(s.encode("utf-16-le")) // 2 == 256
    assert len(s.encode("utf-8")) == 512


def test_set_name_and_display():
    d = doc_of([solid("u", "#FF0000FF")])
    u = d.find("u")
    assert E.node_name(u) == "" and E.display_name(u) == "u"
    apply_op(d, {"op": "set_name", "layer": "u", "name": "  k "})
    assert u.name == "  k " and E.display_name(u) == "  k "   # stored as decoded, no trimming
    apply_op(d, {"op": "set_name", "layer": "u", "name": ""})
    assert E.display_name(u) == "u"
    # a name is not an address: "k" (the name) is an unknown id here
    apply_op(d, {"op": "set_name", "layer": "u", "name": "k"})
    try:
        apply_op(d, {"op": "set_opacity", "layer": "k", "value": 0.5})
        raise AssertionError("names must not be addressable")
    except ScriptError:
        pass
    # any node kind, and undo restores the previous name (one record)
    d = doc_of([{"op": "add_group", "id": "g"},
                {"op": "add_adjustment", "id": "a", "type": "invert", "parent": "g"},
                {"op": "set_name", "layer": "g", "name": "G"},
                {"op": "set_name", "layer": "a", "name": "A"},
                {"op": "set_name", "layer": "g", "name": "G2"},
                {"op": "undo"}])
    assert d.find("g").name == "G" and d.find("a").name == "A"


def test_set_name_errors():
    base = [solid("u", "#FF0000FF")]
    expect_error(base + [{"op": "set_name", "layer": "u", "name": "x" * 256}])
    expect_error(base + [{"op": "set_name", "layer": "u", "name": "a\tb"}])
    expect_error(base + [{"op": "set_name", "layer": "u", "name": "\x7f"}])
    expect_error(base + [{"op": "set_name", "layer": "u", "name": "\ud800"}])
    expect_error(base + [{"op": "set_name", "layer": "u", "name": 3}])
    expect_error(base + [{"op": "set_name", "layer": "u"}])
    expect_error(base + [{"op": "set_name", "name": "x"}])
    expect_error(base + [{"op": "set_name", "layer": "root", "name": "x"}])
    expect_error(base + [{"op": "set_name", "layer": "nope", "name": "x"}])
    expect_error(base + [{"op": "set_name", "layer": "u", "name": "x", "id": "u"}])


# ---------------------------------------------------------------------------------------------
# §2 delete_layer
# ---------------------------------------------------------------------------------------------

def test_delete_structure_and_flags():
    # b0 covers x = 0 only; base covers everything; c (clipped) covers everything
    d = doc_of([solid("b0", "#00FF00FF", [0, 0, 1, 1]), solid("base", "#FF0000FF"),
                solid("c", "#0000FFFF"), {"op": "set_clip", "layer": "c", "value": True},
                {"op": "add_group", "id": "g"}, solid("x", "#FFFFFFFF", [3, 0, 1, 1], parent="g"),
                {"op": "delete_layer", "layer": "base"}])
    assert ids(d.root) == ["b0", "c", "g"]
    assert d.find("c").clip is True            # flag kept, never released
    assert d.find("base") is None
    # c now clips to b0 (§2 first bullet): c's colour where b0 has alpha, nothing elsewhere;
    # the group above is untouched (x white at x = 3)
    out = render_document(d, onto_bg=True)
    assert out[0].tolist() == [[0, 0, 255, 255], [0, 0, 0, 0], [0, 0, 0, 0], [255, 255, 255, 255]]
    # no raster below the clipped layer (a group is): c renders unclipped (§2 third bullet)
    d = doc_of([{"op": "add_group", "id": "g"}, solid("base", "#FF0000FF", [0, 0, 1, 1]),
                solid("c", "#0000FFFF", [0, 0, 2, 1]), {"op": "set_clip", "layer": "c", "value": True},
                {"op": "delete_layer", "layer": "base"}])
    assert render_document(d, onto_bg=True)[0, :, 3].tolist() == [255, 255, 0, 0]

    # a group is deleted with its contents; ids are freed; undo restores them
    d = doc_of([{"op": "add_group", "id": "g"}, solid("x", "#FFFFFFFF", parent="g"),
                {"op": "add_group", "id": "h", "parent": "g"}, solid("y", "#FF00FFFF", parent="h"),
                {"op": "delete_layer", "layer": "g"}])
    assert d.root.children == [] and d.find("x") is None and d.find("y") is None
    out = render_document(d, onto_bg=True)
    assert not out.any()                       # empty root renders bg #00000000
    d = doc_of([{"op": "add_group", "id": "g"}, solid("x", "#FFFFFFFF", parent="g"),
                {"op": "delete_layer", "layer": "g"},
                {"op": "add_layer", "id": "x"}, {"op": "add_group", "id": "g"}])
    assert ids(d.root) == ["x", "g"]
    d = doc_of([{"op": "add_group", "id": "g"}, solid("x", "#FFFFFFFF", parent="g"),
                {"op": "delete_layer", "layer": "g"}, {"op": "undo"}])
    assert ids(d.root) == ["g"] and ids(d.find("g")) == ["x"]
    # selection and Saved survive
    d = doc_of([solid("u", "#FFFFFFFF"),
                {"op": "select_rect", "x": 1, "y": 0, "w": 2, "h": 1}, {"op": "deselect"},
                {"op": "select_rect", "x": 0, "y": 0, "w": 1, "h": 1},
                {"op": "delete_layer", "layer": "u"}])
    assert d.sel.tolist() == [[255, 0, 0, 0]] and d.sel_saved.tolist() == [[0, 255, 255, 0]]


def test_delete_errors():
    expect_error([solid("u", "#FFFFFFFF"), {"op": "delete_layer", "layer": "root"}])
    expect_error([solid("u", "#FFFFFFFF"), {"op": "delete_layer", "layer": "v"}])
    expect_error([solid("u", "#FFFFFFFF"), {"op": "delete_layer"}])
    expect_error([solid("u", "#FFFFFFFF"), {"op": "delete_layer", "layer": 1}])
    expect_error([solid("u", "#FFFFFFFF"), {"op": "delete_layer", "layer": "u", "id": "u"}])
    expect_error([solid("u", "#FFFFFFFF"), {"op": "delete_layer", "layer": "u"},
                  {"op": "delete_layer", "layer": "u"}])


# ---------------------------------------------------------------------------------------------
# §4 duplicate_layer
# ---------------------------------------------------------------------------------------------

GROUP_SETUP = [solid("k", "#202020FF"),
               {"op": "add_group", "id": "g", "mode": "scrn"},
               solid("c1", "#10E080FF", parent="g"),
               {"op": "add_group", "id": "h", "parent": "g", "mode": "isolated"},
               solid("d", "#E03020FF", parent="h"),
               solid("t", "#FFFFFFFF")]


def test_duplicate_placement_and_ids():
    # no parent: index i + 1 (directly above the source, under t)
    d = doc_of(GROUP_SETUP + [{"op": "duplicate_layer", "layer": "k", "id": "k2"}])
    assert ids(d.root) == ["k", "k2", "g", "t"]
    # parent = own container: top
    d = doc_of(GROUP_SETUP + [{"op": "duplicate_layer", "layer": "k", "id": "k2",
                               "parent": "root"}])
    assert ids(d.root) == ["k", "g", "t", "k2"]
    # parent = a group: top of that group
    d = doc_of(GROUP_SETUP + [{"op": "duplicate_layer", "layer": "t", "id": "t2", "parent": "h"}])
    assert ids(d.find("h")) == ["d", "t2"]
    # group copy: flat derived ids (g2/d, not g2/h/d), same order, deep structure
    d = doc_of(GROUP_SETUP + [{"op": "duplicate_layer", "layer": "g", "id": "g2"}])
    assert ids(d.root) == ["k", "g", "g2", "t"]
    g2 = d.find("g2")
    assert ids(g2) == ["g2/c1", "g2/h"] and ids(d.find("g2/h")) == ["g2/d"]
    assert g2.mode == "scrn" and d.find("g2/h").mode == "norm"
    # derived ids of a copy of a copy. The §4.3 formula is id + "/" + d.id with d the source's
    # descendant, and g2's descendants are g2/c1, g2/h, g2/d, so the formula gives g3/g2/c1 ...
    # The sentence at the end of §4.3 says "g3/c1, g3/h, g3/d" instead; that example contradicts
    # the formula and is reported to the lead. The reference follows the formula. No golden
    # duplicates a duplicated group, so the choice does not reach any golden.
    d = doc_of(GROUP_SETUP + [{"op": "duplicate_layer", "layer": "g", "id": "g2"},
                              {"op": "duplicate_layer", "layer": "g2", "id": "g3"}])
    assert ids(d.find("g3")) == ["g3/g2/c1", "g3/g2/h"] and ids(d.find("g3/g2/h")) == ["g3/g2/d"]


def test_duplicate_names():
    d = doc_of(GROUP_SETUP + [{"op": "set_name", "layer": "c1", "name": "Leaf"},
                              {"op": "duplicate_layer", "layer": "g", "id": "g2"}])
    assert d.find("g2").name == "g copy"          # display_name(g) = "g"
    assert d.find("g2/c1").name == "Leaf"         # child keeps its label
    assert d.find("g2/h").name == "h"             # unnamed child: its original id as label
    assert d.find("g2/d").name == "d"
    assert E.node_name(d.find("g")) == ""        # the source is untouched
    # 250 + " copy" = 255 code points: kept; 251 + " copy" = 256: ""
    d = doc_of([solid("u", "#FFFFFFFF"), {"op": "set_name", "layer": "u", "name": "a" * 250},
                {"op": "duplicate_layer", "layer": "u", "id": "u2"},
                {"op": "set_name", "layer": "u", "name": "b" * 251},
                {"op": "duplicate_layer", "layer": "u", "id": "u3"}])
    assert d.find("u2").name == "a" * 250 + " copy" and d.find("u3").name == ""
    # an id with a control code point cannot become a name
    d = doc_of([solid("u\n", "#FFFFFFFF"), {"op": "add_group", "id": "g"},
                solid("c\x7f", "#FFFFFFFF", parent="g"), solid("v" * 300, "#FFFFFFFF", parent="g"),
                {"op": "duplicate_layer", "layer": "u\n", "id": "u2"},
                {"op": "duplicate_layer", "layer": "g", "id": "g2"}])
    assert d.find("u2").name == "" and d.find("g2/c\x7f").name == ""
    assert d.find("g2/" + "v" * 300).name == ""


def test_duplicate_copies_everything_independently():
    d = doc_of([{"op": "add_layer", "id": "u", "fill": "noise", "seed": 5, "alpha": 255},
                {"op": "add_mask", "layer": "u", "fill": "solid", "value": 200, "rect": [1, 0, 2, 1],
                 "outside": 30},
                {"op": "set_mask_enabled", "layer": "u", "value": False},
                {"op": "set_blend", "layer": "u", "mode": "diss"},
                {"op": "set_fill", "layer": "u", "value": 0.6},
                {"op": "set_opacity", "layer": "u", "value": 0.7},
                {"op": "set_visible", "layer": "u", "value": False},
                {"op": "set_clbl", "layer": "u", "value": False},
                {"op": "lock_transparency", "layer": "u", "value": True},
                {"op": "duplicate_layer", "layer": "u", "id": "u2"}])
    u, u2 = d.find("u"), d.find("u2")
    for attr in ("visible", "opacity", "mode", "seed", "fill", "clip", "clbl", "lock_alpha"):
        assert getattr(u, attr) == getattr(u2, attr), attr
    assert np.array_equal(u.rgba, u2.rgba) and u.rgba is not u2.rgba
    assert np.array_equal(u.mask.data, u2.mask.data) and u.mask.data is not u2.mask.data
    assert u2.mask.enabled is False and u2.mask.outside == u.mask.outside
    before = u.rgba.copy()
    u2.rgba[:] = 0
    u2.mask.data[:] = 0
    assert np.array_equal(u.rgba, before) and u.mask.data.tolist() == [[30, 200, 200, 30]]
    # adjustment params independent (set_adjustment on the copy leaves the source)
    d = doc_of([{"op": "add_adjustment", "id": "a", "type": "posterize", "params": {"levels": 3}},
                {"op": "duplicate_layer", "layer": "a", "id": "a2"},
                {"op": "set_adjustment", "layer": "a2", "params": {"levels": 6}}])
    assert d.find("a").params != d.find("a2").params
    assert d.find("a").adj_type == d.find("a2").adj_type == "posterize"


def test_duplicate_errors_leave_doc_unchanged():
    d = doc_of(GROUP_SETUP + [solid("g2/h", "#FFFFFFFF")])
    snap = copy.deepcopy(d)
    for bad in ({"op": "duplicate_layer", "layer": "g", "id": "g2"},          # derived g2/h taken
                {"op": "duplicate_layer", "layer": "g", "id": "t"},
                {"op": "duplicate_layer", "layer": "g", "id": "root"},
                {"op": "duplicate_layer", "layer": "g", "id": ""},
                {"op": "duplicate_layer", "layer": "g", "id": 7},
                {"op": "duplicate_layer", "layer": "g"},
                {"op": "duplicate_layer", "layer": "root", "id": "r2"},
                {"op": "duplicate_layer", "layer": "g", "id": "g9", "parent": "g"},
                {"op": "duplicate_layer", "layer": "g", "id": "g9", "parent": "h"},
                {"op": "duplicate_layer", "layer": "g", "id": "g9", "parent": "t"},
                {"op": "duplicate_layer", "layer": "g", "id": "g9", "parent": "nope"},
                {"op": "duplicate_layer", "layer": "g", "id": "g9", "parent": 1},
                {"op": "duplicate_layer", "layer": "g", "id": "g9", "index": 0}):
        try:
            apply_op(d, bad)
            raise AssertionError("expected an error for %r" % bad)
        except ScriptError:
            pass
        assert [n.id for n in d.iter_nodes()] == [n.id for n in snap.iter_nodes()], bad
    # a raster source may be duplicated into a sibling group (parent not inside a raster)
    apply_op(d, {"op": "duplicate_layer", "layer": "t", "id": "t2", "parent": "g"})
    assert ids(d.find("g"))[-1] == "t2"


# ---------------------------------------------------------------------------------------------
# §5 set_adjustment, §6 set_group_mode
# ---------------------------------------------------------------------------------------------

def test_set_adjustment_replaces():
    d = doc_of([{"op": "add_adjustment", "id": "a", "type": "levels",
                 "params": {"rgb": {"in_black": 40, "gamma": 1.6}}},
                {"op": "set_opacity", "layer": "a", "value": 0.5},
                {"op": "set_name", "layer": "a", "name": "L"},
                {"op": "set_adjustment", "layer": "a", "params": {"rgb": {"in_white": 200}}},
                {"op": "add_adjustment", "id": "b", "type": "levels",
                 "params": {"rgb": {"in_white": 200}}},
                {"op": "add_adjustment", "id": "c", "type": "levels"},
                {"op": "set_adjustment", "layer": "c", "params": {}},
                {"op": "add_adjustment", "id": "e", "type": "levels", "params": {}}])
    assert d.find("a").params == d.find("b").params
    assert d.find("c").params == d.find("e").params
    assert d.find("a").opacity == 0.5 and d.find("a").name == "L"
    assert ids(d.root) == ["a", "b", "c", "e"]
    # colorize-dependent default on replace (A4): saturation back to 25
    d = doc_of([{"op": "add_adjustment", "id": "a", "type": "hue_saturation",
                 "params": {"colorize": True, "hue": 200, "saturation": 60, "lightness": 10}},
                {"op": "set_adjustment", "layer": "a", "params": {"colorize": True, "hue": 30}},
                {"op": "add_adjustment", "id": "b", "type": "hue_saturation",
                 "params": {"colorize": True, "hue": 30}}])
    assert d.find("a").params == d.find("b").params
    base = [{"op": "add_adjustment", "id": "a", "type": "posterize", "params": {"levels": 3}},
            {"op": "add_group", "id": "g"}, solid("u", "#FFFFFFFF")]
    expect_error(base + [{"op": "set_adjustment", "layer": "a", "params": {"levels": 1}}])
    expect_error(base + [{"op": "set_adjustment", "layer": "a", "params": {"level": 100}}])
    expect_error(base + [{"op": "set_adjustment", "layer": "a", "type": "posterize",
                          "params": {"levels": 4}}])
    expect_error(base + [{"op": "set_adjustment", "layer": "a"}])
    expect_error(base + [{"op": "set_adjustment", "layer": "a", "params": [4]}])
    expect_error(base + [{"op": "set_adjustment", "layer": "g", "params": {}}])
    expect_error(base + [{"op": "set_adjustment", "layer": "u", "params": {}}])
    expect_error(base + [{"op": "set_adjustment", "layer": "root", "params": {}}])


def test_set_group_mode():
    def mode_after(create_mode, *modes):
        ops = [{"op": "add_group", "id": "g", "mode": create_mode}]
        ops += [{"op": "set_group_mode", "layer": "g", "mode": m} for m in modes]
        return doc_of(ops).find("g").mode
    assert mode_after("pass", "isolated") == "norm"
    assert mode_after("scrn", "isolated") == "scrn"
    assert mode_after("scrn", "pass") == "pass"
    assert mode_after("scrn", "pass", "isolated") == "norm"   # no memory of Screen
    assert mode_after("isolated", "isolated") == "norm"
    assert mode_after("pass", "pass") == "pass"
    d = doc_of([{"op": "add_group", "id": "g"},
                {"op": "set_group_mode", "layer": "g", "mode": "isolated"}, {"op": "undo"}])
    assert d.find("g").mode == "pass"
    base = [{"op": "add_group", "id": "g"}, solid("u", "#FFFFFFFF"),
            {"op": "add_adjustment", "id": "a", "type": "invert"}]
    for bad in ({"layer": "g", "mode": "norm"}, {"layer": "g", "mode": "scrn"}, {"layer": "g"},
                {"layer": "g", "mode": True}, {"layer": "u", "mode": "pass"},
                {"layer": "a", "mode": "pass"}, {"layer": "root", "mode": "pass"},
                {"layer": "g", "mode": "pass", "opacity": 1}):
        expect_error(base + [dict(op="set_group_mode", **bad)])


# ---------------------------------------------------------------------------------------------
# §7 select_alpha
# ---------------------------------------------------------------------------------------------

def test_combine_hand():
    S = np.array([[0, 100, 200, 255]], np.uint8)
    B = np.array([[50, 150, 100, 0]], np.uint8)
    assert E.combine_selection(S, B, "new").tolist() == [[50, 150, 100, 0]]
    assert E.combine_selection(S, B, "add").tolist() == [[50, 150, 200, 255]]
    assert E.combine_selection(S, B, "subtract").tolist() == [[0, 0, 100, 255]]
    assert E.combine_selection(S, B, "intersect").tolist() == [[0, 100, 100, 0]]


def test_select_alpha_ops():
    # 4x1: src alpha = [128, 128, 0, 0] (solid #FF000080 rect [0,0,2,1]); hidden, masked to 0,
    # opacity 0.1, fill 0.2, Multiply, locked: none of it matters (§7)
    setup = [solid("src", "#FF000080", [0, 0, 2, 1]),
             {"op": "add_mask", "layer": "src", "fill": "solid", "value": 0},
             {"op": "set_visible", "layer": "src", "value": False},
             {"op": "set_opacity", "layer": "src", "value": 0.1},
             {"op": "set_fill", "layer": "src", "value": 0.2},
             {"op": "set_blend", "layer": "src", "mode": "mul"},
             {"op": "lock_transparency", "layer": "src", "value": True},
             {"op": "select_rect", "x": 1, "y": 0, "w": 2, "h": 1}]      # S = [0, 255, 255, 0]
    want = {"new": [128, 128, 0, 0], "add": [128, 255, 255, 0],
            "subtract": [0, 127, 255, 0], "intersect": [0, 128, 0, 0]}
    for mode, row in want.items():
        d = doc_of(setup + [{"op": "select_alpha", "layer": "src", "mode": mode}])
        assert d.sel.tolist() == [row], (mode, d.sel.tolist())
    d = doc_of(setup + [{"op": "select_alpha", "layer": "src"}])
    assert d.sel.tolist() == [[128, 128, 0, 0]]                          # default "new"
    # Saved untouched; undo restores S
    d = doc_of(setup + [{"op": "deselect"}, {"op": "select_alpha", "layer": "src"}])
    assert d.sel_saved.tolist() == [[0, 255, 255, 0]]
    d = doc_of(setup + [{"op": "select_alpha", "layer": "src"}, {"op": "undo"}])
    assert d.sel.tolist() == [[0, 255, 255, 0]]
    # empty source + intersect -> all zero -> no selection
    d = doc_of(setup + [{"op": "add_layer", "id": "e"},
                        {"op": "select_alpha", "layer": "e", "mode": "intersect"}])
    assert d.selection_is_empty()
    base = setup + [{"op": "add_group", "id": "g"},
                    {"op": "add_adjustment", "id": "a", "type": "invert"}]
    for bad in ({"layer": "g"}, {"layer": "a"}, {"layer": "root"}, {"layer": "zz"}, {},
                {"layer": "src", "mode": "xor"}, {"layer": "src", "mode": 1},
                {"layer": "src", "feather": 0}):
        expect_error(base + [dict(op="select_alpha", **bad)])


# ---------------------------------------------------------------------------------------------
# golden pass
# ---------------------------------------------------------------------------------------------

def golden_dir():
    if os.path.isdir(GOLDEN_DIR) and glob.glob(os.path.join(GOLDEN_DIR, "*.json")):
        return GOLDEN_DIR
    return PENDING_DIR


def _load(path):
    with open(path, "rb") as fh:
        return json.loads(fh.read().decode("utf-8"))


def _strip(js):
    return {k: v for k, v in js.items() if k not in HARNESS_KEYS}


def _render(js):
    return render_document(run_script(copy.deepcopy(js)), onto_bg=True)


def render_goldens(outdir=None):
    results, timings, failures, scripts = {}, {}, [], {}
    for path in sorted(glob.glob(os.path.join(golden_dir(), "*.json"))):
        stem = os.path.basename(path)[:-5]
        js = _load(path)
        scripts[stem] = js
        s = _strip(js)
        t0 = time.perf_counter()
        try:
            results[stem] = _render(s)
        except ScriptError as e:
            results[stem] = e
        timings[stem] = time.perf_counter() - t0
        if js.get("expect") == "error":
            if not isinstance(results[stem], ScriptError):
                failures.append("%s: expected a script error, rendered" % stem)
                continue
            head = dict(s, ops=s["ops"][:-1])      # must fail AT the last op, not before
            try:
                _render(head)
            except ScriptError as e:
                failures.append("%s: fails before its target op (%s)" % (stem, e))
        elif isinstance(results[stem], ScriptError):
            failures.append("%s: %s" % (stem, results[stem]))
        elif outdir:
            from PIL import Image
            os.makedirs(outdir, exist_ok=True)
            Image.fromarray(results[stem], "RGBA").save(os.path.join(outdir, stem + ".png"))
    for stem, js in scripts.items():
        tgt = js.get("equal_to")
        if tgt is None:
            continue
        a, b = results.get(stem), results.get(tgt)
        if not (isinstance(a, np.ndarray) and isinstance(b, np.ndarray) and np.array_equal(a, b)):
            failures.append("%s: not byte-equal to its equal_to target %s" % (stem, tgt))
    return results, scripts, timings, failures


def _deep_merge(old, new):
    out = copy.deepcopy(old)
    for k, v in new.items():
        if isinstance(v, dict) and isinstance(out.get(k), dict):
            out[k] = _deep_merge(out[k], v)
        else:
            out[k] = copy.deepcopy(v)
    return out


def mutation_probes(results, scripts):
    """doc 60 §11 hooks, applied to the scripts: each mutant render must differ from the golden."""
    failures = []
    # 40: duplicate without parent goes to the top of the source's container
    for stem in ("DUP-01", "DUP-04"):
        s = _strip(scripts[stem])
        d = run_script(copy.deepcopy(dict(s, ops=[])))
        ops = copy.deepcopy(s["ops"])
        for i, o in enumerate(ops):
            if o["op"] == "duplicate_layer" and "parent" not in o:
                d = run_script(copy.deepcopy(dict(s, ops=s["ops"][:i])))
                o["parent"] = d.parent_of(d.find(o["layer"])).id
        if np.array_equal(_render(dict(s, ops=ops)), results[stem]):
            failures.append("mutation 40 not caught by %s" % stem)
    # 41: set_adjustment merges into the previous params
    for stem in ("ADJ-01", "ADJ-02"):
        s = _strip(scripts[stem])
        ops = copy.deepcopy(s["ops"])
        created = {o["id"]: o.get("params", {}) for o in ops if o["op"] == "add_adjustment"}
        for o in ops:
            if o["op"] == "set_adjustment":
                o["params"] = _deep_merge(created[o["layer"]], o["params"])
        mutant = _render(dict(s, ops=ops))
        if np.array_equal(mutant, results[scripts[stem]["equal_to"]]):
            failures.append("mutation 41 not caught by %s" % stem)
    return failures


def golden_identities(results, scripts):
    """Cross-script facts doc 60 implies beyond the scripts' own equal_to pairs."""
    failures = []

    def check(cond, msg):
        if not cond:
            failures.append(msg)
    # DUP-07 (explicit parent root = top) must differ from DUP-01 (directly above the source)
    check(not np.array_equal(results["DUP-01"], results["DUP-07"]), "DUP-01 == DUP-07")
    # selection goldens: output alpha is S (255 where S is empty); S computed independently here
    for stem in ("SAL-01", "SAL-02", "SAL-03", "SAL-04", "SAL-05", "SAL-06", "SAL-07"):
        s = _strip(scripts[stem])
        ops = s["ops"]
        i = max(k for k, o in enumerate(ops) if o["op"] == "select_alpha")
        before = run_script(copy.deepcopy(dict(s, ops=ops[:i])))
        o = ops[i]
        Sb = before.sel.astype(int)
        B = before.find(o["layer"]).rgba[:, :, 3].astype(int)
        m = o.get("mode", "new")
        S = {"new": B, "add": np.maximum(Sb, B), "subtract": np.clip(Sb - B, 0, 255),
             "intersect": np.minimum(Sb, B)}[m]
        tail = [t["op"] for t in ops[i + 1:]]
        if tail[:1] == ["undo"]:
            S = Sb
        elif tail[:1] == ["reselect"]:
            S = before.sel_saved.astype(int)
        if not S.any():
            S = np.full_like(S, 255)
        check(np.array_equal(results[stem][:, :, 3].astype(int), S), "%s: alpha != S" % stem)
    # SAL-01: the hidden, masked, 0.3-opacity source's own alpha (0 -> 255 ramp inside [8, 56))
    a = results["SAL-01"][:, :, 3]
    check(not a[:, :8].any() and not a[:, 56:].any() and a[:, 55].min() > 240,
          "SAL-01 alpha is not src's raw alpha ramp")
    # DEL-08: empty root renders bg #808080FF
    check(np.all(results["DEL-08"] == np.array([128, 128, 128, 255], np.uint8)), "DEL-08 != bg")
    return failures


def test_goldens():
    results, scripts, _, failures = render_goldens()
    failures += golden_identities(results, scripts)
    failures += mutation_probes(results, scripts)
    assert not failures, failures


def main(argv):
    outdir = None
    if len(argv) >= 3 and argv[1] == "--goldens":
        outdir = argv[2]
    tests = [(k, v) for k, v in sorted(globals().items()) if k.startswith("test_") and k != "test_goldens"]
    t0 = time.perf_counter()
    bad = 0
    for name, fn in tests:
        try:
            fn()
        except Exception as e:           # report every failure, keep going
            bad += 1
            print("FAIL %s: %r" % (name, e))
    print("unit self-checks: %d/%d passed in %.2fs" % (len(tests) - bad, len(tests),
                                                       time.perf_counter() - t0))
    gdir = golden_dir()
    if os.path.isdir(gdir):
        t1 = time.perf_counter()
        results, scripts, timings, failures = render_goldens(outdir)
        failures += golden_identities(results, scripts)
        failures += mutation_probes(results, scripts)
        n_err = sum(1 for v in results.values() if isinstance(v, ScriptError))
        print("goldens (%s): %d scripts (%d error scripts), %d equal_to pairs, in %.2fs" % (
            os.path.relpath(gdir, HERE), len(timings), n_err,
            sum(1 for j in scripts.values() if "equal_to" in j), time.perf_counter() - t1))
        for f in failures:
            print("FAIL golden " + f)
        bad += len(failures)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
