# Render-script golden corpus

Every `*.json` file under `tests/scripts/<suite>/` is a **render script** in the grammar of
`docs/math/` (shape fixed by doc 10 section 11:
`{"canvas":{"w":int,"h":int,"bg":"#RRGGBBAA"}, "ops":[...], "out":"png8"}`). The harness renders
each script with `rasterloom-cli` and with `tests/reference/refcomp.py` and requires the two PNGs to
be byte-identical (`tests/tools/pixcmp.py`, decoded RGBA byte equality). That agreement is the
project's central proof, so these scripts are written from `docs/math/` only, by authors who read
neither `src/` nor `tests/reference/`.

## Layout

| directory | doc | contents | generator |
|---|---|---|---|
| `compositing/` | 10 | blend modes, fill/opacity, groups, clipping, masks, merge/flatten | (owned by the doc-10 golden author) |
| `adjust_filters/` | 20 | Part C: adjustment goldens A01–A24, filter goldens G01–G19 | `gen/gen_adjust_filters.py` |
| `geometry/` | 30 | section 20: SEL-01–24, TR-01–14, RA-01–10, GR-01–04, BK-01–04 | `gen/gen_geometry.py` |
| `brush/` | 40 | section 9: B01–B27 | `gen/gen_brush.py` |
| `editing/` | 60 | section 10: DEL, NAME, DUP, ADJ, GM, SAL, HIST (+ `*R` equal_to targets); staged in `.pending-editing/` until the doc-60 ops are implemented, then `gen_editing.py --activate` | `gen/gen_editing.py` |
| `gen/` | — | generators and `check_scripts_203040.py` (static op/field checker for the three suites above) | — |
| `stats/` | BUILD-SPEC tile engine | tile-allocation goldens: ordinary render scripts whose upper bounds on allocated tiles live in `stats/_expectations.json` (checked through `rasterloom-cli --stats`; the only golden-level catch for mutation 13) | hand-written |

Other directories (for example `smoke/`) belong to their own authors.

The generated files are checked in, but they are **outputs**: edit the generator and re-run it,
never hand-edit a generated `.json`. Each generator deletes every `*.json` in its own directory and
rewrites it; the output is byte-identical on every run (no randomness, no clock). Regenerate and
check with:

```sh
cd tests/scripts/gen
python3 gen_adjust_filters.py && python3 gen_geometry.py && python3 gen_brush.py
python3 gen_editing.py                 # doc 60; also runs its own structural checker
python3 check_scripts_203040.py        # exit 0 = every valid golden uses only doc-defined ops/fields
```

## Naming

- Doc 20 names its goldens in a `file` column: the file is that name, e.g. `adj_levels_identity.json`
  (A01), `flt_gauss_r3.json` (G01). The doc's A/G number is in the generator next to each entry.
- Docs 30 and 40 name goldens by id only: the file is the id, e.g. `SEL-01.json`, `TR-14.json`,
  `B23.json`.
- `err_<what>.json` (any suite): a script the docs define as a **script error** (see below). These
  are not in the docs' golden tables; each one's generator entry cites the doc sentence that makes
  it an error.

## Harness-only top-level keys

Two top-level keys are **harness metadata**, not part of the render-script grammar. Doc 10 section
11 fixes the script shape and 00-conventions C9 makes any unknown field a script error, so the
**harness removes these keys before it hands the script to either renderer**:

- `"expect": "error"` — the script is deliberately invalid (00-conventions C9: unknown op or field,
  wrong type, out-of-range value, unknown id or wrong node kind, singular matrix, undo past the
  start of history, ...). The case passes when **both** renderers exit non-zero and neither writes a
  PNG. Rendering successfully, or only one renderer failing, is a failure. Without the key the
  script must render (exit 0) and the PNGs must be byte-equal.
- `"equal_to": "<stem>"` — besides the usual CLI-vs-reference comparison, this script's PNG must be
  byte-equal to the PNG of the named script in the same directory (for example `brush/B07.json` has
  `"equal_to": "B05"`: view zoom must not enter the brush math, doc 40 section 9).

No other top-level keys exist. Files whose name starts with `_` (for example
`stats/_expectations.json`) are harness manifests, never render scripts. A renderer that is given one of these keys directly is right to
reject the script.

## Behaviour checks expressed as pixels

Goldens that the docs describe as behaviour checks are still ordinary render scripts whose output
pixels encode the behaviour, for example `brush/B23.json` (two strokes, then `{"op":"undo"}`: the
correct PNG shows the first stroke only, so per-dab history is visible as a pixel difference) and
`geometry/GR-04.json` (a zero-length gradient must leave the grey fill untouched).
