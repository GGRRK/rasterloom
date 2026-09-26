# Rasterloom normative math — 40: brush engine, eraser, clone stamp, stroke recording, undo

Status: normative for v0.1. Inherits every rule of `00-conventions.md` (binary64, evaluation
order exactly as written, the single quantiser `q`, libm scalars for transcendentals, straight
alpha, canonical transparent pixel, row-major order, splitmix64). Where this file says "doc 10" it
means the compositing doc (`10-*.md`), which owns layers, masks, lock-transparency and the core
ops; "the selections doc" means the doc that owns the selection mask and its ops.

Contract source: BUILD-SPEC requirement 5, decisions D2 and D5, mutation seeds 7, 8, 9, 10.

Notation used below:

- `rhu(x)` = round half up to an integer: `fl = floor(x); return fl + ((x - fl) >= 0.5)`.
  For `x >= 0` this is exactly the rounding step of `q` in C2. C++ must write it this way (not
  `std::round`, which rounds half away from zero and differs for negative `x`); Python writes
  `fl = math.floor(x); fl + (1 if (x - fl) >= 0.5 else 0)`.
- `q(x)` is C2's quantiser: `rhu(clamp(x, 0.0, 1.0) * 255.0)`, a byte.
- `PI = 3.141592653589793`.
- All quantities are doubles unless called a byte, an integer or a bool.

---

## 1. Semantics (what the user sees)

A **stroke** is everything between pointer-down and pointer-up (one gesture). While the pointer
moves, the engine places round (or elliptical) **dabs** along the path at a fixed spacing that is
a fraction of the dab diameter. Each dab is a soft-or-hard disc whose falloff is set by
**hardness**.

Dabs do not paint onto the layer directly. They deposit into a per-stroke **stroke buffer** `B`,
one double per canvas pixel, starting at 0 at pointer-down. The stroke buffer is then composited
onto the layer's pixels **as they were at pointer-down** (the snapshot `S0`), never onto the
previous frame's result. This is what makes flow and opacity behave:

- **Opacity** is a ceiling for the whole stroke. In **Wash** mode (the default), no amount of
  overlapping within one stroke — including a stroke that crosses itself — takes the stroke above
  its opacity.
- **Flow** is how far each dab moves the stroke buffer *toward* its ceiling: a lerp, never a
  repeated source-over. At flow 1 a single dab reaches the ceiling at its hard core; at flow
  0.25 roughly four overlapping dabs are needed to approach it. The dab's soft falloff scales the
  rate, so soft edges accumulate smoothly.
- In **Build-up** mode the ceiling is lifted to full coverage and opacity instead scales each
  dab's rate (opacity behaves like flow), so a stroke that re-covers an area keeps darkening toward
  full strength.
- A new stroke starts a fresh buffer, so a second stroke over the first *does* accumulate (that is
  ordinary layering of two gestures, as users expect).

Tablet **pressure** can drive dab size and dab opacity through two user curves. Mouse input has
pressure 1.0. The **airbrush** term adds dabs over time while the pointer is held, and the
**stabilizer** smooths the pointer path with an exponential moving average.

The **eraser** uses the same engine; its buffer reduces alpha instead of adding colour. The
**clone stamp** uses the same engine; its buffer is the coverage with which pixels copied from an
offset source position are laid down, the source being read from the stroke-start snapshot.

A selection clips all three tools: the selection coverage multiplies the stroke buffer at
composite time. Lock-transparency preserves the layer's alpha (brush, clone) or makes the eraser
a no-op.

Undo: one whole stroke is one history record, regardless of how many dabs it placed.

Nothing in this math depends on the view (zoom, pan, rotation, screen DPI). Samples are recorded
in canvas coordinates; the view transform is applied by the GUI before recording and never again.

---

## 2. Render-script ops

The render-script shape is fixed: `{"canvas":{...}, "ops":[...], "out":"png8"}`. This doc owns
`brush_stroke`, `eraser_stroke`, `clone_stroke`, and proposes `undo` (section 2.5; doc 10 wins if it
defines `undo` differently — see open questions).

### 2.0 Common rules

- JSON numbers (integer or fractional) are read as doubles unless a field is typed `int`.
  `NaN`/`Infinity` are not valid JSON and are rejected by both parsers.
- A field outside its stated range is a **script error** (the CLI exits non-zero, `refcomp.py`
  raises); values are never silently clamped, except sample `pressure` (clamped, see 2.1),
  because real hardware reports slightly out-of-range pressure.
- Unknown fields in these ops are a script error (typo protection).
- `"layer"` names a raster layer created by `add_layer` (doc 10). A group id, an unknown id, or
  a layer that is not a raster layer is a script error.
- All brush fields below are constant for the whole stroke (there are no per-dab dynamics other
  than the two pressure curves).

### 2.1 Sample object (shared by all three stroke ops)

```json
{"x": 10.0, "y": 20.5, "pressure": 1.0, "tilt_x": 0.0, "tilt_y": 0.0, "t_ms": 0.0}
```

| field | type | default | range / rule |
|---|---|---|---|
| `x`, `y` | double | required | finite, `-1e6 <= v <= 1e6`; canvas coordinates (C4: pixel `(i, j)` covers `[i, i+1) × [j, j+1)`); may lie outside the canvas |
| `pressure` | double | `1.0` | any finite value, clamped with `clamp(p, 0.0, 1.0)` when read |
| `tilt_x`, `tilt_y` | double | `0.0` | finite, degrees, `[-90, 90]`; recorded for replay fidelity, **unused by v0.1 math** |
| `t_ms` | double | previous sample's `t_ms` (first sample: `0.0`) | finite, `>= 0`; milliseconds since an arbitrary stroke origin |

`samples` must contain at least 1 sample.

### 2.2 `brush_stroke`

