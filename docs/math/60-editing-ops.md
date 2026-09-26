# Rasterloom normative math — 60: editing ops (layer structure, names, selection from alpha)

Status: normative for v0.1. Inherits every rule of `00-conventions.md` (C1–C10), the node model and
render rules of `10-compositing.md`, the adjustment parameter tables of `20-adjustments-filters.md`
and the selection model of `30-geometry-selection.md`. Where this file seems to disagree with
00-conventions, 00 wins and the disagreement is a bug to report. `50-resolutions.md` section 0
(lead rulings) applies here too, in particular ruling 4 (every op except `undo` pushes exactly one
history record) and ruling 5 (strict script shape).

This file defines six ops that the GUI needs from the core and that docs 10–40 do not define
(§14, an addendum of 2026-09-26, adds three more for the clipboard and drag-and-drop:
`place_image`, `layer_via_copy`, `clear`):

| op | what the user does |
|---|---|
| `delete_layer` | Layers panel → Delete (a layer, an adjustment layer, or a group with its contents) |
| `set_name` | double-click a layer name and type a new one |
| `duplicate_layer` | Layer → Duplicate (Ctrl+J), including a group with everything inside it |
| `set_adjustment` | edit an existing adjustment layer's parameters in the Properties panel |
| `set_group_mode` | toggle a group between Pass Through and isolated |
| `select_alpha` | Ctrl-click a layer thumbnail (Ctrl+Shift add, Ctrl+Alt subtract, Ctrl+Shift+Alt intersect) |

Only `select_alpha` computes pixel values (selection bytes, §7). The other five change the node
tree or node properties; their pixel consequences follow from doc 10's render rules, which are
unchanged. The golden set (§10) therefore mostly proves *structure* (which node ends up where,
with which properties) through its pixel consequences.

Notation: as doc 10 (`n(v) = v / 255.0`, `q`, "canonicalise"). "Container" means the root or a
group. "Subtree of X" means X and all its descendants. Indices are child-list indices, 0 = bottom
(doc 10 §1).

---

## 1. Model additions

### 1.1 Node name

Every node (raster, adjustment, group) gains one property:

- `name`: a string, **default `""`**. It is a *display label only*: it is never used to address a
  node, never enters any pixel formula, and need not be unique.
- `display_name(node) = node.name if node.name != "" else node.id`. So a node that was never named
  displays its id (the GUI shows this in the Layers panel; the file writers store it as the layer
  name).

Every op that creates a node creates it with `name = ""`: `add_layer`, `add_group`,
`add_adjustment`, and the result layer of `merge_visible` and `flatten`. `merge_down` keeps the lower
layer's `name` (it keeps all of `L`'s other properties too, doc 10 §9.1 step 3). `duplicate_layer`
sets names by §4.4. File readers set `name` from the file (non-normative note in §13).

A valid name (the only values `set_name` accepts) is a JSON string whose decoded value

1. consists of Unicode scalar values only: a JSON escape of a lone surrogate (`"\ud800"`) or of a
   reversed pair is invalid, and so is any byte sequence in the script file that is not UTF-8;
2. contains no code point in `U+0000..U+001F` or `U+007F` (C0 controls and DEL: they cannot be shown
   on one line in the Layers panel and most are forbidden in the XML of `.ora`/`.orp`);
3. has **at most 255 code points** (not bytes, not UTF-16 units). `""` is valid.

The name is stored exactly as decoded: no trimming, no case change, no Unicode normalisation.
Leading, trailing and repeated spaces are kept.

### 1.2 Ids are immutable

A node's `id` is fixed from the op that creates it until the node is removed. No op changes an id.
(The GUI-local op `rename_layer`, which changed ids, is superseded by `set_name` and must not be
added to the core registry.) When a node is removed (`delete_layer`, `merge_down`'s upper layer,
`merge_visible`, `flatten`), the ids of its whole subtree become **free**: a later op may create a
node with one of them, exactly as if it had never been used. `undo` of the removing op makes them
taken again (the restored nodes have them).

The id rules of doc 10 §11 are unchanged: non-empty strings, unique across all nodes, `"root"`
reserved.

### 1.3 History

Each of the six ops pushes exactly one history record when it succeeds, including when it changes
nothing observable (50 §0 ruling 4). `{"op":"undo"}` restores the complete document state from
before the record: the node tree with every property (names included), every node's pixels and
mask, the selection `S` and `Saved` (doc 30 §2), and therefore which ids are taken. No op in this
file changes `canvas.bg` or the canvas size.

---

## 2. `delete_layer`

**Semantics.** Removes the node named by `layer` and, if it is a group, everything inside it.

```
delete_layer(X):
    P = X's container, i = X's index in P
    remove P.children[i]                   # the whole subtree of X goes
    # siblings keep their relative order; those above X move down by one index
```

- Any node kind may be deleted. `"root"` cannot (script error, doc 10 §11 "root is not a node").
- Deleting the only node, or every node one by one, is valid: a document with an empty root renders
  `canvas.bg` alone (doc 10 §5 with no children).
- Nothing else changes: no other node's property (in particular no other node's `clip` flag), the
  selection `S`, `Saved`, `bg`. The removed subtree's ids become free (§1.2).

**Clipping consequences.** Clipping is a per-node flag and the clip group is derived by doc 10 §5 at
render time; `delete_layer` does not touch any flag. After the deletion the §5 loop re-derives every
clip group from the new sibling order. Concretely, when a clip base `B` (raster, `clip = false`) with
clipped layers `C1..Ck` directly above it is deleted:

- if the node now directly below `C1` is a raster layer with `clip = false`, `C1..Ck` join its clip
  group (appended after its own clipped layers, if it has any);
- if the node directly below `C1` is a raster layer with `clip = true`, the run continues downward:
  `C1..Ck` join the clip group that layer belongs to (its base is the nearest raster layer below it
  with `clip = false`; doc 10 §5 collects consecutive clipped layers);
