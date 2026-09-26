#!/usr/bin/env python3
"""gen_compositing.py - write the doc-10 golden corpus (docs/math/10-compositing.md §12) as render
scripts into tests/scripts/compositing/<golden id>.json.

Deterministic: no randomness, no timestamps; running it twice produces identical bytes. Every op
uses exactly the doc-10 §11 grammar (and doc-20 §A9 for `add_adjustment`). Where §12 leaves a
detail open (content and rects of group children, extra layers of merge goldens, ...), the value
chosen here is fixed below and commented; the goldens are specified only by the doc's intent
column, and both renderers render the same bytes, so any fixed choice is a valid test.

    python3 tests/scripts/gen/gen_compositing.py [--out DIR] [--check]

--check verifies the files on disk match what would be generated (exit 1 if not).
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEFAULT_OUT = HERE.parent / "compositing"

# §3.1 table order (JSON spellings)
MODES = ["norm", "diss", "dark", "mul", "idiv", "lbrn", "dkCl", "lite", "scrn", "div", "lddg",
         "lgCl", "over", "sLit", "hLit", "vLit", "lLit", "pLit", "hMix", "diff", "smud", "fsub",
         "fdiv", "hue", "sat", "colr", "lum"]


# ---------------------------------------------------------------------------------------------
# op constructors (field names exactly as doc 10 §11.2)
# ---------------------------------------------------------------------------------------------
def layer_solid(id, color, rect=None, parent=None):
    op = {"op": "add_layer", "id": id}
    if parent:
        op["parent"] = parent
    op.update({"fill": "solid", "color": color})
    if rect:
        op["rect"] = rect
    return op


def layer_grad(id, frm, to, dir, rect=None, parent=None):
    op = {"op": "add_layer", "id": id}
    if parent:
        op["parent"] = parent
    op.update({"fill": "gradient", "from": frm, "to": to, "dir": dir})
    if rect:
        op["rect"] = rect
    return op


def layer_checker(id, a, b, cell, rect=None, parent=None):
    op = {"op": "add_layer", "id": id}
    if parent:
        op["parent"] = parent
    op.update({"fill": "checker", "a": a, "b": b, "cell": cell})
    if rect:
        op["rect"] = rect
    return op


def layer_noise(id, seed, alpha="random", rect=None, parent=None):
    op = {"op": "add_layer", "id": id}
    if parent:
        op["parent"] = parent
    op.update({"fill": "noise", "seed": seed, "alpha": alpha})
    if rect:
        op["rect"] = rect
    return op


def group(id, mode=None, parent=None):
    op = {"op": "add_group", "id": id}
    if parent:
        op["parent"] = parent
    if mode is not None:
        op["mode"] = mode
    return op


def adjustment(id, type_, parent=None):
    op = {"op": "add_adjustment", "id": id}
    if parent:
        op["parent"] = parent
    op["type"] = type_
    return op


def blend(layer, mode, seed=None):
    op = {"op": "set_blend", "layer": layer, "mode": mode}
    if seed is not None:
        op["seed"] = seed
    return op


def opacity(layer, v):
    return {"op": "set_opacity", "layer": layer, "value": v}


def fill(layer, v):
    return {"op": "set_fill", "layer": layer, "value": v}


def visible(layer, v):
    return {"op": "set_visible", "layer": layer, "value": v}


def clip(layer, v=True):
    return {"op": "set_clip", "layer": layer, "value": v}


def clbl(layer, v):
    return {"op": "set_clbl", "layer": layer, "value": v}


def mask_solid(layer, value=255, rect=None, outside=None):
    op = {"op": "add_mask", "layer": layer, "fill": "solid", "value": value}
    if rect:
        op["rect"] = rect
    if outside is not None:
        op["outside"] = outside
    return op


def mask_grad(layer, frm, to, dir):
    return {"op": "add_mask", "layer": layer, "fill": "gradient", "from": frm, "to": to,
            "dir": dir}


def mask_noise(layer, seed):
    return {"op": "add_mask", "layer": layer, "fill": "noise", "seed": seed}


def script(w, h, ops, bg="#00000000"):
    return {"canvas": {"w": w, "h": h, "bg": bg}, "ops": ops, "out": "png8"}


# ---------------------------------------------------------------------------------------------
# §12.1 blend modes: 27 x 4 = 108
# ---------------------------------------------------------------------------------------------
def blend_cases() -> dict:
    out = {}
    b_layer = layer_grad("b", "#00FFFFFF", "#FF0000FF", "h")
    s_layer = layer_grad("s", "#0000FFFF", "#FFFF00FF", "v")
    for mode in MODES:
        tail = [blend("s", mode, 7)]  # "after which set_blend s <mode> with seed 7"
        out[f"blend_{mode}_opq"] = script(256, 256, [b_layer, s_layer] + tail)
        out[f"blend_{mode}_op50"] = script(256, 256, [b_layer, s_layer, opacity("s", 0.5)] + tail)
        out[f"blend_{mode}_bda"] = script(256, 256, [b_layer, mask_noise("b", 11), s_layer] + tail)
        out[f"blend_{mode}_rnd"] = script(256, 256, [
            layer_noise("b", 21, "random"), layer_noise("s", 22, "random"),
            opacity("s", 0.75)] + tail)
    return out


# ---------------------------------------------------------------------------------------------
# §12.2 fill versus opacity: 16
# ---------------------------------------------------------------------------------------------
def BACKDROP():
    return layer_grad("k", "#2040C0FF", "#F0C020FF", "h")


def BASE():
    return layer_grad("base", "#FF000000", "#FF0000FF", "v", rect=[16, 16, 96, 96])


def CLIP():
    return [layer_noise("c", 5, 255), clip("c", True)]


def fo_cases() -> dict:
    out = {}
    U = layer_solid("u", "#10E080FF", rect=[32, 32, 64, 64])
    out["fo_lone_fill"] = script(128, 128, [BACKDROP(), U, fill("u", 0.5)])
    out["fo_lone_opacity"] = script(128, 128, [BACKDROP(), U, opacity("u", 0.5)])
    out["fo_lone_both"] = script(128, 128, [BACKDROP(), U, fill("u", 0.6), opacity("u", 0.7)])
    out["fo_lone_mul_fill"] = script(128, 128, [BACKDROP(), U, blend("u", "mul"), fill("u", 0.4)])
    out["fo_lone_lddg_fill"] = script(128, 128,
                                      [BACKDROP(), U, blend("u", "lddg"), fill("u", 0.6)])
    bbc = [BACKDROP(), BASE()] + CLIP()
    out["fo_clip_fill0"] = script(128, 128, bbc + [fill("base", 0.0)])
    out["fo_clip_opacity0"] = script(128, 128, bbc + [opacity("base", 0.0)])
    out["fo_clip_fill50"] = script(128, 128, bbc + [fill("base", 0.5)])
    out["fo_clip_opacity50"] = script(128, 128, bbc + [opacity("base", 0.5)])
    out["fo_clip_fill0_mul"] = script(128, 128, bbc + [fill("base", 0.0), blend("c", "mul")])
    out["fo_clip_fill30_op70"] = script(128, 128, bbc + [
        layer_solid("c2", "#FFFFFF80"), clip("c2", True), blend("c", "scrn"),
        fill("base", 0.3), opacity("base", 0.7)])
    out["fo_clip_fill0_clbl0"] = script(128, 128, bbc + [
        fill("base", 0.0), blend("c", "mul"), clbl("base", False)])
    out["fo_clip_basemul_fill50"] = script(128, 128, bbc + [blend("base", "mul"),
                                                            fill("base", 0.5)])
    out["fo_clipped_fill"] = script(128, 128, bbc + [fill("c", 0.5)])
    out["fo_adjust_fill"] = script(128, 128, [BACKDROP(), adjustment("a", "invert"),
                                              fill("a", 0.5)])
    out["fo_near_zero"] = script(128, 128, [
        layer_solid("u", "#FF8000FF"), opacity("u", 0.001),
        layer_grad("v", "#00FF0000", "#00FF0004", "h", rect=[64, 0, 64, 128])], bg="#00000000")
    # v covers only x >= 64: a later layer composited with c == 0 over a non-canonical pixel
    # returns (0,0,0,0) (ao == 0), so v must leave the left half untouched for mutation 22 to show.
    return out


# ---------------------------------------------------------------------------------------------
# §12.3 groups: 12   (children: rects chosen here so they overlap partially)
# ---------------------------------------------------------------------------------------------
R1 = [16, 16, 64, 64]          # lower child
R2 = [48, 40, 64, 64]          # upper child, overlaps R1 in [48,80) x [40,80)


def two_children(g):
    """Two Normal children: an opaque solid and a straight-alpha gradient (alpha FF -> 40)."""
    return [layer_solid("g1", "#E03020FF", rect=R1, parent=g),
            layer_grad("g2", "#20A0F0FF", "#F0E02040", "v", rect=R2, parent=g)]


def two_opaque_children(g):
    return [layer_solid("g1", "#E03020FF", rect=R1, parent=g),
            layer_grad("g2", "#20A0F0FF", "#40F060FF", "v", rect=R2, parent=g)]


def mul_child(g):
    return [layer_grad("m", "#FF8040FF", "#4080FFFF", "h", rect=[24, 24, 80, 80], parent=g),
            blend("m", "mul")]


def grp_cases() -> dict:
    out = {}
    out["grp_pass_plain"] = script(128, 128, [BACKDROP(), group("g")] + two_children("g"))
    out["grp_pass_mul_child"] = script(128, 128, [BACKDROP(), group("g", "pass")] + mul_child("g"))
    out["grp_pass_opacity"] = script(128, 128, [BACKDROP(), group("g", "pass"), opacity("g", 0.5)]
                                     + two_opaque_children("g"))
    out["grp_pass_mask"] = script(128, 128, [BACKDROP(), group("g", "pass")] + mul_child("g")
                                  + [mask_grad("g", 0, 255, "v")])
    out["grp_pass_transparent_bd"] = script(128, 128, [
        layer_noise("k", 3, "random"), group("g", "pass"), opacity("g", 0.5),
        layer_grad("sc", "#FF4000FF", "#0040FFFF", "h", rect=[16, 16, 96, 96], parent="g"),
        blend("sc", "scrn")], bg="#00000000")
    out["grp_iso_mul_child"] = script(128, 128, [BACKDROP(), group("g", "isolated")]
                                      + mul_child("g"))
    out["grp_iso_screen"] = script(128, 128, [BACKDROP(), group("g", "scrn"), opacity("g", 0.6)]
                                   + two_children("g"))
    out["grp_iso_mask_diss"] = script(128, 128, [
        BACKDROP(), group("g", "diss"), blend("g", "diss", 9), opacity("g", 0.7),
        mask_noise("g", 4)] + two_children("g"))
    out["grp_pass_adjust"] = script(128, 128, [BACKDROP(), group("g", "pass"),
                                               adjustment("inv", "invert", parent="g")])
    out["grp_iso_adjust"] = script(128, 128, [
        BACKDROP(), group("g", "isolated"),
        layer_solid("g1", "#E03020FF", rect=R1, parent="g"),
        adjustment("inv", "invert", parent="g")])
    # pass G1 (0.8) > isolated mul G2 (0.6) > pass G3 (0.5) > two `over` children.
    # One Normal child below the nested group at G1 and G2 level gives the backdrop threading
    # something to thread.
    out["grp_nested"] = script(128, 128, [
        BACKDROP(),
        group("G1", "pass"), opacity("G1", 0.8),
        layer_grad("n1", "#FFD000FF", "#00D0FFFF", "v", rect=[8, 8, 96, 96], parent="G1"),
        group("G2", "mul", parent="G1"), opacity("G2", 0.6),
        layer_solid("n2", "#C0C0FFFF", rect=[24, 24, 88, 88], parent="G2"),
        group("G3", "pass", parent="G2"), opacity("G3", 0.5),
        layer_grad("o1", "#FF0000FF", "#0000FF80", "h", rect=[16, 40, 96, 48], parent="G3"),
        blend("o1", "over"),
        layer_noise("o2", 13, "random", rect=[40, 16, 48, 96], parent="G3"),
        blend("o2", "over")])
    out["grp_hidden_empty"] = script(128, 128, [
        BACKDROP(),
        group("e1", "pass"),
        group("e2", "isolated"),
        group("h", "pass"), layer_solid("h1", "#E03020FF", rect=R1, parent="h"),
        layer_solid("h2", "#20A0F0FF", rect=R2, parent="h"), visible("h", False),
        group("v", "isolated"), layer_solid("v1", "#10E080FF", rect=[32, 32, 64, 64], parent="v"),
        visible("v1", False)])
    return out


# ---------------------------------------------------------------------------------------------
# §12.4 clipping masks: 8
# ---------------------------------------------------------------------------------------------
def clip_cases() -> dict:
    out = {}
    bbc = [BACKDROP(), BASE()] + CLIP()
    out["clip_partial_base"] = script(128, 128, bbc)
    out["clip_base_mask"] = script(128, 128, bbc + [mask_solid("base", 255,
                                                               rect=[16, 16, 48, 96])])
    out["clip_clbl_on"] = script(128, 128, [BACKDROP(), BASE(), blend("base", "scrn")] + CLIP()
                                 + [blend("c", "mul")])
    out["clip_clbl_off"] = script(128, 128, [BACKDROP(), BASE(), blend("base", "scrn")] + CLIP()
                                  + [blend("c", "mul"), clbl("base", False)])
    out["clip_base_opacity_two"] = script(128, 128, [BACKDROP(), BASE(), opacity("base", 0.5)]
                                          + CLIP() + [
        layer_grad("c2", "#FFFF00FF", "#00FFFFFF", "h"), clip("c2", True),
        blend("c2", "over"), opacity("c2", 0.6)])
    # second clip group: base2 on the right half-ish, hidden, with one visible clipped layer
    out["clip_hidden"] = script(128, 128, bbc + [
        visible("c", False),
        layer_grad("c2", "#00FF00FF", "#0000FFFF", "h"), clip("c2", True),
        layer_grad("base2", "#FFFFFFFF", "#FFFFFF20", "h", rect=[72, 8, 48, 112]),
        layer_noise("c3", 17, 255), clip("c3", True),
        visible("base2", False)])
    # a clipped layer at root index 0 (no backdrop below it), then a group, then a clipped layer
    out["clip_invalid_base"] = script(128, 128, [
        layer_grad("c0", "#2040C0FF", "#F0C020FF", "h"), clip("c0", True),
        group("gr", "pass"), layer_solid("gr1", "#E03020FF", rect=R1, parent="gr"),
        layer_noise("c1", 19, 255, rect=[40, 40, 72, 72]), clip("c1", True)])
    out["clip_adjust"] = script(128, 128, [BACKDROP(), BASE(), adjustment("inv", "invert"),
                                           clip("inv", True)])
    return out


# ---------------------------------------------------------------------------------------------
# §12.5 layer masks: 6
# ---------------------------------------------------------------------------------------------
def mask_cases() -> dict:
    out = {}
    base = [BACKDROP(), layer_noise("u", 8, 255), mask_grad("u", 0, 255, "h")]
    out["mask_gradient"] = script(128, 128, base)
    out["mask_disabled"] = script(128, 128, base + [
        {"op": "set_mask_enabled", "layer": "u", "value": False}])
    out["mask_deleted"] = script(128, 128, base + [{"op": "delete_mask", "layer": "u"}])
    out["mask_applied"] = script(128, 128, [
        BACKDROP(), layer_checker("u", "#FF0000FF", "#0000FF80", 8),
        mask_grad("u", 255, 0, "v"), {"op": "apply_mask", "layer": "u"}, opacity("u", 0.8)])
    out["mask_group_rect"] = script(128, 128, [BACKDROP(), group("g", "isolated")]
                                    + two_children("g")
                                    + [mask_solid("g", 255, rect=[20, 20, 60, 60], outside=64)])
    out["mask_order"] = script(128, 128, [
        BACKDROP(), layer_noise("u", 6, "random"), blend("u", "over"), fill("u", 0.8),
        opacity("u", 0.5), mask_noise("u", 12)])
    return out


# ---------------------------------------------------------------------------------------------
# §12.6 extras: 6
# ---------------------------------------------------------------------------------------------
def extra_cases() -> dict:
    out = {}
    for mode in ("dkCl", "lgCl"):
        out[f"blend_{mode}_tie"] = script(16, 16, [
            layer_solid("b", "#3B0000FF"), layer_solid("s", "#001E00FF"), blend("s", mode)])
    out["merge_down_plain"] = script(128, 128, [
        BACKDROP(),
        layer_grad("u", "#FF8040FF", "#4080FF80", "v", rect=[16, 16, 96, 96]),
        blend("u", "mul"), opacity("u", 0.5), mask_grad("u", 0, 255, "h"),
        {"op": "merge_down", "layer": "u"}, opacity("k", 0.8)])
    out["merge_down_clip"] = script(128, 128, [BACKDROP(), BASE(), fill("base", 0.5)] + CLIP()
                                    + [{"op": "merge_down", "layer": "c"}])
    out["merge_visible_hidden"] = script(128, 128, [
        layer_grad("l1", "#2040C0FF", "#F0C02080", "h"),
        layer_solid("l2", "#10E080FF", rect=[32, 32, 64, 64]), visible("l2", False),
        layer_noise("l3", 23, "random", rect=[48, 8, 72, 112]),
        {"op": "merge_visible"}, visible("l2", True)], bg="#808080FF")
    out["flatten_partial_bg"] = script(128, 128, [
        layer_grad("u", "#0000FFFF", "#00FF0040", "h", rect=[16, 16, 96, 96]),
        blend("u", "scrn"), {"op": "flatten"}], bg="#FF000080")
    return out


def all_cases() -> dict:
    cases = {}
    for part in (blend_cases(), fo_cases(), grp_cases(), clip_cases(), mask_cases(),
                 extra_cases()):
        for k, v in part.items():
            assert k not in cases, k
            cases[k] = v
    return cases


EXPECTED_COUNTS = {"blend": 108, "fo": 16, "grp": 12, "clip": 8, "mask": 6, "extra": 6}


def serialise(obj) -> str:
    # one op per line: readable diffs, deterministic bytes
    lines = ["{", f'  "canvas": {json.dumps(obj["canvas"])},', '  "ops": [']
    ops = obj["ops"]
    for i, op in enumerate(ops):
        lines.append("    " + json.dumps(op) + ("," if i + 1 < len(ops) else ""))
    lines += ["  ],", f'  "out": {json.dumps(obj["out"])}', "}"]
    return "\n".join(lines) + "\n"


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", default=str(DEFAULT_OUT))
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args(argv)
    cases = all_cases()
    n_blend = sum(1 for k in cases if k.startswith("blend_") and not k.endswith("_tie"))
    counts = {"blend": n_blend, "fo": sum(k.startswith("fo_") for k in cases),
              "grp": sum(k.startswith("grp_") for k in cases),
              "clip": sum(k.startswith("clip_") for k in cases),
              "mask": sum(k.startswith("mask_") for k in cases),
              "extra": len(extra_cases())}
    assert counts == EXPECTED_COUNTS, counts
    assert len(cases) == 156, len(cases)
    out = Path(args.out)
    bad = []
    if not args.check:
        out.mkdir(parents=True, exist_ok=True)
    for name in sorted(cases):
        text = serialise(cases[name])
        assert json.loads(text) == cases[name]
        p = out / f"{name}.json"
        if args.check:
            if not p.exists() or p.read_text() != text:
                bad.append(name)
        else:
            p.write_text(text)
    if args.check:
        extra = sorted(p.stem for p in out.glob("*.json") if p.stem not in cases)
        if bad or extra:
            print(f"out of date: {bad}; unexpected files: {extra}")
            return 1
        print(f"{len(cases)} goldens up to date in {out}")
        return 0
    print(f"wrote {len(cases)} goldens to {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