```json
{"op": "brush_stroke", "layer": "fg", "target": "pixels",
 "color": "#1E3A8A", "size": 24.0, "hardness": 0.0, "spacing": 0.25,
 "opacity": 0.6, "flow": 0.25, "angle": 0.0, "roundness": 1.0, "mode": "wash",
 "dabs_per_second": 0.0, "smoothing": 0.0,
 "size_curve": null, "opacity_curve": null, "view_zoom": 1.0,
 "samples": [{"x": 20, "y": 32}, {"x": 108, "y": 32, "t_ms": 40}]}
```

| field | type | default | range / meaning |
|---|---|---|---|
| `layer` | string | required | raster layer id |
| `target` | string | `"pixels"` | `"pixels"` or `"mask"` (paint the layer's mask; error if the layer has no mask) |
| `color` | string | `"#000000"` | `"#RRGGBB"` or `"#RRGGBBAA"` with `AA` = `FF` (any other `AA` is an error); hex case-insensitive |
| `size` | double | `20.0` | `[1.0, 5000.0]`; nominal dab **diameter** in canvas pixels |
| `hardness` | double | `1.0` | `[0.0, 1.0]` |
| `spacing` | double | `0.25` | `[0.01, 10.0]`; fraction of the dab diameter |
| `opacity` | double | `1.0` | `[0.0, 1.0]` |
| `flow` | double | `1.0` | `[0.0, 1.0]` |
| `angle` | double | `0.0` | `[-360.0, 360.0]` degrees, counter-clockwise on screen |
| `roundness` | double | `1.0` | `[0.01, 1.0]`; minor/major axis ratio |
| `mode` | string | `"wash"` | `"wash"` or `"buildup"` |
| `dabs_per_second` | double | `0.0` | `[0.0, 1000.0]`; airbrush rate, 0 = off |
| `smoothing` | double | `0.0` | `[0.0, 0.99]`; EMA stabilizer strength, 0 = off |
| `size_curve` | curve or null | `null` | pressure→size curve (section 3.3); `null` = factor 1.0 |
| `opacity_curve` | curve or null | `null` | pressure→opacity curve; `null` = factor 1.0 |
| `view_zoom` | double | `1.0` | `(0.0, 256.0]`; the GUI zoom at recording time. **Informational only: it must not enter any formula** (it exists so mutation 9 is observable) |
| `samples` | array of sample | required | ≥ 1 sample, in input order |

A **curve** is a JSON array of 2 to 16 points `[x, y]`: each `x`, `y` in `[0.0, 1.0]`, `x` strictly
increasing, first `x` exactly `0.0`, last `x` exactly `1.0`. Anything else is a script error.
(The GUI curve editor holds up to 16 control points; the identity curve is `[[0,0],[1,1]]`.)

### 2.3 `eraser_stroke`

Same fields as `brush_stroke` **except**: no `color`, no `target` (always the layer's pixels;
erasing a mask is out of scope in v0.1 — paint the mask black with `brush_stroke` instead).

### 2.4 `clone_stroke`

Same fields as `brush_stroke` **except** no `color` and no `target` (always pixels), plus:

| field | type | default | range / meaning |
|---|---|---|---|
| `source` | `[x, y]` doubles or absent | absent | sets the clone source point (the GUI's Alt-click), canvas coords, each in `[-1e6, 1e6]`; resets the stored offset (section 4) |
| `aligned` | bool | `true` | aligned mode (section 4) |
| `source_layer` | string | the op's `layer` | raster layer to sample from (v0.1: one layer; no "all layers" sampling) |

A `clone_stroke` with no `source` when no earlier `clone_stroke` in the script set one is a script
error.

### 2.5 `undo` (proposed here; owned by doc 10 if doc 10 defines history)

```json
{"op": "undo", "steps": 1}
```

`steps`: int, default `1`, range `[1, 1000]`, must not exceed the number of history records
available (else script error). Semantics in section 6.

---

## 3. The stroke pipeline, in evaluation order

A stroke op executes these steps in this order. The GUI executes the same steps through the same
core API (`begin_stroke`, `add_sample` per input event, `end_stroke`); **the CLI must drive
`brush_stroke` through that same API, one `add_sample` call per JSON sample**, not through a batch
shortcut, so that per-event state (mutation 8) and history granularity (mutation 10) are exercised
by the headless goldens.

### 3.1 Stroke start (`begin_stroke`)

1. Validate the op (section 2). Resolve the target layer.
2. Take the snapshot `S0` = the target's pixels (or mask, for `target: "mask"`) as they are now.
   For `clone_stroke` take `SRC` = snapshot of `source_layer` now (if `source_layer` is the target,
   `SRC` is `S0`). Snapshots are immutable for the rest of the stroke (in C++ they are the CoW
   `shared_ptr<const Tile>` handles already held for undo, D5).
3. Bake the pressure LUTs (3.3).
4. Rotation scalars, once per stroke, libm scalars:
   ```
   theta = angle * (PI / 180.0)
   ca = cos(theta)
   sa = sin(theta)
   ```
   (`angle = 0` gives exactly `ca = 1.0`, `sa = 0.0`.)
5. Allocate the stroke buffer `B`: one double per canvas pixel, all `0.0` (sparse by tile in C++;
   a canvas-sized `float64` array in the reference). Pixels outside the canvas do not exist.
6. Initialise the walk state: `frac = 0.0`, `step` unset, `dab_count = 0`.
7. Clone only: resolve the integer offset `(oxi, oyi)` (section 4) using the first sample.

### 3.2 Sample preprocessing (per `add_sample`)

Samples are indexed `k = 0, 1, ...` in input order. Raw values: `xr_k, yr_k, p_k` (pressure after
the clamp), `traw_k`.

Effective time (monotone): `te_0 = traw_0`; for `k >= 1`: `te_k = max(te_{k-1}, traw_k)`.

**EMA stabilizer** (positions only; pressure, tilt and time are never smoothed). With
`m = smoothing` and `a = 1.0 - m` computed once per stroke:

```
xs_0 = xr_0                       ys_0 = yr_0
xs_k = (a * xr_k) + (m * xs_{k-1})    ys_k = (a * yr_k) + (m * ys_{k-1})      for k >= 1
```

With `smoothing = 0` this is exactly `xs_k = xr_k` (`1.0*x + 0.0*y` is exact). There is **no
catch-up** at pointer-up: the stroke ends at the last smoothed position (Parity notes).

The brush state carried from sample to sample is `(xs, ys, p, te)` — smoothed position, raw
pressure, effective time.

### 3.3 Pressure curves and LUTs (baked once per stroke)

For a curve with points `(X_0, Y_0) ... (X_{n-1}, Y_{n-1})`, `2 <= n <= 16`, bake a 256-entry
`uint16` table `LUT[0..255]`:

```
for i in 0..255:
    x = i / 255.0
    k = the largest index in [0, n-2] with X_k <= x
    t = (x - X_k) / (X_{k+1} - X_k)
    y = Y_k + (t * (Y_{k+1} - Y_k))
    LUT[i] = rhu(clamp(y, 0.0, 1.0) * 65535.0)        # integer 0..65535
```

The curve is **piecewise linear** between control points. The denominator is never zero because
`X` is strictly increasing.

Lookup for a pressure `p` (already in `[0,1]`):

```
i = q(p)                     # = rhu(p * 255.0), integer 0..255
factor = LUT[i] / 65535.0
```

A `null` curve means `factor = 1.0` exactly (no table lookup). Two factors per dab:
`fs` (size, from `size_curve`) and `fo` (opacity, from `opacity_curve`).

The identity curve bakes to `LUT[i] = 257 * i`, so `factor = i / 255` up to the division's rounding.

### 3.4 Dab placement: the spacing walk

Adapted from libmypaint's `count_dabs_to()` and the dab loop of `mypaint_brush_stroke_to()`
(ISC licence; <https://github.com/mypaint/libmypaint/blob/master/mypaint-brush.c>, functions
`count_dabs_to` and `mypaint_brush_stroke_to`): a fractional dab count `frac` ("partial dabs")
carries across input events; the remaining dab count to the next event is distance/spacing plus
time × dabs-per-second; each dab consumes one unit, the first dab of an event consumes
`1 - frac`, and positions are linearly interpolated. Rasterloom differs from libmypaint in that the
distance term uses only the actual dab diameter (libmypaint also adds a base-radius term and a
per-radius term), and the step length is fixed at the last placed dab.

Definitions:

```
r_air = dabs_per_second
dab_params(p) -> (d, ...)       # section 3.5; d = size * fs is the *unclamped* diameter
next_step(d) = spacing * max(d, 1.0)
```

**First sample** (`k = 0`): place one dab at `(xs_0, ys_0)` with pressure `p_0`. Then
`step = next_step(d_of_that_dab)`, `frac = 0.0`. A one-sample stroke (a click) is exactly one dab.

**Each later sample** (`k >= 1`) walks the segment from state `A = (xs_{k-1}, ys_{k-1}, p_{k-1},
te_{k-1})` to `Bs = (xs_k, ys_k, p_k, te_k)`:

```
dx = xs_k - xs_{k-1}
dy = ys_k - ys_{k-1}
L  = sqrt((dx * dx) + (dy * dy))          # canvas pixels. NEVER multiplied by view_zoom.
D  = (te_k - te_{k-1}) / 1000.0            # seconds, >= 0
u  = 0.0
loop:
    w    = 1.0 - u
    todo = ((L * w) / step) + ((D * w) * r_air)
    if (frac + todo) < 1.0:
        break
    need = 1.0 - frac
    u = u + (w * (need / todo))
    if u > 1.0: u = 1.0
    x = xs_{k-1} + (u * dx)
    y = ys_{k-1} + (u * dy)
    p = p_{k-1} + (u * (p_k - p_{k-1}))
    place dab at (x, y) with pressure p            # sections 3.5 - 3.7
    frac = 0.0
    step = next_step(d_of_that_dab)
frac = frac + todo                                 # todo from the iteration that broke
```

Notes that are normative:

- `frac` and `step` persist across `add_sample` calls for the whole stroke. They are **never**
  reset by an input event (mutation 8 resets `frac`).
- `todo` is recomputed after every dab with the new `step` (the dab just placed sets the spacing to
  the next one).
- When `L = 0` and (`D = 0` or `r_air = 0`), `todo = 0` and the loop exits immediately; `frac`
  cannot reach 1 without a positive `todo`, so `need / todo` never divides by zero.
- The invariant `0 <= frac < 1` holds after every sample.
- `p` is interpolated between the two samples' **raw** pressures; positions between the
  **smoothed** positions.
- Dab counter: every placed dab increments `dab_count` (including dabs with `d <= 0`). If
  `dab_count` would exceed `1 000 000` the op is a script error (both implementations must raise;
  this bounds pathological spacing/rate combinations).
- A dab whose centre is outside the canvas is still placed (it counts, it sets `step`); it simply
  touches no pixels, or only those that lie inside the canvas.

### 3.5 Per-dab parameters

For a dab at `(cx, cy)` with pressure `p`:

```
fs = size-curve factor of p      (1.0 if size_curve is null)
fo = opacity-curve factor of p   (1.0 if opacity_curve is null)
d  = size * fs                   # unclamped diameter; feeds next_step
if d <= 0.0: the dab touches no pixels (still counted, step = next_step(d) = spacing)
d_draw = max(d, 1.0)
k_small = min(d, 1.0)            # fades dabs thinner than one pixel instead of drawing them at full strength
O  = (opacity * fo) * k_small    # the dab's opacity ceiling
R  = d_draw / 2.0                # major semi-axis
Rm = R * roundness               # minor semi-axis
w  = (1.0 / Rm) if Rm > 1.0 else 1.0     # one pixel of the minor axis, in normalised radius
he = min(hardness, 1.0 - w)      # effective hardness: the edge band is at least one pixel wide
f  = flow
```

### 3.6 Dab mask (coverage `m` per pixel)

Pixels touched by a dab: integer `px` in `[ceil((cx - R) - 0.5), floor((cx + R) - 0.5)]` and
integer `py` in `[ceil((cy - R) - 0.5), floor((cy + R) - 0.5)]`, intersected with the canvas.
Every other pixel is untouched by this dab (not even with `m = 0`). This is the set of pixels whose
centre lies in the closed square `[cx - R, cx + R] × [cy - R, cy + R]`, which contains the ellipse
because `Rm <= R`.

For each touched pixel, in any order (pixels are independent within a dab):

```
ddx = (px + 0.5) - cx
ddy = (py + 0.5) - cy
uu  = (ddx * ca) - (ddy * sa)          # along the major axis
vv  = (ddx * sa) + (ddy * ca)          # along the minor axis
nu  = uu / R
nv  = vv / Rm
r   = sqrt((nu * nu) + (nv * nv))      # normalised elliptical radius; 1.0 = dab edge
if r >= 1.0:        m = 0.0
elif r <= he:       m = 1.0
else:
    t = (r - he) / (1.0 - he)
    m = 1.0 - ((t * t) * (3.0 - (2.0 * t)))       # 1 - smoothstep(t)
```

Axis convention: with `y` growing downward (C4), the major axis points along `(ca, -sa)`, i.e.
`angle = 90` is a vertical major axis and positive angles rotate counter-clockwise on screen.

`1.0 - he >= w > 0` whenever the `else` branch runs, so the division is safe. No transcendental is
evaluated per pixel (`sqrt` is correctly rounded, C1).

**AA rule at the dab edge**: the mask reaches exactly 0 at the nominal ellipse (`r = 1`) and the
transition band is never narrower than one pixel measured along the minor axis (`he <= 1 - w`).
A `hardness = 1` dab therefore has a one-pixel smoothstep edge ending on its nominal radius; a dab
with `Rm <= 1` is all falloff (`he = 0`). No supersampling is used.

### 3.7 Stroke-buffer accumulation (the flow model)

For each touched pixel of each dab, in dab order, update `B` at that pixel:

**Wash** (`mode = "wash"`): the ceiling is the dab's opacity `O`; the mask shapes the *rate*.
```
if O > B:
    B = B + ((f * m) * (O - B))
```

**Build-up** (`mode = "buildup"`): the ceiling is full coverage, and opacity scales the rate like
flow. `kb = f * O` is computed once per dab.
```
B = B + ((kb * m) * (1.0 - B))
```

Properties (informative, follow from the formulas): `0 <= B <= 1` always; in Wash, `B <= O` (the
dab's opacity, which is the stroke's opacity unless the opacity curve varies it) no matter how often
the stroke re-covers a pixel; `B` never decreases within a stroke; where several dabs overlap, the
stroke builds up as `1 - Π(1 - f·m_i)` toward the ceiling, so a soft edge is the smooth sum of
the overlapping falloffs. In Build-up at a pixel with `m = 1` the update is a source-over of alpha
`kb` onto the stroke buffer, and a stationary soft airbrush slowly fills out toward full coverage.

**Revised 2026-09-26.** The first version of this section capped each dab at its own mask-shaped
ceiling (`T = O * m`, Build-up at `m`). At flow 1 that makes the stroke edge the *maximum* of the
overlapping dab falloffs instead of their accumulation, and soft strokes showed visible beading
between dab centres at the default 25 % spacing (seen in the GUI contact sheet). This revision keeps
the spec's rule (flow is a lerp toward the opacity ceiling, never a repeated source-over onto the
layer) with the mask moved from the ceiling into the rate, which is how Krita's alpha-darken Wash
mode behaves.

This is **not** repeated source-over of each dab onto the layer (mutation 7 is exactly that
defect).

Worked accumulation, `opacity 0.6, flow 0.25`, same pixel with `m = 1` hit by three dabs:
Wash `0.15, 0.26249999999999996, 0.34687499999999993` (converges to 0.6); Build-up
`0.15, 0.27749999999999997, 0.38587499999999997` (converges to 1.0).
The same with `m = 0.5` (a soft edge): Wash `0.075, 0.140625, 0.198046875` (converges to 0.6); Build-up
`0.075, 0.144375, 0.208546875` (converges to 1.0).

### 3.8 Composite: stroke buffer onto the snapshot

Performed per canvas pixel, for pixels with `B > 0.0`. Every other pixel keeps its `S0` bytes
unchanged (a byte copy, no re-quantisation).

Selection: `s = sel / 255.0` where `sel` is the document's current selection-mask byte at the pixel
(the selections doc owns it); with no active selection `s = 1.0` exactly. **Selection is applied
here, once, at composite time, by multiplying the stroke buffer** — not inside the per-dab
accumulation. Where `s = 0` the pixel is a byte copy of `S0`. (Multiplying the source alpha by `s`
is the same as lerping between the unpainted and fully painted results in premultiplied space.)

Decode the snapshot pixel: `Cd = v / 255.0` per colour channel, `ad = A / 255.0` (C2).
Brush colour: `Cs = hex_byte / 255.0` per channel.

`lock` is the target layer's lock-transparency flag as defined in doc 10.

**Brush, `target = "pixels"`** (Normal source-over in straight alpha, W3C Compositing-1 §5.1 simple
alpha compositing, `co = cs·αs + cb·αb·(1 − αs)`, `αo = αs + αb·(1 − αs)`,
<https://www.w3.org/TR/compositing-1/#simplealphacompositing>; brush blend modes other than Normal
are not in v0.1):

```
a_s = B * s
if a_s == 0.0: byte copy of S0
if lock and A == 0: byte copy of S0                   # nothing to colour
if lock:                                              # doc 10 §10: source-atop, alpha kept
    Co  = (Cd * (1.0 - a_s)) + (Cs * a_s)             # per channel R, G, B
    out = (q(Co_R), q(Co_G), q(Co_B), A)              # alpha byte copied unchanged
else:
    a_o = a_s + (ad * (1.0 - a_s))                    # > 0 because a_s > 0
    Co  = ((Cs * a_s) + ((Cd * ad) * (1.0 - a_s))) / a_o  # per channel R, G, B
    out = (q(Co_R), q(Co_G), q(Co_B), q(a_o)), then canonical-transparent rule (C3)
```

With lock-transparency the destination is treated as opaque (doc 10 §10, the same rule the
gradient tool and paint bucket use in doc 30): the colour moves toward `Cs` by exactly `a_s`, and
the alpha byte is restored — painting recolours existing pixels and never creates or removes
coverage. (Resolved 2026-09-26: an earlier draft used the unlocked source-over colour here, which
contradicted doc 10.)

**Eraser** (alpha reduction):

```
if lock: byte copy of S0 for every pixel (the eraser changes nothing on a locked layer)
a_s = B * s
if a_s == 0.0: byte copy of S0
a_o = ad * (1.0 - a_s)
out = (R, G, B bytes of S0 unchanged, q(a_o)), then canonical-transparent rule (C3)
```

**Clone stamp**: the source pixel for destination pixel `(px, py)` is `SRC(px + oxi, py + oyi)`
(section 4), read from the stroke-start snapshot; out-of-canvas source reads are `(0,0,0,0)` (C4).
Decode it as `Cc` (per channel) and `ac`. Then:

```
a_s = (B * s) * ac
if a_s == 0.0: byte copy of S0
then exactly the Brush formula above with Cs := Cc (per channel), including the lock rule
```

The dab mask is the clone's coverage; the source's own alpha multiplies it.

**Brush, `target = "mask"`** (painting a layer mask; the mask is one byte `M8` per pixel, as
stored by doc 10):

```
g  = ((0.30 * Cs_R) + (0.59 * Cs_G)) + (0.11 * Cs_B)     # D3 luma of the brush colour
a_s = B * s
if a_s == 0.0: byte copy
Mv = M8 / 255.0
out = q(Mv + (a_s * (g - Mv)))
```

Lock-transparency does not apply to a mask.

### 3.9 Preview during the stroke vs commit at stroke end

- **Commit** (`end_stroke`): run 3.8 once over the final `B` and write the result into the target
  as ONE history record (section 6). This is the only thing that affects render-script output.
- **Preview** (GUI only, after any `add_sample`): the GUI may show `composite(S0, B_current)` for
  dirty tiles using exactly 3.8. Each preview is computed from `S0` and the current `B`, **never
  from the previous preview**, so previews never compound and the committed result is independent
  of how many previews were shown or which tiles were cached.

---

## 4. Clone stamp source state

The render state (and the GUI tool state) holds `clone_src` (a point or none) and `clone_off`
(an integer pair or none), both initially none. They are tool state, not document state: `undo`
does not change them.

At `begin_stroke` of a `clone_stroke`, with `(x0, y0)` = the first sample's raw position:

```
if the op has "source":  clone_src = source; clone_off = none
if clone_src is none:     script error
if aligned and clone_off is not none:
    use clone_off
else:
    clone_off = (rhu(clone_src.x - x0), rhu(clone_src.y - y0))
    use clone_off
(oxi, oyi) = clone_off
```

- **Aligned** (`true`): the offset is fixed by the first stroke after the source was set and every
  later aligned stroke reuses it, so the sampled region moves with the brush across strokes.
- **Non-aligned** (`false`): every stroke re-derives the offset from the source point, so each
  stroke starts sampling at the source point again.
- The offset is an integer pixel offset (`rhu`), so cloning is a pure pixel copy with no
  resampling.
- The source is sampled from `SRC`, the snapshot taken at stroke start. Pixels painted earlier in
  the same stroke are never re-sampled (no feedback "echo" when source and destination overlap).

---

## 5. Input recording (GUI → samples)

This section fixes how the GUI produces the sample stream, so a recorded GUI stroke replays
identically in the CLI and the reference.

- **One sample per accepted input event.** The canvas accepts every `QTabletEvent`
  unconditionally (BUILD-SPEC req 5) and every mouse move/press/release of the drawing button.
  The press is sample 0; every move is one sample; the release contributes a final sample only if
  its position or time differs from the last sample.
- **Coordinates**: the event's sub-pixel widget position (`QPointF`) is mapped through the inverse
  view transform (zoom, pan, rotation) to canvas coordinates *before* recording. After that point
  no view quantity is used. `view_zoom` is recorded in the op for diagnostics only.
- **Pressure**: tablet `pressure()` as reported; **mouse input records `pressure = 1.0`,
  `tilt_x = tilt_y = 0.0`**. A tablet eraser end (`pointerType() == Eraser`) selects the eraser
  tool's parameters; it does not change the math.
- **Time**: `t_ms = event.timestamp() - timestamp_of_sample_0` (milliseconds, as a double).
- **Airbrush timer**: when `dabs_per_second > 0` and the pointer is down, the GUI appends a
  synthetic sample every 16 ms of wall time with no input event (same raw position and pressure as
  the last sample, current `t_ms`). These synthetic samples are recorded in the stream like real
  ones, so replay needs no timer. A stationary pointer then produces dabs through the time term of
  3.4 alone. (With the stabilizer on, repeated samples also let the smoothed position converge on
  the pointer while it is held still.)
- **Stroke end**: pointer-up, tablet proximity leave, focus loss, or tool switch all end the
  stroke with `end_stroke` (commit). There is no stroke cancel in v0.1.

---

## 6. Undo semantics (BUILD-SPEC D5, req 12)

- `begin_stroke` opens a memento holding the CoW snapshot handles; `end_stroke` commits **exactly
  one** history record for the whole stroke, whatever the number of samples or dabs, including a
  stroke that changed no pixel (for example a click outside the canvas). No history record is ever
  created per dab or per input event.
- Undoing the record restores the target (pixels or mask) to `S0` exactly (byte-identical); redo
  restores the committed result exactly.
- Render scripts: every document-changing op pushes exactly one history record (this doc requires
  it for its three stroke ops; doc 10 states it for the others). `{"op":"undo","steps":n}` pops `n`
  records and restores the document to its state before them. The reference implements this by
  keeping a copy of the document state before each history-producing op. `undo` itself is not a
  history record. v0.1 scripts have no `redo` op.
- Depth: 50 records by default (configurable 1–1000); render scripts run with depth 1000.

How mutation 10 is detected: (a) golden `B23` (two strokes, then `undo` of one step — correct output
shows the first stroke only; with per-dab records one undo removes only the last dab);
(b) core selftest `brush_history_granularity`: on a 64×64 document, `history_size()` before, run a
100-sample `brush_stroke` that places ≥ 40 dabs through `begin_stroke/add_sample/end_stroke`,
assert `history_size()` increased by exactly 1, then `undo()` once and assert the layer tiles equal
the pre-stroke tiles byte for byte.

---

## 7. Edge cases (normative summary)

| case | behaviour |
|---|---|
| one sample | exactly one dab at that sample |
| repeated identical samples, `dabs_per_second = 0` | no additional dabs |
| decreasing `t_ms` | `te` is held (`te_k = max(te_{k-1}, t_k)`), so `D = 0` for that segment |
| pressure 0 with a size curve giving 0 | `d = 0`: dab counted, touches nothing, next step = `spacing * 1.0` |
| `d` in `(0, 1)` | drawn at diameter 1 with ceiling scaled by `d` (`k_small`) |
| `opacity = 0` or `flow = 0` | `B` stays 0 everywhere; the layer is unchanged (byte copy); one history record still pushed |
| dab partly or wholly off-canvas | only in-canvas pixels are touched; dab still counts |
| samples outside the canvas | allowed; the walk runs normally |
| `roundness < 1`, `angle = 0` | `ca = 1, sa = 0`: major axis horizontal |
| `B > 0` but `s = 0` | byte copy |
| result alpha quantises to 0 | stored as `(0,0,0,0)` (C3) |
| lock-transparency, brush/clone on alpha-0 pixel | byte copy (stays transparent) |
| lock-transparency, eraser | entire op is a no-op on pixels (history record still pushed) |
| clone source out of canvas | source reads `(0,0,0,0)` → `a_s = 0` → byte copy |
| `target = "mask"` on a layer with no mask | script error |
| more than 1 000 000 dabs in one stroke | script error |
| `smoothing = 0` | positions are exactly the raw positions |

---

## 8. Worked examples (implementations must reproduce these doubles exactly)

**Dab walk.** `size 8, spacing 0.25, dabs_per_second 20, smoothing 0.5, size_curve [[0,0],[1,1]]`,
samples `(10,10,p=1,t=0)`, `(13,14,p=0.5,t=20)`, `(13,14,p=0.5,t=120)`. Smoothed positions:
`(10,10)`, `(11.5,12)`, `(12.25,13)`. Dabs `(x, y, p, d)` printed with Python `repr`:

```
10.0 10.0 1.0 8.0
10.90909090909091 11.212121212121213 0.696969696969697 5.584313725490196
11.535483870967742 12.047311827956989 0.5 4.015686274509804
11.766600331996932 12.355467109329243 0.5 4.015686274509804
11.997716793026123 12.663622390701498 0.5 4.015686274509804
12.228833254055314 12.97177767207375 0.5 4.015686274509804
final frac 0.0915847614247312
```

(Dab 2: `q(0.696969…) = 178`, `LUT[178] = 45746`, `d = 8 * (45746/65535) = 5.584313725490196`.)

**Straight line split into events.** `size 16, spacing 0.25` (step 4) from `(32,64)` to `(224,64)`
as one segment places 48 dabs: the 49th, at `x = 224`, is missed because the final `frac` is
`0.9999999999999698`. The same line as 81 samples with `x_i = 32.0 + (2.4 * i)` (evaluated in
binary64, `i = 0..80`) places 49 dabs (final `frac = 0.0`), with the first 48 positions within
`1.2e-13` of the one-segment case. Both results are correct; they only show that a golden must
be compared with the reference, never with a differently-segmented version of itself. With mutation
8 the 81-sample version places 1 dab; with `view_zoom 4` and mutation 9 the one-segment version
places 192.

**Dab mask** `m` for a dab at `(10.0, 10.0)`, `size 8`:

```
hardness 0.5, pixel (12,11)                        -> 0.563242072809033
hardness 1.0, pixel (12,12)                        -> 0.44678887547554913
hardness 0.0, pixel (13,10)                        -> 0.037317932004975574
hardness 1.0, roundness 0.5, angle 30, pixel (12,8) -> 0.5623208812614978
```

---

## 9. Golden cases (27; the brush inventory needs ≥ 20)

All canvases are small. "Transparent layer" = `add_layer` with no fill; fills use doc 10's
`add_layer` fill options. Selection and lock ops are the selections doc's and doc 10's. Samples
are listed as `(x, y[, p][, t_ms])`.

| id | script (summary) | what bug this catches |
|---|---|---|
| B01 | 64×64, transparent layer, `brush_stroke` one sample `(32,32)`, size 21, hardness 1, `#C03020` | first dab not placed at pointer-down; hard-edge AA band wrong (not 1 px, not ending at R); pixel-centre offset (`+0.5`) missing |
| B02 | 64×64, one dab `(31.3,32.6)`, size 40, hardness 0 | falloff shape (smoothstep) wrong or inverted (mutation 36); sub-pixel centre handling |
| B03 | 64×64, one dab, size 40, hardness 0.5 | effective hardness `he` / band placement wrong |
| B04 | 96×96, one dab `(48,48)`, size 60, roundness 0.3, angle 30 | rotation sign/axis convention; `ca/sa` swapped; roundness applied to the wrong axis |
| B05 | 256×64, transparent layer, 2-sample line `(20,32)→(236,32)`, size 16, spacing default | spacing not 25 % of diameter; walk arithmetic (`need/todo`) wrong |
| B06 | same line as B05 as 81 samples `x_i = 20.0 + (2.7 * i)`, `y = 32` (each segment 2.7 px, shorter than the 4 px step); correct output places 55 dabs like B05 | **mutation 8** (accumulator reset per event → 1 dab); `frac` lost between events |
| B07 | same as B05 with `view_zoom: 4.0` (output must also equal B05's bytes) | **mutation 9** (spacing in screen space → 4× dab density); any use of view zoom in math |
| B08 | 128×128 opaque white layer, self-crossing figure-eight stroke, size 30, hardness 0.8, `opacity 0.6, flow 0.25`, `mode wash` | **mutation 7** (repeated source-over exceeds 0.6 at the crossing); Wash ceiling not enforced |
| B09 | B08 with `mode buildup` | Build-up treated as Wash (no build past opacity); rate `kb = f*O` wrong |
| B10 | 128×128 layer `#808080`, zig-zag stroke re-covering itself, `opacity 0.5, flow 1.0`, wash | ceiling vs flow swapped; B not starting from 0 per stroke; composite onto previous preview instead of `S0` |
| B11 | 128×128 layer filled `#3366CC80`, soft (hardness 0) stroke `#FFCC00`, opacity 0.8 flow 0.5 | straight-alpha source-over wrong at partial alpha (premultiplied storage, mutation-2-like errors on brush path); soft-edge quantisation |
| B12 | 256×64, line with pressure ramp 0→1 over 30 samples, `size_curve [[0,0],[0.3,0.1],[0.7,0.9],[1,1]]` | curve bake (segment choice, interpolation, `rhu` to 16 bit) wrong; size curve ignored; spacing not following dab size |
| B13 | 64×64, 2 dabs at constant pressure `128/255` and `127/255`, `size_curve [[0,0],[0.5,0],[0.502,1],[1,1]]`, size 24 | **mutation 37** (LUT index off by one flips between full and near-zero size); `q(p)` indexing replaced by truncation |
| B14 | 256×64, pressure varying 1→0.2→1, `opacity_curve [[0,0],[1,1]]`, wash | opacity curve wired to size or flow; `O` not per-dab; ceiling from the wrong factor |
| B15 | 192×192, zig-zag of 25 samples, `smoothing 0.7`, pressure ramp, identity `size_curve` | EMA formula/order wrong; smoothing applied to pressure (**mutation 38**); `a`/`m` swapped |
| B16 | 64×64, airbrush `dabs_per_second 50`, 11 samples at the same point `(32,32)` with `t_ms` 0,16,…,160, soft, flow 0.2, wash and a second layer with buildup | time term missing/scaled (ms vs s); stationary airbrush in wash exceeding the ceiling |
| B17 | 128×128 layer with an opaque disc on transparent, lock-transparency on (doc 10), stroke across the disc edge | lock-transparency changing alpha; painting into transparent pixels |
| B18 | 128×128, elliptical selection with feather (selections doc), stroke crossing the selection edge | selection not applied, applied inside accumulation (changes Wash overlaps), or applied as a hard threshold |
| B19 | 128×128 opaque gradient layer, `eraser_stroke` self-crossing, soft, `opacity 0.7 flow 0.4`, wash | eraser alpha reduction formula; eraser via repeated alpha multiply (mutation-7 class on the eraser path); colour changed by eraser |
| B20 | B17's layer, lock-transparency on, `eraser_stroke` across the disc | **mutation 39** (eraser ignores lock-transparency → holes) |
| B21 | 128×128, layer with gradient + shape, `clone_stroke` with `source (20,20)`, aligned, then a second aligned `clone_stroke` elsewhere | offset derivation, `rhu` of offset, aligned offset persisting across strokes |
| B22 | B21 with `aligned: false` for the second stroke, plus a third stroke whose source region overlaps its own destination | non-aligned reset; sampling the live layer instead of the stroke-start snapshot (echo pattern) |
| B23 | 64×64, two `brush_stroke`s, then `{"op":"undo"}` | **mutation 10** (per-dab history: one undo removes one dab, not the stroke); undo not byte-exact |
| B24 | 64×64 layer with a white mask (doc 10), `brush_stroke target "mask"` colour `#404040`, soft | mask painting luma/lerp wrong; lock/colour path used for masks |
| B25 | 128×64, stroke with pressure tapering to 0.01 at the end, identity size curve, size 12 | sub-pixel dab rule (`d_draw`, `k_small`); `d <= 0` handling; spacing floor `max(d,1)` |
| B26 | 64×64, soft size 6, `spacing 0.01`, 5-sample curve, `view_zoom 0.5` | dense-spacing precision; zoom leaking in (second catch for mutation 9) |
| B27 | 64×64, stroke starting at `(-20,-10)` and ending at `(90,70)` (both off-canvas) | off-canvas dab clipping, touched-pixel range, walk with off-canvas samples |
| B28 | 192×96 opaque white layer, soft (hardness 0) size 48 stroke `#E0602A`, opacity 1, flow 1, default spacing, gentle arc (added 2026-09-26) | **mutation 42** (max-envelope edges bead between dab centres); §3.7 rate/ceiling order |

Notes for golden authors: B07 is additionally asserted byte-equal to B05; B06 is compared to the
reference (not to B05: here both happen to place 55 dabs, but split segments may differ from one
segment in the last bits of dab positions and in the final dab, see section 8). Every golden runs through `rasterloom-cli` vs `refcomp.py` per D7.

---

## 10. Mutation hooks owned by this doc

Each mutation is a single change at a single formula. `--selftest-mutate=N` applies it in the core.

| id | defect (exact site) | caught by |
|---|---|---|
| 7 | 3.7: both modes replaced by repeated source-over: `B = B + (((f * O) * m) * (1.0 - B))` | B08 (primary: Wash crossing exceeds 0.6), B10, B19 |
| 8 | 3.4: at the start of every `add_sample` with `k >= 1`, set `frac = 0.0` before the walk | B06 (primary: 1 dab instead of 55), B12, B15 |
| 9 | 3.4: `L = sqrt((dx*dx) + (dy*dy)) * view_zoom` (spacing measured in screen pixels) | B07 (primary, zoom 4), B26 (zoom 0.5) |
| 10 | 6: a history record is committed after every dab (the stroke memento is closed and a new one opened after each dab; `S0` is not re-taken) | B23 (primary), selftest `brush_history_granularity` |
| 36 | 3.6: transition band uses `m = (t * t) * (3.0 - (2.0 * t))` (falloff inverted: a ring) | B02 (primary), B03, B11, B19 |
| 37 | 3.3: pressure lookup uses `i = max(q(p) - 1, 0)` | B13 (primary), B12, B25 |
| 38 | 3.2: the EMA is also applied to pressure: `ps_k = (a * p_k) + (m * ps_{k-1})` and `ps` is used as the dab pressure | B15 (primary) |
| 39 | 3.8: eraser skips the `lock` check and applies alpha reduction on locked layers | B20 |
| 42 | 3.7: the pre-2026-09-26 model — Wash ceiling `T = O * m` with rate `f` (`if T > B: B = B + (f * (T - B))`), Build-up ceiling `m` with rate `kb`; soft strokes bead between dab centres | B28 (primary), B11 and every soft multi-dab golden (edge profiles differ) |

---

## 11. Parity notes (feeds `docs/PARITY.md`)

All behavioural references are open sources (libmypaint, Krita manual, W3C). Photoshop was not run,
inspected or output-compared. Items below are **unknown** relative to Photoshop unless stated as a
deliberate divergence.

1. **Dab profile**: `1 - smoothstep` falloff from an effective hardness, with a forced ≥1 px edge
   band ending on the nominal radius. Photoshop's round-tip profile and its hardness→profile mapping
   are unknown; soft brushes will look different in falloff shape.
2. **Spacing**: libmypaint-style fractional dab accumulator, distance measured along the smoothed
   path in canvas pixels, step fixed by the previously placed dab's diameter. Photoshop's exact rule
   (and how it treats pressure-varying size) is unknown.
3. **Wash / Build-up**: the two modes are our formalisation of "opacity = stroke ceiling, flow =
   rate toward it" (Wash) and "opacity acts like flow" (Build-up), following the Krita manual's
   description of its Wash/Build-up painting modes
   (<https://docs.krita.org/en/reference_manual/brushes/brush_settings/opacity_and_flow.html>).
   Photoshop's Build-up toggle is tied to its airbrush option; exact equivalence is unknown.
4. **Airbrush**: time term `dt × dabs_per_second` added to the distance term (libmypaint's
   `DABS_PER_SECOND`); GUI synthesises 16 ms repeat samples. Photoshop's airbrush rate model is
   unknown.
5. **Pressure curves**: piecewise-linear, ≤16 points, 8-bit pressure index into a 256-entry
   16-bit LUT. Only size and opacity are pressure-controllable in v0.1 (no flow, angle, roundness,
   tilt, jitter, scatter, texture, dual brush, colour dynamics) — deliberate v0.1 scope.
6. **Tilt** is recorded but has no effect in v0.1 — deliberate.
7. **Stabilizer**: plain EMA on position with no catch-up at stroke end; strokes with high smoothing
   end short of the pointer. Photoshop's smoothing (pulled-string, catch-up options) differs —
   deliberate divergence.
8. **Sub-pixel dabs** fade by diameter (`k_small`) rather than shrinking; unknown vs Photoshop.
9. **Brush blend mode**: Normal only in v0.1 — deliberate.
10. **Lock-transparency**: brush/clone recolour source-atop (destination treated as opaque, §3.8) and keep the
    original alpha. Photoshop's exact colour rule on semi-transparent locked
    pixels is unknown. **Eraser on a locked layer does nothing** — Photoshop's eraser on a layer with
    locked transparency is reported by its public documentation to paint the background colour;
    that is a deliberate divergence in v0.1.
11. **Clone stamp**: samples one layer from the stroke-start snapshot (no in-stroke feedback),
    integer offset, no "current & below / all layers" sampling, no clone-source overlay, no scale or
    rotation of the source. The in-stroke snapshot behaviour relative to Photoshop is unknown.
12. **Mask painting** uses the D3 luma of the brush colour. Eraser on masks is not supported in
    v0.1 (paint black instead) — deliberate.
13. **Undo**: one record per stroke, matching the widely documented behaviour of raster editors;
    no per-dab or per-event records.

---

## 12. Provenance

- Spacing walk (3.4): libmypaint `count_dabs_to()` and `mypaint_brush_stroke_to()`, ISC,
  <https://github.com/mypaint/libmypaint/blob/master/mypaint-brush.c> (read 2026-09-26). Adopted:
  partial-dab carry across events, distance/spacing + time×rate dab count, first dab of an event
  consumes `1 - partial`, linear interpolation of position/pressure/time. Not adopted: base-radius
  term, elliptical distance metric, float32 arithmetic, NaN reset.
- Source-over (3.8): W3C Compositing and Blending Level 1, simple alpha compositing,
  <https://www.w3.org/TR/compositing-1/#simplealphacompositing>.
- Wash/Build-up wording: Krita manual, Opacity and Flow,
  <https://docs.krita.org/en/reference_manual/brushes/brush_settings/opacity_and_flow.html>.
- Everything else (mask formula, AA rule, LUT bake, EMA, clone state, lock rules) is Rasterloom's
  own definition in this document.