- if there is no node below `C1` in the container, or that node is a group or an adjustment layer,
  `C1..Ck` render unclipped, each as a lone layer (doc 10 §5, "rendered as an ordinary unclipped
  layer"), and they keep `clip = true`, so moving a raster layer back under them re-clips them.

Deleting a clipped layer only removes it from its clip group; deleting the last clipped layer of a
group makes the base render `LONE` again (doc 10 §6.1 last paragraph).

**Merge rules.** All merge preconditions (doc 10 §9.1) are evaluated on the tree as it is when the
merge op runs. So a `merge_down U` that was an error because the node above `U` was clipped becomes
valid once that clipped node is deleted, and a `merge_down` whose `U` or `L` was deleted is an
unknown-id error. `merge_visible` and `flatten` simply see fewer nodes.

**Errors (script errors per C9):** `layer` missing, not a string, unknown, or `"root"`; any other
field.

---

## 3. `set_name`

**Semantics.** `node.name = name`. `""` resets the node to the default (it displays its id again).
Any node kind. Pixels never change; the op still pushes one history record (§1.3), so an `undo`
right after it reverts only the name.

Names are not ids: after `set_name u "k"`, the string `"k"` in a later op still means the node whose
**id** is `k` (or is an unknown-id error if there is none), never `u`.

**Errors:** `layer` missing / not a string / unknown / `"root"`; `name` missing, not a JSON string,
or not a valid name (§1.1: lone surrogate, a control code point, more than 255 code points); any
other field.

---

## 4. `duplicate_layer`

**Semantics.** Makes a deep copy of the subtree of the node `S` named by `layer` and inserts it.

### 4.1 What is copied

The copy `S'` of `S`, and recursively the copy of each descendant, has every property of its
original with the same value, and its own independent storage (a later edit to one never affects the
other):

| node kind | copied |
|---|---|
| every kind | `visible`, `opacity`, the layer mask (every byte, its `outside` value, its enabled flag) or its absence |
| raster | every pixel byte; `mode`, `seed`, `fill`, `clip`, `clbl`, `lock_alpha` |
| adjustment | `type` and the complete params; `mode`, `seed`, `fill`, `clip`, `clbl` |
| group | `mode` (including `pass`), `seed`, and its children, in the same order, each copied by this table |

Only `id` (§4.3) and `name` (§4.4) differ from the original. The selection, `Saved`, `bg` and every
other node are unchanged. A hidden source gives a hidden copy.

### 4.2 Where the copy goes

- **No `parent` field:** the copy goes into `S`'s own container at index `i + 1`, where `i` is
  `S`'s index: **directly above the source**, below whatever was above it.
- **With `parent`:** the copy goes to the **top** of the named container (doc 10 `add_layer`
  placement), even when `parent` names `S`'s own container. `parent` must be `"root"` or a group
  id, and must not be `S` itself or a descendant of `S` (script error, like `move_layer` into itself).

Clipping follows from the flags and doc 10 §5, with nothing special-cased:

- duplicating a clip base `B` (no `parent`) puts `B'` (with `clip = false`) between `B` and its
  clipped layers, so those clipped layers now clip to `B'`, and `B` renders on its own;
- duplicating a clipped layer (no `parent`) puts its copy (with `clip = true`) directly above it,
  inside the same clip group.

### 4.3 Ids of the copy (the derivation rule)

- `S'` gets the id given in the `id` field.
- Every descendant `d` of `S` (children, grandchildren, …; only when `S` is a group) gets the id
  `id + "/" + d.id`: the new top id, a slash (U+002F), and the **original descendant's own id**,
  not its path. Example: duplicating group `g` (children `c1`, and group `h` holding `d`) with
  `"id":"g2"` creates `g2` with children `g2/c1` and `g2/h`, and `g2/h` holds `g2/d`.

These ids are pairwise distinct (the originals are). Before anything is inserted, every new id
(the top id and all derived ids) is checked against the ids that exist in the document at that
moment; **any** collision, or a top id equal to `"root"` or `""`, is a script error and the
document is unchanged. The derived ids are ordinary ids afterwards: any op may use them, and a later
duplicate of `g2` with `"id":"g3"` applies the same rule to `g2`'s descendants (whose ids are
`g2/c1`, `g2/h`, `g2/d`), giving `g3`, `g3/g2/c1`, `g3/g2/h` and `g3/g2/d`.

(The GUI chooses the top id and must check the derived ids too before sending the op.)

### 4.4 Names of the copy

- `S'.name = display_name(S) + " copy"` if that string has at most 255 code points, else `""`.
  (With the default name, `display_name(S)` is `S`'s id, so the copy of an unnamed layer `u`
  displays `u copy`.) If `display_name(S)` contains a code point that §1.1 forbids (possible only
  for an id, since ids have no character rule), `S'.name = ""`.
- Each descendant copy `d'`: `d'.name = display_name(d)` if that is a valid name (§1.1), else `""`.
  So every copied child displays the same label as its original, not its derived id.

Names never affect pixels; this rule exists so that the GUI, `.orp`/`.ora` and PSD writers agree.

**Errors:** `layer` missing / not a string / unknown / `"root"`; `id` missing, not a string, `""`,
`"root"`, or taken; a derived id taken; `parent` not a string, unknown, not a container (a raster
or adjustment layer), or `S` or inside `S`; any other field.

---

## 5. `set_adjustment`

**Semantics.** Replaces the parameters of an existing adjustment layer.

- `layer` must name an adjustment layer (raster, group or `"root"`: script error).
- `params` (required) is validated **exactly** as `add_adjustment`'s `params` for the node's
  current `type` (doc 20 A1–A8, A9 and `50-resolutions.md` section 3: strict keys and types, ranges,
  cross-field rules such as `in_black < in_white` after defaults, the `colorize`-dependent ranges
  and defaults of A4).
- The new params **replace** the old ones wholesale: every key that `params` omits takes the type's
  **default**, never its previous value. Example: an adjustment created with levels
  `{"rgb":{"in_black":40,"gamma":1.6}}` and then set to `{"rgb":{"in_white":200}}` is exactly a
  levels layer with `{"rgb":{"in_white":200}}` (in_black 0, gamma 1.0). `params: {}` resets the
  layer to its type's defaults.
- The type cannot change: there is no `type` field (it is an unknown-field error). To change type,
  delete the layer and add a new one.
- Every other property of the node (mode, seed, opacity, fill, visible, clip, clbl, mask, name,
  position) is unchanged.

Pixels: the node renders by doc 10 §4.2 with the new `ADJ` from now on. The result is byte-identical
to a document in which the node had been created with the new params (the goldens assert this with
`equal_to`).

**Errors:** `layer` missing / not a string / unknown / not an adjustment layer; `params` missing or
not a JSON object; any params error doc 20 defines for the node's type; any other field.

---

## 6. `set_group_mode`

**Semantics.** The Layers panel's "isolate" toggle for a group.

```
set_group_mode(G, m):
    if m == "pass":      G.mode = pass
    if m == "isolated":  if G.mode == pass:  G.mode = norm       # doc 10: "isolated" = norm
                         else: G.mode unchanged                 # already isolated; keep its blend
```

- `layer` must name a group (raster, adjustment or `"root"`: script error).
- `mode` is required and is exactly `"pass"` or `"isolated"`; any other string (including a blend
  mode such as `"norm"` or `"scrn"`) is a script error. To pick a specific blend mode for an
  isolated group, use doc 10 `set_blend`.
- The difference from `set_blend … "isolated"` is deliberate: `set_blend` always sets `norm`, while
  `set_group_mode "isolated"` keeps a group that is already isolated with, say, Screen, in Screen.
  Switching an isolated Screen group to `"pass"` and back to `"isolated"` gives `norm` (no memory of
  the earlier mode).
- The Dissolve `seed`, opacity, mask, children and everything else are unchanged. Pushes one record
  even when the mode does not change.

Pixels: doc 10 §7.1 (pass) or §7.2 (isolated) from now on.

**Errors:** `layer` missing / not a string / unknown / not a group; `mode` missing / not a string /
not one of the two values; any other field.

---

## 7. `select_alpha`

