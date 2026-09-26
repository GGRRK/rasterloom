#!/usr/bin/env python3
"""Static checker for the doc 20/30/40 golden corpus (tests/scripts/{adjust_filters,geometry,brush}).

Not a renderer. For every *.json it checks:
  - the file parses; top-level keys are canvas/ops/out (+ harness keys expect/equal_to);
  - every op name is one the math docs define (table below, per owning doc);
  - every op field is one that op's table defines, and int-typed fields are JSON integers;
  - layer/group ids referenced by ops exist at that point of the script (valid scripts only).
Scripts marked "expect": "error" are checked for op names only; their deliberate defect is
reported as information, not as a failure.

Exit status 0 when every valid golden passes; prints a per-doc op usage table.
"""
import json
import os
import sys
from collections import Counter, defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
DIRS = ["adjust_filters", "geometry", "brush"]

# op -> (owning doc, allowed fields, int-typed fields). Fields per the op tables:
# doc 10 section 11.2 (+ 00 C10 for undo), doc 20 A9/B1-B6, doc 30 section 19, doc 40 section 2.
_STROKE = {"layer", "size", "hardness", "spacing", "opacity", "flow", "angle", "roundness", "mode",
           "dabs_per_second", "smoothing", "size_curve", "opacity_curve", "view_zoom", "samples"}
_SEL_MODE = {"mode"}
OPS = {
    # doc 10
    "add_layer": ("10", {"id", "parent", "fill", "color", "from", "to", "dir", "a", "b", "cell", "seed",
                         "alpha", "rect"}, {"cell", "seed"}),
    "add_group": ("10", {"id", "parent", "mode"}, set()),
    "move_layer": ("10", {"id", "parent", "index"}, {"index"}),
    "set_blend": ("10", {"layer", "mode", "seed"}, {"seed"}),
    "set_opacity": ("10", {"layer", "value"}, set()),
    "set_fill": ("10", {"layer", "value"}, set()),
    "set_visible": ("10", {"layer", "value"}, set()),
    "set_clip": ("10", {"layer", "value"}, set()),
    "set_clbl": ("10", {"layer", "value"}, set()),
    "add_mask": ("10", {"layer", "fill", "value", "from", "to", "dir", "seed", "rect", "outside"},
                 {"value", "from", "to", "seed", "outside"}),
    "set_mask_enabled": ("10", {"layer", "value"}, set()),
    "delete_mask": ("10", {"layer"}, set()),
    "apply_mask": ("10", {"layer"}, set()),
    "lock_transparency": ("10", {"layer", "value"}, set()),
    "merge_down": ("10", {"layer"}, set()),
    "merge_visible": ("10", {"id"}, set()),
    "flatten": ("10", {"id"}, set()),
    "undo": ("00/40", {"steps"}, {"steps"}),
    # doc 20
    "add_adjustment": ("20", {"id", "type", "params", "parent"}, set()),
    "filter_gaussian_blur": ("20", {"layer", "radius", "edge", "coverage"}, set()),
    "filter_motion_blur": ("20", {"layer", "angle", "distance", "edge", "coverage"}, set()),
    "filter_unsharp_mask": ("20", {"layer", "amount", "radius", "threshold", "edge", "coverage"}, {"threshold"}),
    "filter_add_noise": ("20", {"layer", "amount", "distribution", "monochromatic", "seed", "coverage"}, {"seed"}),
    "filter_high_pass": ("20", {"layer", "radius", "edge", "coverage"}, set()),
    "filter_offset": ("20", {"layer", "dx", "dy", "mode", "coverage"}, {"dx", "dy"}),
    # doc 30
    "select_rect": ("30", {"x", "y", "w", "h", "antialias"} | _SEL_MODE, set()),
    "select_ellipse": ("30", {"x", "y", "w", "h", "antialias"} | _SEL_MODE, set()),
    "select_polygon": ("30", {"points", "antialias"} | _SEL_MODE, set()),
    "select_wand": ("30", {"layer", "x", "y", "tolerance", "contiguous", "antialias"} | _SEL_MODE,
                    {"x", "y", "tolerance"}),
    "select_all": ("30", set(), set()),
    "deselect": ("30", set(), set()),
    "reselect": ("30", set(), set()),
    "select_inverse": ("30", set(), set()),
    "feather": ("30", {"radius"}, set()),
    "expand": ("30", {"by"}, {"by"}),
    "contract": ("30", {"by"}, {"by"}),
    "fill_selection": ("30", {"layer", "color", "opacity"}, set()),
    "transform": ("30", {"layer", "interp", "matrix", "translate", "scale", "rotate", "skew", "pivot",
                         "quad", "rect"}, set()),
    "image_size": ("30", {"w", "h", "interp"}, {"w", "h"}),
    "canvas_size": ("30", {"w", "h", "anchor"}, {"w", "h"}),
    "crop": ("30", {"x", "y", "w", "h"}, {"x", "y", "w", "h"}),
    "rotate_canvas": ("30", {"angle"}, set()),
    "flip": ("30", {"axis", "layer"}, set()),
    "gradient": ("30", {"layer", "type", "p0", "p1", "c0", "c1", "opacity", "reverse"}, set()),
    "bucket_fill": ("30", {"layer", "x", "y", "color", "opacity", "tolerance", "contiguous", "antialias"},
                    {"x", "y", "tolerance"}),
    # doc 40
    "brush_stroke": ("40", _STROKE | {"target", "color"}, set()),
    "eraser_stroke": ("40", set(_STROKE), set()),
    "clone_stroke": ("40", _STROKE | {"source", "aligned", "source_layer"}, set()),
}
SAMPLE_FIELDS = {"x", "y", "pressure", "tilt_x", "tilt_y", "t_ms"}
COVERAGE_FIELDS = {"src", "x", "y", "w", "h", "value", "dir"}
TOP = {"canvas", "ops", "out", "expect", "equal_to"}
LAYER_REF_FIELDS = ("layer", "source_layer")


