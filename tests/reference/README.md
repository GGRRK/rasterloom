# tests/reference — the NumPy reference renderer (the oracle)

`refcomp.py` renders a render script exactly as `docs/math/` specifies, independently of the C++
core. The C++ output and this output must agree byte for byte on every golden script.

**Independence rule.** Everything here is written from `docs/math/` only. Authors of files in this
directory never open `src/` (or any C++); the C++ author never opens this directory.

```bash
python3 tests/reference/refcomp.py SCRIPT.json OUT.png   # exit 0 + PNG, or non-zero and no PNG
python3 tests/reference/test_refcomp.py                  # self-checks (plain asserts; pytest also works)
```

Exit codes: `0` success; `2` script error (C9: unknown op/field, wrong type, out of range, unknown
id, wrong node kind, …); `3` internal error (a bug in the reference; a traceback is printed);
`64` bad command line. The PNG is written only on success (via a temp file and rename). A
pre-existing `OUT.png` is left alone on failure, so harnesses must check the exit code.

Dependencies: Python 3, NumPy (float64 only), Pillow (PNG encode/decode only).

## Layout

| path | what |
|---|---|
| `refcomp.py` | CLI |
| `rc/core.py` | `q`, `dec`, `clamp01`, `canonicalize`, `assemble`, splitmix64 / `pixel_hash` / `unit` (scalar and grid), `ScriptError` |
| `rc/fields.py` | `FieldReader`, the strict JSON field reader |
| `rc/model.py` | `Document`, `RasterLayer`, `AdjustmentLayer`, `Group`, `Mask` |
| `rc/blend.py` | the 27 blend functions (doc 10 §3) |
| `rc/composite.py` | `COMPOSITE`, `ADJUST`, coverage, `RENDER`, clip groups, groups, `render_document` (doc 10 §4–§7) |
| `rc/fills.py` | content fills of doc 10 §11.1 (`layer_fill`, `mask_fill`, `rect_region`, `gradient_t`) |
| `rc/registry.py` | op registry, adjustment registry, auto-discovery |
| `rc/runner.py` | script parsing, the op loop, C10 history and the `undo` op |
| `rc/ops_compositing.py` | every doc 10 op, `add_adjustment`, and the `invert` adjustment |
| `rc/ops_*.py` | **your module goes here** (auto-discovered) |
| `examples/*.json` | the BUILD-SPEC example and scripts exercising every doc 10 op |
| `test_refcomp.py` | self-checks with hand-computed bytes |

## Extending: add a module `rc/ops_<something>.py`

Every module in `rc/` whose name starts with `ops_` is imported automatically (sorted by name)
before a script runs. Do not edit other people's modules; just create yours. Registering the same
op or adjustment twice raises at import time.

### Registering an op

```python
# rc/ops_geometry.py
import numpy as np
from .core import ScriptError, q, dec, canonicalize
from .registry import op

@op("flip")                     # history=True by default: one C10 record per op
def flip(doc, f):
    axis = f.str("axis", choices=("h", "v"))          # required string with a fixed set
    if f.has("layer"):
        layer = doc.get_raster(f.str("layer"))
        ...
```

- The handler is `handler(doc, f)`; `doc` is the `Document` (mutate it in place), `f` is a
  `FieldReader` over the op object (the `"op"` key is already consumed).
- **Unknown fields are automatic.** After your handler returns, the runner raises a script error
  for any key you never read. So read only the fields that belong to the chosen variant; a field
  that doesn't belong (e.g. `from` with `fill:"solid"`) then errors by itself. `f.has(name)` tests
  presence without marking it read.
- **Errors:** raise `ScriptError("…")` (or `raise f.err("…")`, which prefixes the op path) for any
  invalid value, unknown id, wrong node kind, singular matrix, etc. Never clamp silently.
- An exception other than `ScriptError` is reported as an internal error (exit 3).
- The runner checks after every op that `doc.w`, `doc.h` <= 16384 (C9).
- History (C10): the runner snapshots the whole document before each op with `history=True` and
  pushes it after the op succeeds; `undo` pops. Snapshots are only taken when the script contains
  an `undo` op anywhere (they are unobservable otherwise). Use `@op(name, history=False)` only for
  an op that must not create a record (only `undo` does). Anything you store on the document must
  be deep-copyable (numpy arrays, Python values); put module-private persistent state in
  `doc.ext["<yourmodule>.<key>"]` so it is snapshotted and restored with the document.