**Semantics.** Makes a selection from a raster layer's transparency (Ctrl-click the thumbnail),
combined with the current selection by `mode`.

**Formula** (integers, exact). For every canvas pixel `(x, y)`:

```
B(x, y) = A byte of layer L's stored pixel at (x, y)        # 0 where L was never written (doc 10 §1)
S'(x, y) = combine(S(x, y), B(x, y), mode)                   # doc 30 §4, on bytes:
    new:        B
    add:        max(S, B)
    subtract:   max(S - B, 0)
    intersect:  min(S, B)
```

`S` is the stored selection array (all 0 when there is no selection, doc 30 §2/§4).

- `B` is the layer's **own pixel alpha only**. It ignores the layer's mask (enabled or not), its
  visibility (a hidden layer can be the source), its opacity and fill, its blend mode, clipping, and
  `lock_alpha`. Doc 30's §9 selection contract does not apply (this op writes the selection, not
  pixels).
- `Saved` is not changed (doc 30 §5: only `deselect` writes it).
- If `S'` is all zero, the document has **no selection** (doc 30 §2), so for example `select_alpha`
  of an empty layer with `mode` `new` or `intersect` means that the next fill covers the whole
  canvas. The GUI may warn, as for any empty selection; the op itself never fails for that.
- Only raster layers are sources in v0.1: an adjustment layer (it has no pixels), a group (its
  composite alpha would need a render) or `"root"` is a script error.

**Errors:** `layer` missing / not a string / unknown / not a raster layer; `mode` not a string or not
one of the four values (default `"new"`); any other field.

---

## 8. Interaction summary (what each op leaves alone)

| op | tree | node properties | pixels / masks | `S` | `Saved` | ids |
|---|---|---|---|---|---|---|
| `delete_layer` | subtree removed | — | removed with the subtree | kept | kept | subtree ids freed |
| `set_name` | — | `name` of one node | — | kept | kept | — |
| `duplicate_layer` | copy inserted (§4.2) | copies (§4.1) | copied | kept | kept | new + derived ids taken |
| `set_adjustment` | — | params of one adjustment node | — | kept | kept | — |
| `set_group_mode` | — | `mode` of one group | — | kept | kept | — |
| `select_alpha` | — | — | — | replaced (§7) | kept | — |

None of the six ops reads or honours the selection when changing the tree, and none of them is
affected by `lock_alpha`.

---

## 9. Render-script ops (strict grammar)

Common rules are doc 10 §11 and 00 C9: an unknown field, a missing required field, a wrong JSON type
or an out-of-range value is a script error (exit non-zero, no PNG). `str` = JSON string.

| op | fields (type, default, range) | semantics |
|---|---|---|
| `delete_layer` | `layer` str, required: any node id, not `"root"` | §2 |
| `set_name` | `layer` str, required: any node id, not `"root"`; `name` str, required: a valid name (§1.1: scalar values only, no `U+0000..U+001F` / `U+007F`, at most 255 code points; `""` allowed) | §3 |
| `duplicate_layer` | `layer` str, required: any node id, not `"root"`; `id` str, required: non-empty, not `"root"`, not taken, and no derived id (§4.3) taken; `parent` str, optional: `"root"` or a group id, not the source or inside it; absent = directly above the source | §4 |
| `set_adjustment` | `layer` str, required: an adjustment layer; `params` object, required: doc 20's params for that layer's type | §5 |
| `set_group_mode` | `layer` str, required: a group; `mode` str, required: `"pass"` \| `"isolated"` | §6 |
| `select_alpha` | `layer` str, required: a raster layer; `mode` str, default `"new"`: `"new"` \| `"add"` \| `"subtract"` \| `"intersect"` | §7 |

Examples:

```json
{"op":"delete_layer","layer":"g"}
{"op":"set_name","layer":"u","name":"Sky – warm"}
{"op":"duplicate_layer","layer":"g","id":"g2"}
{"op":"duplicate_layer","layer":"u","id":"u2","parent":"g"}
{"op":"set_adjustment","layer":"lv1","params":{"rgb":{"in_white":200}}}
{"op":"set_group_mode","layer":"g","mode":"isolated"}
{"op":"select_alpha","layer":"src","mode":"add"}
```

---

## 10. Golden cases

Location: **`tests/scripts/editing/<id>.json`** once both implementations of this doc exist. Until
then the generator writes them to `tests/scripts/.pending-editing/` (a dot-directory, which
`run_goldens.py` and the per-domain CTest glob both skip), so the existing gates stay green while
the ops are unimplemented. The last step of the implementation lane is
`python3 tests/scripts/gen/gen_editing.py --activate`, which writes the same files to
`tests/scripts/editing/` and removes the pending directory (then re-run CMake so
`goldens_editing` is registered).

Generator: `tests/scripts/gen/gen_editing.py` (deterministic; it also runs a structural checker over
every script: op names and fields, id existence per §1.2/§4.3, and for every `err_*` script the
specific rule it breaks). Files are named by the ids below; `*R` files are the `equal_to` targets,
which are goldens in their own right. Error scripts are `err_<what>.json` with `"expect":"error"`
(tests/scripts/README.md).