def is_int(v):
    return isinstance(v, int) and not isinstance(v, bool)


def is_num(v):
    return isinstance(v, (int, float)) and not isinstance(v, bool)


def check_file(path):
    problems = []
    with open(path) as fh:
        s = json.load(fh)
    for k in s:
        if k not in TOP:
            problems.append(f"unknown top-level key {k!r}")
    if s.get("out") != "png8":
        problems.append("out != png8")
    if "expect" in s and s["expect"] != "error":
        problems.append("expect must be 'error'")
    c = s.get("canvas", {})
    if set(c) - {"w", "h", "bg"} or not is_int(c.get("w")) or not is_int(c.get("h")):
        problems.append("bad canvas")
    ids = {"root"}
    ops_used = []
    for i, op in enumerate(s.get("ops", [])):
        name = op.get("op")
        ops_used.append(name)
        if name not in OPS:
            problems.append(f"op[{i}] unknown op {name!r}")
            continue
        _, allowed, ints = OPS[name]
        for f, v in op.items():
            if f == "op":
                continue
            if f not in allowed:
                problems.append(f"op[{i}] {name}: unknown field {f!r}")
            elif f in ints and not is_int(v):
                problems.append(f"op[{i}] {name}: field {f!r} must be int, got {v!r}")
        if name in ("brush_stroke", "eraser_stroke", "clone_stroke"):
            for j, smp in enumerate(op.get("samples", [])):
                if set(smp) - SAMPLE_FIELDS:
                    problems.append(f"op[{i}] sample {j}: unknown fields {set(smp) - SAMPLE_FIELDS}")
                if not all(is_num(smp.get(k)) for k in ("x", "y")):
                    problems.append(f"op[{i}] sample {j}: x/y missing or not numbers")
        if "coverage" in op:
            cov = op["coverage"]
            if set(cov) - COVERAGE_FIELDS or cov.get("src") not in ("selection", "rect", "ramp"):
                problems.append(f"op[{i}] bad coverage {cov}")
        # id bookkeeping (approximate model: enough to catch typos in the generators)
        for f in LAYER_REF_FIELDS + ("parent",):
            if f in op and op[f] not in ids:
                problems.append(f"op[{i}] {name}: {f}={op[f]!r} not defined yet")
        if name in ("add_layer", "add_group", "add_adjustment"):
            ids.add(op["id"])
        elif name == "merge_down":
            ids.discard(op["layer"])
        elif name == "flatten":
            ids = {"root", op.get("id", "flattened")}
    return s, problems, ops_used


def main():
    usage = defaultdict(Counter)       # doc -> op -> count
    n_valid = n_err = n_fail = 0
    for d in DIRS:
        folder = os.path.join(ROOT, d)
        for f in sorted(os.listdir(folder)):
            if not f.endswith(".json"):
                continue
            path = os.path.join(folder, f)
            try:
                s, problems, ops_used = check_file(path)
            except (json.JSONDecodeError, OSError) as e:
                print(f"FAIL {d}/{f}: does not parse: {e}")
                n_fail += 1
                continue
            unknown_ops = [o for o in ops_used if o not in OPS]
            if s.get("expect") == "error":
                n_err += 1
                if unknown_ops:
                    print(f"FAIL {d}/{f}: error case uses unknown op(s) {unknown_ops}")
                    n_fail += 1
                elif problems:
                    print(f"info {d}/{f} (expect error): static defect {problems[0]}")
                continue
            n_valid += 1
            for o in ops_used:
                if o in OPS:
                    usage[OPS[o][0]][o] += 1
            if problems:
                n_fail += 1
                for p in problems:
                    print(f"FAIL {d}/{f}: {p}")
    print()
    print("op usage in valid goldens, by owning doc:")
    for doc in sorted(usage):
        print(f"  doc {doc}: " + ", ".join(f"{o}x{n}" for o, n in sorted(usage[doc].items())))
    unused = sorted(o for o in OPS if not any(o in usage[doc] for doc in usage))
    print(f"  defined but unused: {', '.join(unused) if unused else '-'}")
    print(f"\n{n_valid} valid goldens, {n_err} expect-error scripts, {n_fail} failures")
    return 1 if n_fail else 0


if __name__ == "__main__":
    sys.exit(main())