### FieldReader accessors

All take `(name, default)`; omit `default` to make the field required. Each marks the field read.

| accessor | accepts |
|---|---|
| `f.str(name, default, choices=None, nonempty=False)` | JSON string (optionally from a set) |
| `f.int(name, default, lo=None, hi=None)` | JSON integer token (not bool, **not `2.0`**), inclusive range |
| `f.seed(name, default)` | integer in `[0, 2^53)` |
| `f.num(name, default, lo, hi, lo_open=False, hi_open=False)` | any JSON number → binary64, finite, range-checked (bool rejected) |
| `f.unit_num(name, default)` | number in `[0.0, 1.0]` |
| `f.bool(name, default)` | `true` / `false` |
| `f.color(name, default)` | `"#RRGGBB"`/`"#RRGGBBAA"` → `(r, g, b, a)` ints (not canonicalised) |
| `f.grey(name, default)` | integer `0..255` |
| `f.rect(name, w, h)` | `[x, y, w, h]` integers, `w, h >= 1`; default `[0, 0, W, H]` |
| `f.raw(name, default)` | the raw JSON value; you validate it |
| `f.obj(name, default)` | nested object → child `FieldReader` (its unknown keys are checked too) |
| `f.obj_list(name, default)` | array of objects → list of child `FieldReader`s (each checked) |

For an array of numbers (e.g. stroke points, a matrix) use `f.raw(...)` and validate each element
with `rc.fields.is_int` / `rc.fields.is_number` yourself.

### The document model

```python
doc.w, doc.h                 # canvas size (ints); geometry ops may change them
doc.bg                       # (r, g, b, a) ints, canonical
doc.root                     # the root container (a Group with id "root"); doc.root.children
doc.sel                      # np.uint8 (h, w): doc 30 selection coverage S; all 0 = no selection
doc.sel_saved                # None (absent) or np.uint8 (h, w): doc 30 `Saved` for reselect
doc.selection_is_empty()     # True if every byte of sel is 0
doc.selection_effective()    # E: a new uint8 (h, w) array, 255 everywhere if empty, else sel
doc.ext                      # dict for module-private state (snapshotted by undo)

doc.get_node(id, kinds=("raster", "adjustment", "group"))   # ScriptError: unknown / "root" / wrong kind
doc.get_raster(id)           # get_node(id, ("raster",))
doc.get_container(id)        # "root" or a group id -> Group
doc.parent_of(node)          # the container holding node
doc.iter_nodes()             # every node, depth-first, bottom-to-top per container
doc.find(id)                 # node or None
doc.check_new_id(id)         # ScriptError if empty / "root" / duplicate
doc.empty_rgba()             # np.zeros((h, w, 4), uint8)
```

Nodes (`node.kind` is `"raster"`, `"adjustment"` or `"group"`):

| attribute | kinds | meaning |
|---|---|---|
| `id`, `visible`, `opacity` (float), `seed` (Dissolve seed) | all | doc 10 §1 |
| `mask` | all | `None` or `Mask` with `.data` (np.uint8 (h, w)), `.enabled` (bool), `.outside` (int) |
| `rgba` | raster | np.uint8 (h, w, 4), straight RGBA, **canonical** (alpha 0 ⇒ (0,0,0,0)) |
| `lock_alpha` | raster | lock transparency (C8a, doc 10 §10) |
| `mode`, `fill`, `clip`, `clbl` | raster, adjustment | blend mode JSON key, fill, clip flag, clbl |
| `adj_type`, `params` | adjustment | type name and whatever its parser returned |
| `mode`, `children` | group | `"pass"` or a blend-mode key (`"isolated"` is stored as `"norm"`); child list, index 0 = bottom |

Rules when you write pixels:

- Replace or edit `layer.rgba` (shape `(doc.h, doc.w, 4)`, dtype `uint8`) and **canonicalise**
  afterwards: `rc.core.canonicalize(arr)` (in place) or build it with
  `rc.core.assemble(r, g, b, a)` from four uint8 planes.