Fixture **K**: canvas 64×64, bg `#00000000`, `add_layer k` gradient h `#2040C0FF`→`#F0C020FF`
(doc 10 §12.2's backdrop). Fixture **SRC** (selection goldens): canvas 64×64, bg `#00000000`,
`add_layer src` gradient h `#FF000000`→`#FF0000FF` rect `[8,0,48,64]` (alpha 0 → 255 across the
rect, 0 outside), `add_mask src` noise seed 3, `set_opacity src 0.3`, `set_visible src false`, then
`add_layer L` (empty); every selection golden ends with `fill_selection L #1060e0ff`, so the output
alpha is `S` (255 everywhere where `S` is empty). The exact ops are in the generator.

### 10.1 `delete_layer`

| id | setup | what it catches |
|---|---|---|
| DEL-01 | K + `u` solid `#10E080FF` [8,8,32,32] + `v` solid `#E03020C0` [24,24,32,32]; `delete_layer u` | wrong node removed; index shift of the nodes above |
| DEL-02 | K + pass group `g` with `c1` solid and `c2` `mul`; `delete_layer g` (equal_to FIX-K) | children left behind (reparented instead of deleted) |
| DEL-03 | K + `b0` solid [4,4,28,56] + base (alpha-ramp red [16,8,40,48]) + `c` noise clipped; `delete_layer base` | §2 re-derivation: `c` must clip to `b0` (not be released, not vanish) |
| DEL-04 | K + group `g` + base + `c` clipped (partial alpha); `delete_layer base` | no raster below: `c` renders unclipped (not hidden, not clipped to `k`) |
| DEL-05 | K + base + `c1` clipped + `c2` clipped `mul`; `delete_layer c1` | a later clipped layer keeps its base |
| DEL-06 | K + isolated group `g` with 2 children; `delete_layer g`; `undo`; `set_opacity c2 0.5` | undo restores the subtree and its ids |
| DEL-07 | K + `u`; `delete_layer u`; `add_layer u` (different colour and rect) | ids of deleted nodes are freed |
| DEL-08 | bg `#808080FF`; `a`, group `g` ⊃ `b`; delete `a`, delete `g` | deleting every node is valid; empty root renders bg |
| DEL-09 | K + `u` `mul` + `c` clipped; `delete_layer c`; `merge_down u` | merge preconditions evaluated on the current tree |

### 10.2 `set_name`

| id | setup | what it catches |
|---|---|---|
| NAME-01 | K + `u`; `set_name u "Übermalung Ω"`; `undo` (equal_to NAME-01R = K + `u`) | set_name must push a record: a missing record makes `undo` remove `u` |
| NAME-02 | K + `u`; `set_name u "k"`; `set_opacity k 0.5` | names are not addressable: the opacity lands on `k`, not `u` |
| NAME-03 | K + `u`; group `g` ⊃ adjustment `a` (invert, fill 0.5); names on all three, `set_name u ""`, then `set_opacity u 0.5` | any node kind; `""` valid; ids unchanged |
| NAME-04 | K + `u`; a name of 254 × `é` + `𝄞` (255 code points, 512 UTF-8 bytes, 256 UTF-16 units) | length counted in code points, not bytes or UTF-16 units |
| err_name_too_long | 256 ASCII characters | limit enforced |
| err_name_control | name contains `\n` | C0 control rejected |
| err_name_del | name contains U+007F | DEL rejected |
| err_name_lone_surrogate | name `"\ud800x"` (escape in the file) | lone surrogate rejected (Python's `json` accepts it; the reference must check) |
| err_name_root | `set_name root …` | root is not a node |
| err_name_not_string | `name` is a number | type check |
| err_name_missing | no `name` | required field |

### 10.3 `duplicate_layer`

| id | setup | what it catches |
|---|---|---|
| DUP-01 | K + `u` solid [8,8,32,32] + `t` solid `mul` [20,20,32,32]; `duplicate_layer u id u2` | **mutation 40**: the copy must sit directly above `u`, under `t`; at the top it hides the Multiply |
| DUP-02 | K + `u` noise (random alpha), mask gradient (then disabled), `diss` seed 77, fill 0.6, opacity 0.7, lock; `duplicate_layer u id u2`; `delete_layer u`; `set_mask_enabled u2 true`; select + `fill_selection u2` (locked) (equal_to DUP-02R, the same ops on `u` without the duplicate) | every property copied: pixels, mask bytes and enabled flag, mode, seed, fill, opacity, lock |
| DUP-03 | K + isolated `scrn` group `g` (opacity 0.8, rect mask) ⊃ `c1`, `c2` clipped, invert `a` (fill 0.5), group `h` ⊃ `d`; `duplicate_layer g id g2`; hide `g`; edit `g2/c1`, `g2/a`, `g2/d` | deep copy of a group; the flat derived-id rule (`g2/d`, not `g2/h/d`) |
| DUP-04 | K + base (alpha ramp) + `c` noise clipped; `duplicate_layer base id b2`; `set_opacity b2 0.5` | copy of a base goes between base and its clipped layers (**mutation 40** second catch) |
| DUP-05 | K + hidden `u`; `duplicate_layer u id u2` (equal_to FIX-K) | hidden source gives a hidden copy |
| DUP-06 | K + pass group `g` ⊃ `x`; `u` at root; `duplicate_layer u id u2 parent g`; hide `u` | `parent` given: top of that container |
| DUP-07 | DUP-01's layers; `duplicate_layer u id u2 parent root` | explicit `parent` = own container still means top (not "above the source") |
| DUP-08 | K + posterize `a` (levels 3, fill 0.5); duplicate as `a2`; `undo`; duplicate as `a2` again; `set_adjustment a2 {levels: 6}` | undo frees the copy's id; the copy's params are independent of the source's |
| DUP-09 | K + base + `c` clipped `mul`; `duplicate_layer c id c2`; `set_blend c2 scrn` | a clipped copy stays clipped in the same clip group |
| err_dup_id_taken | `id` equals an existing id | unique ids |
| err_dup_derived_taken | a root layer `g2/c1` exists; duplicate group `g` (child `c1`) as `g2` | derived ids checked (§4.3) |
| err_dup_into_self | duplicate `g` with `parent g` | parent inside the source |
| err_dup_into_descendant | duplicate `g` with `parent h` (`h` inside `g`) | parent inside the source |
| err_dup_parent_not_group | `parent` is a raster layer | parent kind |
| err_dup_root | `layer` is `"root"` | root is not a node |
| err_dup_id_root | `id` is `"root"` | reserved id |
| err_dup_missing_id | no `id` | required field (no automatic id in scripts) |

### 10.4 `set_adjustment`

| id | setup | what it catches |
|---|---|---|
| ADJ-01 | K + levels `{"rgb":{"in_black":40,"gamma":1.6}}`; `set_adjustment` `{"rgb":{"in_white":200}}` (equal_to ADJ-01R: created with the final params) | **mutation 41**: params merged into the old ones instead of replaced |
| ADJ-02 | K + hue_saturation `{colorize true, hue 200, saturation 60, lightness 10}`; set to `{colorize true, hue 30}` (equal_to ADJ-02R) | colorize-dependent default (saturation 25) applied on replace; **mutation 41** |
| ADJ-03 | K + pass group ⊃ `x` + threshold `a` level 100, clipped to `x`, fill 0.5, `over`, opacity 0.8, mask; set level 180 (equal_to ADJ-03R: the same with level 180 from the start) | other properties (clip, fill, mode, opacity, mask, position) untouched |
| ADJ-04 | K + posterize levels 3; set levels 6; `undo` (equal_to ADJ-04R = K + posterize 3) | undo restores the old params |
| err_setadj_raster | target is a raster layer | kind check |
| err_setadj_group | target is a group | kind check |
| err_setadj_type_field | `type` present | type is immutable, unknown field |
| err_setadj_range | posterize `levels: 1` | doc 20 validation reused |
| err_setadj_foreign_key | threshold given `levels` | keys of the node's own type only |
| err_setadj_levels_order | levels `in_black 200, in_white 200` | cross-field rule after defaults |
| err_setadj_missing_params | no `params` | required field |

### 10.5 `set_group_mode`

| id | setup | what it catches |
|---|---|---|
| GM-01 | K + pass group ⊃ `mul` child; `set_group_mode isolated` (equal_to GM-01R: group created `isolated`) | pass → isolated gives `norm` |
| GM-02 | K + `scrn` group (opacity 0.6) ⊃ two children; `set_group_mode isolated` (equal_to GM-02R: no op) | an isolated blend mode is kept (not reset to `norm`) |
| GM-03 | GM-02's group; `set_group_mode pass` (equal_to GM-03R: group created `pass`, opacity 0.6) | isolated → pass |
| GM-04 | GM-01's pass group; `set_group_mode isolated`; `undo` (equal_to GM-04R: GM-01's setup alone) | one record; undo restores `pass` |
| err_gm_raster | target raster | kind |
| err_gm_adjustment | target adjustment | kind |
| err_gm_mode_norm | `mode: "norm"` | only the two values |
| err_gm_mode_missing | no `mode` | required |
| err_gm_root | target `"root"` | root is not a node |

### 10.6 `select_alpha`

| id | setup | what it catches |
|---|---|---|
| SAL-01 | SRC; `select_alpha src` | `B` = own alpha only: mask, visibility and opacity of `src` ignored |
| SAL-02 | SRC; `select_ellipse 4,4,40,40` aa; `select_alpha src add` | `max` on partial values |
| SAL-03 | same, `subtract` | clamped difference |
| SAL-04 | same, `intersect` | `min` |
| SAL-05 | SRC + empty layer `e`; `select_rect 0,0,16,16`; `select_alpha e` | all-zero result = no selection: the fill covers everything |
| SAL-06 | SRC; `select_rect 8,8,24,24`; `select_alpha src`; `undo` | one record; undo restores the rectangle |
| SAL-07 | SRC; `select_rect 8,8,24,24`; `deselect`; `select_alpha src`; `reselect` | `Saved` untouched by select_alpha |
| err_sal_group | source is a group | kind |
| err_sal_adjustment | source is an adjustment | kind |
| err_sal_mode | `mode: "xor"` | enum |
| err_sal_root | source `"root"` | root |

### 10.7 History across all six ops

| id | setup | what it catches |
|---|---|---|
| HIST-01 | K + `u` + pass group `g` ⊃ `x`, posterize `a`; then `set_name`, `duplicate_layer`, `set_adjustment`, `set_group_mode`, `select_alpha`, `delete_layer` (one each); `undo steps 6`; fill a new layer (equal_to HIST-01R: the setup + the fill) | each op pushes exactly one record |
| HIST-02 | the same six ops, then `undo steps 3` (equal_to HIST-02R: the setup + the first three ops) | partial undo lands between records; the duplicate's position and the replaced params survive |

Also generated: `FIX-K` (fixture K alone), the `equal_to` target of DEL-02 and DUP-05.

Totals: 39 goldens in the tables above plus 12 `*R` targets and `FIX-K` = **52 rendering scripts**
(14 of them carry `equal_to`), and **31 error scripts**.

---

## 11. Mutation hooks

Ids 40 and 41 are new (the count only goes up, BUILD-SPEC). The C++ author adds them to the
registry; the mutation gate's `--ids` range must then include them.

| id | defect, precisely | caught by |
|---|---|---|
| 40 | `duplicate_layer` without `parent` inserts the copy at the **top** of the source's container (the `add_layer` placement) instead of at index `i + 1` | DUP-01, DUP-04 |
| 41 | `set_adjustment` **merges** `params` into the node's previous params (keys omitted from `params` keep their old values) instead of replacing them with the type's defaults | ADJ-01, ADJ-02 |

Other likely defects that the goldens catch without a hook: `select_alpha` multiplying in the layer
mask (SAL-01), `set_group_mode isolated` resetting a Screen group to Normal (GM-02), derived ids
built from the path (DUP-03), deleted ids not freed (DEL-07), clip flags released on delete (DEL-03).

---

## 12. Parity notes (feed `docs/PARITY.md`)

1. **Deleting a clip base** keeps the clipped layers' flags, so they re-clip to the next raster
   layer below or render unclipped (§2). Photoshop's behaviour (whether it releases the clipped
   layers or re-bases them) was not checked. **Unknown.**
2. **Deleting the last layer** is allowed (an empty document renders its background). Photoshop does
   not allow deleting the last layer. **Divergence**; the GUI may still refuse it in the UI.
3. **Layer names** are limited to 255 code points with no control characters. Photoshop's limit is
   reported as 255 characters; its handling of control characters and of astral code points is
   unknown.
4. **Duplicate naming** appends `" copy"` to the top node only, and children keep their names.
   Photoshop numbers repeated copies (`copy 2`, `copy 3`); v0.1 does not. **Divergence** (names
   only, no pixel effect).
5. **Duplicate placement**: directly above the source, or at the top of a chosen container.
   Photoshop's "Duplicate Layer" dialog also offers other documents as destinations; v0.1 has only
   the same document. **Divergence.**
6. **Duplicating a clip base** re-bases its clipped layers onto the copy (§4.2). Photoshop's
   behaviour is not checked. **Unknown.**
7. **Load selection from transparency** uses the stored alpha byte directly (no threshold, no mask).
   Photoshop documents Ctrl-click as loading the layer's transparency; its arithmetic for Add /
   Subtract / Intersect on partial values is unknown (doc 30 parity note 4 applies). Ctrl-clicking a
   group or an adjustment layer thumbnail is not supported in v0.1 (Photoshop loads a group's
   merged transparency and an adjustment's mask). **Divergence.**
8. **Editing an adjustment layer** replaces all params at once. Photoshop edits live. The result is
   the same; it is noted only because v0.1's doc 20 A9 said params are fixed at creation, which
   this doc supersedes.
9. **Group isolate toggle**: switching Pass Through → isolated gives Normal, and the toggle keeps an
   already isolated group's blend mode. Photoshop has no separate toggle (the mode menu contains
   Pass Through); unknown whether it remembers the earlier mode.

---

## 13. Notes for the implementers (non-normative, binding where they restate)

- **C++**: the six ops belong in the core registry (`librasterloomcore`), not in `src/gui/`; the
  GUI-local `delete_layer` must be removed from `gui_ops.cpp` once the core registers it (same
  grammar, now with the clip and id rules above) and `rename_layer` must go (ids are immutable;
  the Rename action sends `set_name`). Validate every field and every new id **before** changing
  the document, so an error leaves the state untouched (the GUI's `apply_op` rolls back anyway,
  but the script engine does not need to).
- **Reference**: Python's `json` module accepts lone surrogate escapes and returns a `str` that
  contains them; check `any(0xD800 <= ord(ch) <= 0xDFFF for ch in name)`. `len(name)` counts code
  points once that check passes. nlohmann-json rejects lone surrogates while parsing, so the C++
  side fails earlier, which is also a script error.
- **CoW**: a duplicate may share tile references with its source under copy-on-write (BUILD-SPEC
  D5); the tests only require that later edits to one never show in the other (DUP-02, DUP-08).
- **Files**: ORA/PSD writers store `display_name(node)` as the layer name. Readers set `name` from
  the file; a file name that breaks §1.1 is sanitised by the reader, never rejected, in this order:
  1. *decode*: every ill-formed piece becomes U+FFFD — a lone UTF-16 surrogate (PSD `luni`), a
     three-byte encoded surrogate `ED A0..BF 80..BF` (what an XML character reference to a
     surrogate or a CESU-8 writer produces) as **one** U+FFFD, and every other maximal ill-formed
     UTF-8 subpart as one U+FFFD (Unicode §3.9, "U+FFFD substitution of maximal subparts"); a
     `.orp` `document.json` gets the same treatment for invalid UTF-8 and for `\uXXXX` escapes of
     lone surrogates before it is parsed;
  2. *drop* every code point in `U+0000..U+001F` and `U+007F`;
  3. *cut* to the first 255 code points.

  The result is stored exactly otherwise (no trimming, no normalisation), and each changed name
  adds an import warning (`PSD: layer name '…' was sanitised (…)`, `ORA: …`). The PSD group-end
  divider's name is not a node name and is not checked. The §1.1 rule binds `set_name`; this
  sanitising is how file import meets it.