- Lock transparency is *your* op's rule (C8a / doc 10 §10 for painting, doc 20 §B0 for filters);
  read `layer.lock_alpha`. Merge Down, Apply Mask and Flatten ignore it.
- Selection: read `doc.selection_effective()` and decode with `dec(...)`; the selection ops of
  doc 30 own `doc.sel` / `doc.sel_saved`.
- **Every canvas-sized array** a canvas-geometry op must resize: `doc.w`, `doc.h`, every raster's
  `rgba`, every node's `mask.data`, `doc.sel`, `doc.sel_saved` (if not None).

### Registering an adjustment type

`add_adjustment` (doc 20 §A9) is implemented here and dispatches on `type`:

```python
# rc/ops_adjust_filters.py
from .registry import register_adjustment

def parse_levels(p):             # p: FieldReader over the op's "params" object ({} if absent)
    rgb = p.obj("rgb", {})       # nested objects: child readers, unknown keys error automatically
    return {"in_black": rgb.int("in_black", 0, 0, 255), ...}

def apply_levels(params, r, g, b):   # uint8 (h, w) planes in; return three uint8 (h, w) planes
    lut = build_lut(params)          # 256-entry uint8 table built from scalars (Python math)
    return lut[r], lut[g], lut[b]

register_adjustment("levels", parse_levels, apply_levels)
```

- `apply` is doc 20's `f` (bytes → bytes). It must not read alpha. The compositor (doc 10 §4.2
  `ADJUST`) handles alpha 0, coverage, blend mode and canonicalisation.
- Valid type names are the eight of doc 20 §A9; a known but unregistered type is a script error
  ("not implemented in the reference"). `invert` is registered in `ops_compositing.py`.

### Useful helpers

- `rc.core`: `q(x)` (arrays → uint8, scalars → int; raises on NaN), `dec(v)`, `clamp01(x)`,
  `canonicalize(rgba)`, `assemble(r, g, b, a)`, `splitmix64(z)`, `pixel_hash(seed, x, y, k)`,
  `unit(h)` (scalars), `pixel_hash_grid(seed, w, h, k)` (uint64 (h, w) for the whole canvas),
  `unit_grid(hashes)`, `hash_byte_grid(seed, w, h, k)` (top 8 bits).
- `rc.composite`: `COMPOSITE(D, src_rgb, c, mode, seed)`, `ADJUST(D, node, c, mode, seed)`,
  `coverage(node, h, w)`, `mask_factor(node, h, w)`, `render_document(doc, onto_bg=True)` (the
  document composite, e.g. for "sample merged").
- `rc.fills`: `rect_region(rect, w, h)` (bool mask of on-canvas pixels in a rect),
  `gradient_t(rect, w, h, dir)`.
- C1 reminder: vectorise `+ - * /`, `np.sqrt`, `np.floor`, `np.minimum/maximum`, `np.where`
  freely, in the stated evaluation order; never NumPy transcendental ufuncs (`np.exp`, `np.power`,
  `np.sin`, …), never `np.round`, never FMA. Transcendentals go through Python `math` on scalars
  (a 256-entry LUT, or a per-row / per-dab precompute).

## Choices made where the docs are silent or ambiguous

These are the reference's readings; each is also reported to the lead.

1. **`int` fields reject `2.0`.** C9 says integers "must be integral JSON numbers"; the reference
   accepts only a JSON integer token (Python `int`, never `bool`). `1.0` for `cell` is an error.
2. **Duplicate JSON object keys are a script error**, and `NaN`/`Infinity` literals are rejected.
3. **Every op except `undo` pushes one history record**, including an op that happens to change
   nothing (e.g. `merge_visible` with no visible node, a no-op `reselect`).
4. **Top-level script keys** `canvas`, `ops` and `out` are all required; `out` must be `"png8"`.
5. **`add_mask` variant fields** follow the `add_layer` rule: a field of another fill (e.g.
   `value` with `fill:"gradient"`) is an error.
6. **Selection storage** follows doc 30 §2 (no separate "active" flag: all-zero = no selection).
7. **`merge_visible` id** is validated (non-empty, not duplicate) only when a merge happens.