- **Doc 20 A9** says "Params are fixed at creation in v0.1 (no 'edit adjustment' op)". This doc
  supersedes that sentence with `set_adjustment` (§5).

---

## 14. Addendum 2026-09-26: getting pixels in — `place_image`, `layer_via_copy`, `clear`

Status: normative for v0.1 (import lane). Everything above still holds; this section adds three
ops that the GUI's clipboard and drag-and-drop are built on (Edit → Cut / Copy / Copy Merged /
Paste / Paste in Place / Clear, Layer → New → Layer via Copy / Layer via Cut, File → Place, dropping
image files or image data on a document). As for the six ops above, every one of them pushes
exactly one history record (50 §0 ruling 4), validates every field before it changes the document
(§13), and is a script error in every case this section names (C9).

| op | what the user does |
|---|---|
| `place_image` | Paste, Paste in Place, File → Place, drop an image file or image data on an open document |
| `layer_via_copy` | Layer via Copy (Ctrl+J with a selection), Layer via Cut (Shift+Ctrl+J); also the in-document form of Copy / Copy Merged followed by Paste in Place |
| `clear` | Edit → Clear (Delete); the pixel half of Edit → Cut |

The system clipboard itself is not part of the document or of any script: a render script cannot
read it. What the GUI puts on the clipboard (the result of §14.4 `COPY`, cropped by §14.4.1) and
what it pastes are therefore recorded as a `place_image` op that **carries the pixels** in its
`png` field, so an exported session script replays byte-exactly with no clipboard at all.

### 14.1 The `png` payload: base64

`png` is a JSON string holding the payload bytes in base64 (RFC 4648 §4, the standard alphabet):

1. every character is one of `A–Z a–z 0–9 + /` or `=`; nothing else (no whitespace, no line
   breaks, no URL-safe `-` / `_`);
2. the length is a multiple of 4;
3. `=` appears only as padding: the last group of four is `xx==` or `xxx=`, every other group has
   no `=`. (Regular expression over the whole string:
   `^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$`.)

Decoding follows RFC 4648 §4; unused low bits of the last group (in `xx==` and `xxx=`) are
ignored even when they are not zero. Anything else is a script error.

### 14.2 The `png` payload: the PNG subset

The decoded bytes must be a PNG file (ISO/IEC 15948, W3C PNG 2nd edition) of this exact shape;
anything else is a script error. Multi-byte integers are big-endian.

1. **Signature**: bytes 0–7 are `89 50 4E 47 0D 0A 1A 0A`.
2. **Chunks** follow back to back: `length` (4 bytes, at most `2^31 − 1`), `type` (4 bytes, each
   an ASCII letter `A–Z` or `a–z`), `data` (`length` bytes), `crc` (4 bytes). A chunk that does not
   fit in the remaining bytes is an error. A chunk is **critical** when the first letter of its type
   is uppercase, **ancillary** otherwise.
3. **CRC**: for every critical chunk, `crc` must equal the CRC-32 of `type` followed by `data`
   (ISO 3309 / ITU-T V.42, the polynomial of PNG Annex D, i.e. zlib's `crc32`). Ancillary chunks
   (`tEXt`, `gAMA`, `iCCP`, `sRGB`, `pHYs`, …) are skipped entirely: neither their contents nor their
   CRC are examined, so colour-space and gamma information is **ignored** (bytes are taken as
   they are stored, like every other 8-bit input in v0.1).
4. **Order and kinds of critical chunks**: the first chunk is `IHDR`; after it only `PLTE`, `IDAT`
   and `IEND` may appear as critical chunks (any other critical type is an error), `IHDR` appears
   once, `PLTE` at most once and only before the first `IDAT` (its contents are ignored; it is a
   suggested palette for colour type 6), at least one `IDAT` exists, and all `IDAT` chunks are
   **consecutive** (no other chunk, critical or ancillary, between two `IDAT`s). The first `IEND`
   ends the file; bytes after it are ignored; reaching the end of the bytes before an `IEND` is an
   error.
5. **IHDR** has `length` 13: `width` (4 bytes), `height` (4 bytes), then five bytes that must be
   exactly: bit depth **8**, colour type **6** (RGBA), compression method 0, filter method 0,
   interlace method **0** (none). `width` and `height` must be in `1..16384` (C9's size limit).
   Every other bit depth, colour type (grey, RGB, palette, grey+alpha) or Adam7 interlacing is an
   error: the GUI always re-encodes what it pastes or places into this shape.
6. **Image data**: the `data` of all `IDAT` chunks, concatenated in order, is one zlib stream
   (RFC 1950 / RFC 1951) that must decompress completely (end-of-stream reached, Adler-32 correct)
   to **exactly** `height * (1 + 4 * width)` bytes: one filter-type byte followed by `4 * width`
   filtered bytes per row. Fewer or more bytes is an error. Bytes of the `IDAT` data after the end
   of the zlib stream are ignored.
7. **Unfiltering** (PNG §9): per row, the filter-type byte `t` must be 0–4 (else an error); with
   `bpp = 4`, for each byte `k` of the row (0-based), `a` = the reconstructed byte `k − 4` of this
   row (0 when `k < 4`), `b` = reconstructed byte `k` of the previous row (0 for the first row),
   `c` = reconstructed byte `k − 4` of the previous row (0 when `k < 4` or on the first row):
   ```
   t = 0 (None):    Recon = Filt
   t = 1 (Sub):     Recon = (Filt + a) mod 256
   t = 2 (Up):      Recon = (Filt + b) mod 256
   t = 3 (Average): Recon = (Filt + floor((a + b) / 2)) mod 256
   t = 4 (Paeth):   Recon = (Filt + paeth(a, b, c)) mod 256
   paeth(a, b, c): p = a + b - c; pa = |p - a|; pb = |p - b|; pc = |p - c|
                   return a if pa <= pb and pa <= pc, else b if pb <= pc, else c
   ```
   The reconstructed row is `width` pixels of `(R, G, B, A)` bytes, straight alpha: the payload
   image `P` with `P[j][i]` = pixel `i` of row `j`.

### 14.3 `place_image`

**Semantics.** Adds a new raster layer holding the payload image, placed with its top-left pixel
at canvas pixel `(x, y)`.

```
(w, h, P) = the payload (§14.1, §14.2)
if "x" is absent:  x = floor_div(W - w, 2);  y = floor_div(H - h, 2)     # centred on the canvas
new raster layer L: default properties (doc 10 §1: visible, opacity 1, fill 1, norm, Dissolve
                    seed 0, no clip, clbl, no lock, no mask), L.name = name (default "")
for every canvas pixel (cx, cy):
    i = cx - x;  j = cy - y
    L(cx, cy) = canonical(P[j][i])   if 0 <= i < w and 0 <= j < h
                (0, 0, 0, 0)          otherwise
insert L (§14.3.1)
```

`floor_div(n, 2)` is division rounding toward **negative infinity** (Python `n // 2`): an image
3 px wider than the canvas lands at `x = -2`, not `-1`. `canonical` is C3's rule (alpha byte 0 →
`(0, 0, 0, 0)`). Parts of the image outside the canvas are discarded (C8a: layers are canvas-sized).
The selection, `Saved`, `bg` and every other node are unchanged.

#### 14.3.1 Placement (shared by `place_image` and `layer_via_copy`)

- `above: X` (a node id, any kind, not `"root"`): the new layer goes into X's container at index
  `i + 1`, where `i` is X's index — **directly above X** (Photoshop pastes above the active layer).
- `parent: P` (`"root"` or a group id): the new layer goes to the **top** of P (doc 10 `add_layer`).
- Both present is a script error. Neither present: the op's default (`place_image`: top of the root;
  `layer_via_copy`: §14.5).

**Errors:** `id` missing / not a string / `""` / `"root"` / taken; `png` missing, not a string, or
not a valid payload (§14.1, §14.2); `x` without `y` or `y` without `x`; `x`, `y` not JSON integers
or outside `-32768..32768`; `name` not a string or not a valid name (§1.1); `above` unknown or
`"root"`; `parent` unknown or not a container; `above` and `parent` together; any other field.

### 14.4 `COPY`: the pixels that Copy / Copy Merged take

`COPY(source)` is a canvas-sized RGBA image computed from the document as it is when the op runs:

```
Src(x, y) = RENDER(root.children, BG)(x, y)     if source is "merged"   # exactly the "png8" output
                                                                        # (doc 10 §5, §9.4: bg included)
            L(x, y), the stored pixel           if source is raster layer L
E(x, y) = effective selection coverage (doc 30 §2: 255 everywhere when there is no selection)

X(x, y) = Src(x, y)                                        if E == 255   # exact short-cuts
          (0, 0, 0, 0)                                     if E == 0
          canonical(Src.R, Src.G, Src.B, q((Src.A / 255.0) * (E / 255.0)))   otherwise
```

For a layer source, `Src` is the layer's **own pixels only**: its mask (enabled or not), visibility,
opacity, fill, blend mode, clipping and `lock_alpha` are ignored, as for `select_alpha` (§7). A
partially selected pixel keeps its colour and has its alpha scaled by the coverage, so a feathered
selection copies with soft edges.

#### 14.4.1 The copied rectangle (what goes on the clipboard)

The clipboard holds `X` cropped to its **content box**: the smallest rectangle
`[bx, by, bw, bh]` that contains every pixel of `X` whose alpha byte is not 0 (transparent pixels
around the copied pixels are trimmed, as Photoshop does), plus that rectangle's canvas position
`(bx, by)`. If every alpha byte of `X` is 0 there is nothing to copy: the GUI reports "the selected
area is empty" and leaves the clipboard unchanged. Paste in Place is then `place_image` with the
cropped pixels at `x = bx, y = by`, which gives the same layer pixels as `layer_via_copy` (§14.5)
would. This crop is used by the GUI and by the core function the GUI calls; no op outputs it
directly, so it is checked by unit tests rather than goldens (§14.8).

### 14.5 `layer_via_copy`

**Semantics.** Photoshop's Layer via Copy / Layer via Cut, and the in-document equivalent of
Copy (or Copy Merged) followed by Paste in Place.

```
source = "merged" if merged else the raster layer named by `layer`
X = COPY(source)                          # §14.4, from the state before the op
if every alpha byte of X is 0: script error ("the selected area is empty")
new raster layer C with pixels X, default properties, C.name = name (default "")
insert C: §14.3.1; default (no above / parent): directly above `layer` when merged is false,
          top of the root when merged is true
if cut: L = CLEAR(L)                      # §14.6, applied to the source layer after C is inserted
```

`merged` and `cut` are booleans, default `false`. `layer` is required when `merged` is false and
must be **absent** when `merged` is true; `merged` together with `cut: true` is a script error
(nothing to cut from a composite). The selection and `Saved` are unchanged (Photoshop keeps the
selection too).

**Errors:** `id` as `place_image`; `layer` missing (merged false), present (merged true), unknown,
or not a raster layer (a group, an adjustment layer, `"root"`); `merged` / `cut` not booleans;
`merged` and `cut` both true; an empty `X`; `name`, `above`, `parent` as `place_image`; any other
field.

### 14.6 `clear`

**Semantics.** Deletes the selected pixels of a raster layer (Edit → Clear, the Delete key), with
soft selection edges fading the pixels out. `CLEAR(L)`:

```
if L.lock_alpha: nothing changes                    # as the eraser (C8a); still one record
else for every pixel: L(x, y) = coverage_lerp(O = L(x, y), F = (0, 0, 0, 0), M = E(x, y))
```

`coverage_lerp` is doc 20 §B0.1 exactly as written there (the `M == 0` and `M == 255` short-cuts,
then the premultiplied mix and the canonical rule), with `F` the transparent pixel and `M` the
effective selection coverage (doc 30 §2), so with no selection the whole layer is cleared. The
layer's mask, visibility and opacity are ignored; the selection and `Saved` are unchanged.

**Errors:** `layer` missing, not a string, unknown, `"root"`, or not a raster layer; any other field.

### 14.7 Render-script ops (strict grammar)

| op | fields (type, default, range) | semantics |
|---|---|---|
| `place_image` | `id` str, required, new; `png` str, required (§14.1–§14.2); `x`, `y` int `-32768..32768`, optional, both or neither (absent = centred); `name` str, default `""`, a valid name (§1.1); `above` str (node id) **or** `parent` str (container), optional | §14.3 |
| `layer_via_copy` | `id` str, required, new; `layer` str (raster), required unless `merged`; `merged` bool, default `false`; `cut` bool, default `false`; `name` str, default `""`; `above` **or** `parent`, optional | §14.5 |
| `clear` | `layer` str, required: a raster layer | §14.6 |

Examples (payload shortened):

```json
{"op":"place_image","id":"p","png":"iVBORw0KGgoAAAANSUhEUgAA…","name":"photo","above":"Layer 1"}
{"op":"place_image","id":"p2","png":"iVBORw0KGgo…","x":-12,"y":40}
{"op":"layer_via_copy","layer":"u","id":"u via copy"}
{"op":"layer_via_copy","layer":"u","id":"u via cut","cut":true}
{"op":"layer_via_copy","merged":true,"id":"m","parent":"g"}
{"op":"clear","layer":"u"}
```

### 14.8 Golden cases

Location: `tests/scripts/place/`, generator `tests/scripts/gen/gen_place.py` (deterministic; it
contains its own PNG writer, which uses **every filter type 0–4** in rotation across rows, splits
`IDAT` into several chunks and inserts ancillary chunks, some with a deliberately wrong CRC, so the
goldens exercise the whole of §14.2). Fixture **K** is §10's (64×64, `k` gradient).

| id | setup | what it catches |
|---|---|---|
| PL-01 | K + `place_image` of a 20×12 image (every alpha 0..255 band, alpha-0 pixels with non-zero colour), default placement | decoding (all five filters, split `IDAT`, skipped ancillary chunks), centring `(22, 26)`, canonicalisation |
| PL-02 | K + the same image at `x = -5, y = 50` | clipping at the canvas edge, negative offsets |
| PL-03 | K + a 67×69 image, default placement | **mutation 43**: `floor_div(-3, 2) = -2`, `floor_div(-5, 2) = -3` (truncation gives -1, -2) |
| PL-04 | K + `u` solid + `t` solid `mul`; `place_image … above u` | placement directly above, under the Multiply layer |
| PL-05 | K + isolated `scrn` group `g` ⊃ `c`; `place_image … parent g` | top of a group, inside its isolation |
| PL-06 | K + a 32×16 opaque solid payload at `(8, 8)` (equal_to PL-06R: K + `add_layer` solid rect `[8, 8, 32, 16]`) | the payload's bytes arrive unchanged |
| PL-07 | K + place; `set_opacity` of the new id 0.5; `set_blend … diff` | the new node is an ordinary raster layer |
| PL-08 | K + place; `undo` (equal_to FIX-K) | one history record |
| PL-09 | K + a payload with a `PLTE`, a zero-length first `IDAT`, and chunks after `IEND` | the permitted §14.2 variations are accepted |
| LVC-01 | K + `u` noise (random alpha); `select_ellipse` anti-aliased; `layer_via_copy u` as `c`; hide `u` | **mutation 44**: alpha scaled by partial coverage |
| LVC-02 | bg `#336699FF`; gradient `k` over the left 40 px only, `u` `mul`, invert `a` (fill 0.3); `select_rect` across `k` and bare background; `layer_via_copy merged` as `m`; hide `k`, `u`, `a` | merged = the png8 composite **including the background**, copied through the selection |
| LVC-03 | K + `u` alpha-ramp rect; feathered rect selection; `layer_via_copy u cut` as `c`; hide `c` | cut: the source is cleared by §14.6 |
| LVC-04 | LVC-03's ops, then hide `u` instead | cut: the copy holds the selected pixels |
| LVC-05 | K + `u` solid + `t` `mul`; rect selection; `layer_via_copy u` as `c`; `set_blend c diff` | default placement directly above the source |
| LVC-06 | K + `u` noise; no selection; `layer_via_copy u` as `c`; `delete_layer u` (equal_to LVC-06R: K + `u`) | no selection copies every byte unchanged |
| LVC-07 | K + `u` with a mask, opacity 0.3, hidden; `select_rect`; `layer_via_copy u` as `c` | mask, opacity and visibility of the source ignored |
| LVC-08 | LVC-03's setup; `layer_via_copy u cut`; `undo` (equal_to LVC-08R: the setup alone) | copy + cut is one record |
| CLR-01 | K + `u` noise; anti-aliased ellipse; `clear u` | **mutation 45**: coverage honoured (partial alpha) |
| CLR-02 | K + `u` noise; `clear u` with no selection (equal_to FIX-K) | no selection clears everything |
| CLR-03 | K + `u` noise; `lock_transparency u`; `select_rect`; `clear u` (equal_to CLR-03R: without the `clear`) | a locked layer is left alone |
| CLR-04 | K + `u` gradient; feathered rect; `clear u`; `undo`; `select_ellipse`; `clear u` | one record; a second clear with another selection |

Error scripts (`"expect": "error"`), each breaking exactly one rule: base64 with a space, a `-`,
bad length, misplaced `=`; bytes that are not a PNG; colour type 2; bit depth 16; Adam7; a wrong
`IDAT` CRC; no `IEND`; an unknown critical chunk; non-consecutive `IDAT`s; `PLTE` after `IDAT`;
decompressed data one byte short and one byte long; filter type 5; width 16385; `x` without `y`;
`x` out of range; `above` with `parent`; `above: "root"`; an id that is taken; a name with a control
character; `layer_via_copy` of a group, of an adjustment layer, with `merged` and `layer`, with
`merged` and `cut`, without `layer`, of an empty selection area; `clear` of a group and of
`"root"`.

The Paste / Paste in Place / Copy crop path of §14.4.1 is covered by unit tests
(`tests/unit/editing/test_place.cpp`: `place_image` of the cropped `COPY` at `(bx, by)` equals
`layer_via_copy`, for layer and merged sources and soft selections) and by the offscreen GUI test
`tests/gui/gui_import.cpp` (real `QClipboard` round trips).

### 14.9 Mutation hooks

| id | defect, precisely | caught by |
|---|---|---|
| 43 | `place_image`'s default centring uses division truncating toward zero (`(W - w) / 2` in C) instead of `floor_div` | PL-03 |
| 44 | `COPY` treats every partially selected pixel as fully selected (`X = Src` whenever `E > 0`) | LVC-01, LVC-04 |
| 45 | `clear` ignores the selection and clears the whole layer | CLR-01, CLR-04, LVC-03 |

### 14.10 Parity notes (feed `docs/PARITY.md`)

1. **Pasted and placed images become raster layers.** Photoshop's Place Embedded (and dropping a
   file on a document) creates a Smart Object and, with the default "Resize Image During Place",
   scales an image larger than the canvas down to fit. Rasterloom places at 100 % as a plain raster
   layer (resize afterwards with Free Transform). **Divergence.**
2. **Content outside the canvas is discarded** when an image larger than the canvas is pasted or
   placed (C8a: canvas-sized layers). Photoshop keeps it in the layer. **Divergence.**
3. **Copy trims transparent borders** (§14.4.1) and copies soft selection edges as partial alpha:
   this matches Photoshop's documented behaviour as far as the public User Guide describes it; the
   exact alpha arithmetic of partially selected pixels is not published. **Unknown** in the last
   bit.
4. **Copy with no selection** copies the whole layer (as after Select All) and **Clear needs a
   selection in the GUI** (the op itself clears everything). Photoshop's menu state without a
   selection was not checked. **Unknown.**
5. **Clear on a transparency-locked layer** does nothing here; Photoshop fills the selection with
   the background colour. **Divergence.**
6. **Paste position**: centred on the active selection's bounds if there is one, else on the
   visible part of the canvas; Paste in Place uses the copied pixels' original position (for
   images copied in other applications, which carry no position, it behaves as Paste). The
   selection-centred rule is Photoshop's behaviour as commonly described; not verified against
   Photoshop. **Unknown.**
7. **Colour management**: gamma, `sRGB`, `iCCP` and other colour information in pasted or placed
   images is ignored (bytes are used as stored), as everywhere in v0.1. **Divergence.**
8. **Copy Merged includes the canvas background** (`bg`, doc 10 §9.4), because it is part of the
   composite the user sees; Photoshop's Background layer is an ordinary layer and is included too.
