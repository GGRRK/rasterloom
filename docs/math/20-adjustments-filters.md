# Rasterloom normative math — 20: adjustment layers and filters

Status: normative for v0.1. Inherits every rule of `00-conventions.md` (C1–C8). Where this file
says "q" it means the C2 quantiser, "canonical" means the C3 transparent-pixel rule, and
"`pixel_hash`/`unit`" mean the C6 generator.

Scope: the 8 adjustment layers of BUILD-SPEC requirement 7 (Levels, Curves, Brightness/Contrast,
Hue/Saturation, Black & White, Invert, Posterize, Threshold) and the 6 filters of requirement 8
(Gaussian Blur, Motion Blur, Unsharp Mask, Add Noise, High Pass, Offset).

How an adjustment layer enters the layer stack (what its "backdrop" is, how its result is blended
with its opacity, mask, clipping and group context) is owned by `10-compositing.md`. This file only
defines the **adjusted colour**: a pure per-pixel function `adj(RGBA8) -> RGBA8`.

Notation used throughout:

- `R8, G8, B8, A8` are the stored bytes of a pixel (integers 0..255). `r = R8 / 255.0` etc. per C2.
- `rhaz(y)` = round half away from zero of a double `y`: for `y >= 0` it is
  `floor(y) + ((y - floor(y)) >= 0.5)`; for `y < 0` it is `-rhaz(-y)`. (Same rule as C2's `q`
  without the clamp and scale; never `int(y + 0.5)`, never half-to-even.)
- `//` is floor division of non-negative integers; all integer arithmetic in this file is exact
  (use 64-bit integers; no value exceeds 2^40).
- `K_PI = 3.141592653589793`. `K_DEG = K_PI / 180.0` (one division, computed once, per C7).
- Integer-valued JSON fields must be JSON integers. Double-valued JSON fields accept any JSON
  number and are parsed to the nearest binary64 (glibc `strtod` and Python `float` are both
  correctly rounded, so they agree).
- **Validation is strict:** an unknown field, a missing required field, a wrong JSON type or a
  value outside its stated range rejects the whole script (CLI exits non-zero, `refcomp.py`
  raises). Nothing is silently clamped at parse time; this keeps the two parsers from agreeing
  on different "repaired" values.

---

## Part A — Adjustments

### A0. The adjusted-colour contract

For every pixel of the backdrop handed over by doc 10:

```
adj(R8, G8, B8, A8):
    (R', G', B') = f(R8, G8, B8)           # f is the type-specific map below, bytes -> bytes
    if A8 == 0: return (0, 0, 0, 0)          # canonical rule (C3)
    return (R', G', B', A8)                  # alpha passes through untouched
```

- `f` never reads alpha. Every `f` is evaluated for all pixels (including alpha-0 ones, whose
  result is then discarded by the canonical rule), so an implementation may skip alpha-0 pixels.
- Five types (Levels, Curves, Brightness/Contrast, Invert, Posterize) are **per-channel lookup
  tables**: `f(R8,G8,B8) = (LUT_R[R8], LUT_G[G8], LUT_B[B8])`, each LUT a 256-entry byte table built
  once per render from scalars. Building the table and applying it is normative; any
  transcendental appears only in the table build (C1).
- Three types (Hue/Saturation, Black & White, Threshold) mix channels and are specified per pixel
  with only `+ - * /`, `min`, `max`, comparisons and integer arithmetic.
- No intermediate quantisation happens inside an adjustment unless stated. The only `q` is the
  final one.

### A1. Levels

**Semantics.** For each of the composite ("rgb") and the three channels ("r", "g", "b"): input
black point and white point stretch the input range to full scale (values below/above clip),
gamma bends the midtones (gamma > 1 brightens), output black/white compress the result into the
output range (they may be crossed to invert). The per-channel setting is applied first, then the
composite setting is applied to that result.

Provenance: the map and the channel-then-composite order follow GIMP's
`gimp_operation_levels_map` and its process loop
(https://gitlab.gnome.org/GNOME/gimp/-/blob/master/app/operations/gimpoperationlevels.c, lines
96–178 at the time of writing: channel map with `low_input[channel + 1]`, then the value map with
`low_input[0]`). GPL-3.0-or-later, compatible with D1.

**Formula.** For one setting `S = (in_black, in_white, gamma, out_black, out_white)` precompute
once, in this order:

```
lb  = S.in_black  / 255.0
hb  = S.in_white  / 255.0
ob  = S.out_black / 255.0
ow  = S.out_white / 255.0
inv = 1.0 / S.gamma
```

and define the scalar map

```
L_S(x):
    v = (x - lb) / (hb - lb)                  # hb > lb is guaranteed by validation
    v = clamp(v, 0.0, 1.0)
    if S.gamma != 1.0 and v > 0.0:
        v = pow(v, inv)                        # libm pow on a scalar (C1)
    return ob + (v * (ow - ob))
```

LUT build, for each channel `c` in R, G, B and each `i` in 0..255:

```
x        = i / 255.0
y        = L_rgb( L_c(x) )                     # channel setting first, composite second
LUT_c[i] = q(y)
```

`L_c` for a channel whose setting was not given is the identity setting (0, 255, 1.0, 0, 255); it
may be skipped because it is exact: `(x - 0.0) / (1.0 - 0.0) = x`, no pow, `0.0 + (x * 1.0) = x`.

**Edge cases.** `v = 0` skips pow (pow(0, inv) would be 0 anyway). `ow < ob` (crossed output) uses
the same formula; `ow - ob` is then negative. The result of `L_c` is in `[min(ob,ow), max(ob,ow)]`
and therefore inside `[0,1]` before it enters `L_rgb`.

**Params** (`"type": "levels"`): an object with optional keys `"rgb"`, `"r"`, `"g"`, `"b"`, each an
object with:

| field | type | default | range |
|---|---|---|---|
| `in_black` | int | 0 | 0..254 |
| `in_white` | int | 255 | 1..255, and `in_black < in_white` |
| `gamma` | double | 1.0 | 0.1..9.99 |
| `out_black` | int | 0 | 0..255 |
| `out_white` | int | 255 | 0..255 (may be < `out_black`) |

Missing fields take their defaults individually.

### A2. Curves

**Semantics.** Each of "rgb", "r", "g", "b" is a curve through 2..16 control points (input level,
output level). The curve is a **natural cubic spline** through the points (second derivative zero
at both end points). Left of the first point the curve is held flat at the first point's output;
right of the last point it is held flat at the last point's output. The curve's output is clipped
to 0..255. Per-channel curve first, composite ("rgb") curve second, as in Levels.

Provenance: the spline construction, the tridiagonal solver, the evaluation polynomial, the
flat extension (`x = qBound(first.x, x, last.x)`) and the output clip are those of Krita's
`KisLegacyCubicSpline` / `KisTridiagonalSystem`
(https://invent.kde.org/graphics/krita/-/blob/master/libs/image/kis_cubic_curve_spline.h) and
`KisCubicCurve::Data::value`
(https://invent.kde.org/graphics/krita/-/blob/master/libs/image/kis_cubic_curve.cpp), GPL-2.0-or-later;
adapted here to level units (0..255 instead of 0..1). The channel-then-composite order follows
GIMP `gimp_curve_map_pixels`
(https://gitlab.gnome.org/GNOME/gimp/-/blob/master/app/core/gimpcurve-map.c, default branch:
`map(curve_colors, map(curve_red, src[0]))`).

**Spline build.** Input: points `(X[0], Y[0]) .. (X[n-1], Y[n-1])`, `n` in 2..16, integers,
`X` strictly increasing. All arithmetic below is on doubles (`X[i]`, `Y[i]` converted exactly).
`K6 = 1.0 / 6.0` (computed once). `m = n - 1` is the number of intervals.

```
for i in 0..m-1:   H[i] = X[i+1] - X[i]                      # exact small integers as doubles

# second-derivative vector C[0..m], natural ends
C[0] = 0.0 ; C[m] = 0.0
s = m - 1                                                     # number of unknowns C[1..m-1]
if s >= 1:
    for k in 0..s-1:
        TB[k] = 2.0 * (H[k] + H[k+1])
        TF[k] = 6.0 * (((Y[k+2] - Y[k+1]) / H[k+1]) - ((Y[k+1] - Y[k]) / H[k]))
    for k in 0..s-2:
        TA[k] = H[k+1]                                        # sub- and super-diagonal (symmetric)
    # Thomas algorithm, exactly Krita's sweep
    if s == 1:
        U[0] = TF[0] / TB[0]
    else:
        AL[1] = -TA[0] / TB[0]                                # -(TA[0]) / TB[0]
        BE[1] =  TF[0] / TB[0]
        for k in 1..s-2:
            den     = (TA[k-1] * AL[k]) + TB[k]
            AL[k+1] = -TA[k] / den
            BE[k+1] = (TF[k] - (TA[k-1] * BE[k])) / den
        U[s-1] = (TF[s-1] - (TA[s-2] * BE[s-1])) / (TB[s-1] + (TA[s-2] * AL[s-1]))
        for k in s-2 down to 0:
            U[k] = (AL[k+1] * U[k+1]) + BE[k+1]
    for k in 0..s-1:  C[k+1] = U[k]

for i in 0..m-1:
    D[i] = (C[i+1] - C[i]) / H[i]
    B[i] = ((-0.5 * (C[i] * H[i])) - (K6 * ((D[i] * H[i]) * H[i]))) + ((Y[i+1] - Y[i]) / H[i])
```

`den` is computed once per iteration and used for both `AL` and `BE` (Krita writes the same
expression twice; the values are identical). Negation is exact, so `-TA[k] / den` is
unambiguous.

**Evaluation** at a double `x` (the input may be non-integer when the composite curve is applied
to a channel curve's output):

```
S(x):
    x = clamp(x, X[0], X[m])                                  # flat extension  (mutation 25 hook)
    i = the largest index in 0..m-1 with X[i] <= x            # so X[m] itself uses i = m-1
    t = x - X[i]
    y = ((Y[i] + (B[i] * t)) + (((0.5 * C[i]) * t) * t)) + ((((K6 * D[i]) * t) * t) * t)
    return clamp(y, 0.0, 255.0)
```

LUT build, for each channel `c` and each `i` in 0..255:

```
LUT_c[i] = q( S_rgb( S_c(i) ) / 255.0 )                        # i converted to double exactly
```

A channel with no curve given uses the identity curve `[[0,0],[255,255]]`, which is exact
(`n = 2`: `C = [0,0]`, `D = 0`, `B = 1.0`, `S(x) = x`), so it may be skipped.

**Edge cases.** `n = 2` is a straight line (no solver). Overshoot of the cubic between points is
real (natural splines ring) and is clipped by the final clamp. With `X[0] > 0` all inputs below
`X[0]` map to `Y[0]`; with `X[m] < 255` all inputs above map to `Y[m]`.

**Params** (`"type": "curves"`): object with optional keys `"rgb"`, `"r"`, `"g"`, `"b"`; each value
is an array of 2..16 points `[in, out]`, both ints 0..255, `in` strictly increasing (else error).
Default for each key: `[[0,0],[255,255]]`.

### A3. Brightness/Contrast

**Semantics.** Brightness moves every level toward white (positive) or black (negative)
proportionally to its distance from that end, so black and white are never clipped by brightness
alone. Contrast then scales around mid-grey 0.5 with a slope `tan((contrast + 1) * π/4)`: 0 is
identity, -1 collapses everything to mid-grey, +1 is effectively a hard threshold at 0.5.

This is the "modern" (non-legacy) form in the sense of a smooth, non-clipping brightness term.
Provenance: GIMP `gimp_operation_brightness_contrast_map` and its caller
(https://gitlab.gnome.org/GNOME/gimp/-/blob/master/app/operations/gimpoperationbrightnesscontrast.c,
lines 87–121: `brightness = config->brightness / 2.0; slant = tan((config->contrast + 1) * G_PI_4)`).
Adobe's own modern formula is not published (see Parity notes).

**Formula.** Precompute once:

```
bh    = P.brightness / 2.0
if P.contrast == 0.0:
    slant = 1.0                                   # stated exactly: tan(π/4) in libm is 0.9999999999999999
else:
    slant = tan((P.contrast + 1.0) * 0.7853981633974483)      # 0.7853981633974483 = G_PI_4; libm tan
```

LUT (same table for R, G and B), for `i` in 0..255:

```
v = i / 255.0
if bh < 0.0:  v = v * (1.0 + bh)
else:         v = v + ((1.0 - v) * bh)
v = ((v - 0.5) * slant) + 0.5
LUT[i] = q(v)
```

**Edge cases.** `contrast = +1`: the argument is `1.5707963267948966`, `tan` returns
`1.633123935319537e16`; every level except an exact 0.5 goes to 0 or 255 (no integer level maps to
exactly 0.5 when `bh = 0`). `contrast = -1`: `slant = tan(0.0) = 0.0`, every level becomes
`q(0.5) = 128`. No division, no guard needed.

**Params** (`"type": "brightness_contrast"`):

| field | type | default | range |
|---|---|---|---|
| `brightness` | double | 0.0 | -1.0..1.0 |
| `contrast` | double | 0.0 | -1.0..1.0 |

### A4. Hue/Saturation (master range only in v0.1)

**Colour model: HSL** (the double-hexcone; lightness = (max + min) / 2), in the gamma-encoded
document space (D3). Hue is carried in **sextant units** `h6 ∈ [0, 6)` (0 = red, 1 = yellow,
2 = green, 3 = cyan, 4 = blue, 5 = magenta) so no per-pixel division by 6 or 360 is needed.

Provenance: the RGB↔HSL formulas are the classic Foley–van Dam double-hexcone as implemented in
GIMP `libgimpcolor/gimpcolorspace.c` (`gimp_rgb_to_hsl`, `gimp_hsl_to_rgb`, `gimp_hsl_value`),
rewritten in sextant units; the saturation and lightness maps are GIMP's `map_saturation` and
`map_lightness`
(https://gitlab.gnome.org/GNOME/gimp/-/blob/master/app/operations/gimpoperationhuesaturation.c,
lines 136–167), with the lightness input **not** halved (GIMP halves; we do not, so ±100 reach
white/black).

Normative property (verified exhaustively over all 2^24 colours while writing this document): with
hue 0, saturation 0, lightness 0 the round trip `q(hsl_to_rgb(rgb_to_hsl(c)))` is the identity.
Any implementation that fails this has an evaluation-order bug.

```
rgb_to_hsl(r, g, b):                       # r, g, b doubles in [0,1]
    mx = max(max(r, g), b)
    mn = min(min(r, g), b)
    l  = (mx + mn) / 2.0
    d  = mx - mn
    if d == 0.0:
        return (0.0, 0.0, l)
    if l <= 0.5:  s = d / (mx + mn)
    else:         s = d / ((2.0 - mx) - mn)
    if   r == mx: h6 = (g - b) / d
    elif g == mx: h6 = 2.0 + ((b - r) / d)
    else:         h6 = 4.0 + ((r - g) / d)
    if h6 < 0.0:  h6 = h6 + 6.0
    return (h6, s, l)

hsl_value(n1, n2, h):
    if   h > 6.0: h = h - 6.0
    elif h < 0.0: h = h + 6.0
    if   h < 1.0: return n1 + ((n2 - n1) * h)
    elif h < 3.0: return n2
    elif h < 4.0: return n1 + ((n2 - n1) * (4.0 - h))
    else:         return n1

hsl_to_rgb(h6, s, l):
    if s == 0.0:
        return (l, l, l)
    if l <= 0.5:  m2 = l * (1.0 + s)
    else:         m2 = (l + s) - (l * s)
    m1 = (2.0 * l) - m2
    return ( hsl_value(m1, m2, h6 + 2.0),
             hsl_value(m1, m2, h6),
             hsl_value(m1, m2, h6 - 2.0) )
```

Branch tests use the exact comparisons shown (`r == mx` first, then `g == mx`). A vectorised
implementation must select branches with masks and must not let a `NaN`/`inf` from an unselected
branch (e.g. `d == 0`) reach the output.

**Master mode formula.** Precompute once: `dh = P.hue / 60.0`, `ks = 1.0 + (P.saturation / 100.0)`,
`vl = P.lightness / 100.0`. Per pixel:

```
(h6, s, l) = rgb_to_hsl(R8/255.0, G8/255.0, B8/255.0)
h6 = h6 + dh                                              # (mutation 26 hook: sign of dh)
if   h6 >= 6.0: h6 = h6 - 6.0
elif h6 <  0.0: h6 = h6 + 6.0
if vl < 0.0: l = l * (vl + 1.0)
else:        l = l + (vl * (1.0 - l))
s = clamp(s * ks, 0.0, 1.0)
(r, g, b) = hsl_to_rgb(h6, s, l)
return (q(r), q(g), q(b))
```

`dh ∈ [-3, 3]` and `h6 ∈ [0, 6)`, so one wrap step suffices. Achromatic pixels (`d == 0`) keep
`s = 0`, so hue and saturation have no effect on them; lightness still applies.

**Colorize mode formula.** Precompute once: `hc = P.hue / 60.0; if hc >= 6.0: hc = hc - 6.0`,
`sc = P.saturation / 100.0`, `vl = P.lightness / 100.0`. Per pixel:

```
(_, _, l) = rgb_to_hsl(R8/255.0, G8/255.0, B8/255.0)      # only the lightness is used
if vl < 0.0: l = l * (vl + 1.0)
else:        l = l + (vl * (1.0 - l))
(r, g, b) = hsl_to_rgb(hc, sc, l)
return (q(r), q(g), q(b))
```

**Params** (`"type": "hue_saturation"`):

| field | type | default | range (colorize = false) | range (colorize = true) |
|---|---|---|---|---|
| `colorize` | bool | false | | |
| `hue` | double | 0.0 | -180.0..180.0 | 0.0..360.0 |
| `saturation` | double | 0.0 (colorize: 25.0) | -100.0..100.0 | 0.0..100.0 |
| `lightness` | double | 0.0 | -100.0..100.0 | -100.0..100.0 |

Per-range editing (reds, yellows, … with range sliders) is out of scope for v0.1 (Parity notes).

### A5. Black & White

**Semantics.** Converts to grey where the user controls how bright each of six colour families
becomes. Each pixel is decomposed into an achromatic part (its minimum channel), a **secondary**
part (the amount by which the middle channel exceeds the minimum, in the hue of the two largest
channels: yellow = R+G, cyan = G+B, magenta = R+B) and a **primary** part (the amount by which the
maximum channel exceeds the middle one, in the hue of the largest channel: red, green or blue).
Grey = achromatic + secondary × its family weight + primary × its family weight. Pure red (1,0,0)
therefore becomes `W_reds`, pure yellow (1,1,0) becomes `W_yellows`, white stays white, greys stay
unchanged. Optional tint recolours the grey with a hue and saturation, keeping the grey as HSL
lightness.

Provenance: this decomposition is Rasterloom's own construction from the HSV sextant geometry
(the same min/mid/max split that underlies A4's hue formula); it is **not** taken from any Adobe
material. It reproduces the documented user-facing behaviour (six families, defaults
40/60/40/60/20/80) but whether it equals Adobe's internal formula is unknown (Parity notes).

**Formula.** Precompute once: `wR = P.reds/100.0`, `wY = P.yellows/100.0`, `wG = P.greens/100.0`,
`wC = P.cyans/100.0`, `wB = P.blues/100.0`, `wM = P.magentas/100.0`. Per pixel, in integers then
doubles:

```
mx = max(R8, G8, B8) ; mn = min(R8, G8, B8) ; md = (R8 + G8 + B8) - mx - mn      # exact ints
wp = wR if R8 == mx else (wG if G8 == mx else wB)         # primary family: largest channel
ws = wY if B8 == mn else (wC if R8 == mn else wM)         # secondary: family of the two largest
Y  = (mn + ((md - mn) * ws)) + ((mx - md) * wp)           # ints converted to double exactly
g  = Y / 255.0
```

Tie-break independence: when `mx == md`, `wp` is multiplied by 0; when `md == mn`, `ws` is
multiplied by 0. So the stated tie-break order (R, G, B for the maximum; B, R, G for the minimum)
never changes the result, but it is still normative so both implementations select identically.
(A product with a negative weight may be `-0.0`; adding `-0.0` is exact.)

Untinted output: `(q(g), q(g), q(g))` (q clamps `g`, which can leave `[0,1]` with weights outside
0..100).

Tinted output: precompute once `(th6, ts, _) = rgb_to_hsl(T.R8/255.0, T.G8/255.0, T.B8/255.0)` of the
tint colour `T` (A4 function). Per pixel:

```
gl = clamp(g, 0.0, 1.0)
(r, g2, b) = hsl_to_rgb(th6, ts, gl)
return (q(r), q(g2), q(b))
```

**Params** (`"type": "black_white"`):

| field | type | default | range |
|---|---|---|---|
| `reds` | double | 40.0 | -200.0..300.0 |
| `yellows` | double | 60.0 | -200.0..300.0 |
| `greens` | double | 40.0 | -200.0..300.0 |
| `cyans` | double | 60.0 | -200.0..300.0 |
| `blues` | double | 20.0 | -200.0..300.0 |
| `magentas` | double | 80.0 | -200.0..300.0 |
| `tint` | string or null | null | `"#RRGGBB"` (alpha not accepted) |

### A6. Invert

`LUT[i] = 255 - i` for R, G and B (integer; no double path). Params (`"type": "invert"`): `{}`
(the `params` field may be omitted).

### A7. Posterize

**Semantics.** Each channel is reduced to `n` evenly spaced levels including 0 and 255, each
input snapped to the nearest level.

Provenance: the "scale by n−1, round, divide by n−1" form is the textbook uniform quantiser, as
in GIMP's posterize operation (https://gitlab.gnome.org/GNOME/gimp/-/blob/master/app/operations/gimpoperationposterize.c).

**Formula.** `n = P.levels`, `k = n - 1` (integer, converted to double once as `kd`). For `i` in
0..255:

```
t = (i / 255.0) * kd
j = floor(t) + ((t - floor(t)) >= 0.5)                     # rhaz, t >= 0
LUT[i] = q(j / kd)
```

(An exact tie `t = j + 0.5` would need `2·i·k = 255·(2j+1)`, impossible because the left side is
even; the rhaz rule is stated anyway.) Same LUT for R, G, B.

**Params** (`"type": "posterize"`): `levels` int, default 4, range 2..255.

### A8. Threshold

**Semantics.** Every pixel becomes black or white: white if its luma is at least the threshold
level, else black.

**Luma choice: ITU-R BT.601** `Y' = 0.299 R' + 0.587 G' + 0.114 B'` on the gamma-encoded bytes,
rounded to an integer grey level. Why: Threshold is a "convert to grey, then cut" operation, and
BT.601 is the de-facto luma of 8-bit R'G'B'→grey conversion (JFIF/JPEG, GIMP "Luma" desaturate,
most greyscale conversions); rounding to an integer grey first makes `level` mean exactly "grey
value ≥ level". D3's 0.30/0.59/0.11 is the W3C Compositing constant for the four non-separable
blend modes and is deliberately **not** reused here: keeping separate constants means global
mutation 1 (Rec.709 luma in blend modes) cannot mask or be masked by Threshold. Source for the
coefficients: ITU-R BT.601 (https://www.itu.int/rec/R-REC-BT.601).

**Formula** (all integer, exact):

```
Y8  = ((299 * R8) + (587 * G8) + (114 * B8) + 500) // 1000      # 0..255; white -> 255 exactly
out = 255 if Y8 >= P.level else 0
return (out, out, out)
```

**Params** (`"type": "threshold"`): `level` int, default 128, range 1..255.

### A9. Render-script op: `add_adjustment`

```json
{"op": "add_adjustment", "id": "lv1", "type": "levels",
 "params": {"rgb": {"in_black": 20, "in_white": 235, "gamma": 1.4}}}
```

| field | type | default | notes |
|---|---|---|---|
| `op` | string | — | `"add_adjustment"` |
| `id` | string | — (required) | unique among all layer/group ids of the script (doc 10 rule) |
| `type` | string | — (required) | one of `levels`, `curves`, `brightness_contrast`, `hue_saturation`, `black_white`, `invert`, `posterize`, `threshold` |
| `params` | object | `{}` | type-specific, exactly the tables in A1–A8; unknown keys are an error |
| placement fields | — | — | the same placement fields and defaults that doc 10 defines for `add_layer` (e.g. parent group / position); an adjustment layer is placed exactly like a raster layer |

The new layer is an adjustment layer with doc 10's defaults for a new layer (Normal blend, opacity
1.0, fill 1.0, visible, no mask). All of doc 10's ops that take a layer id (blend, opacity, fill,
visibility, mask, clipping, grouping, merge, flatten) apply to it. Params are fixed at creation in
v0.1 (no "edit adjustment" op; add a new layer instead).

---

## Part B — Filters

### B0. Filter framework (applies to all six)

A filter is **destructive**: it rewrites the pixel buffer of one raster layer. A layer's pixel
buffer is canvas-sized (`W × H`, doc 10); `x, y` below are canvas coordinates.

Pipeline, per filter op:

```
O = the target layer's pixels (RGBA8, W x H)
F = filter(O, params)                         # computed over the whole layer, B1..B6
if the target layer has lock-transparency set (doc 10 property):
    for every pixel: F.A = O.A ; if O.A == 0: F = (0,0,0,0)     # colour of F kept otherwise
M = coverage mask (8-bit, W x H), B0.2
R = coverage_lerp(O, F, M)                    # B0.1
target layer pixels = R                        # canonical rule applied per pixel
```

The layer's mask (if any) is not touched and not consulted. Hidden layers are filtered like
visible ones. Targeting a group or an adjustment layer is a script error.

#### B0.1 Coverage lerp

Selection coverage mixes the filtered and the original pixel **in premultiplied space**, so a
half-selected pixel next to transparency does not pick up the colour of an invisible pixel:

```
coverage_lerp(O, F, M):
    if M == 0:   return O                     # normative short-cuts, not optimisations:
    if M == 255: return F                     # the general formula is not exact at m = 0 or 1
    m  = M / 255.0
    aO = O.A / 255.0
    aF = F.A / 255.0
    a  = (aO * (1.0 - m)) + (aF * m)
    for each colour channel c:
        p = (((O.c / 255.0) * aO) * (1.0 - m)) + (((F.c / 255.0) * aF) * m)
        C = p / a if a > 0.0 else 0.0
        R.c = q(C)
    R.A = q(a)
    apply canonical rule
```

#### B0.2 Coverage sources

The optional `coverage` field of every filter op is one of:

| value | M(x, y) |
|---|---|
| omitted | 255 everywhere |
| `{"src": "selection"}` | the document's active selection mask as defined by the selections doc (8-bit, canvas-sized); if no selection is active, 255 everywhere |
| `{"src": "rect", "x": int, "y": int, "w": int, "h": int, "value": int}` | `value` (default 255, range 0..255) for `x <= px < x+w` and `y <= py < y+h`, else 0; `w, h >= 0`; the rect may extend past the canvas (clipped) |
| `{"src": "ramp", "dir": "h"}` | `(px * 255) // (W - 1)`, or 255 when `W == 1` |
| `{"src": "ramp", "dir": "v"}` | `(py * 255) // (H - 1)`, or 255 when `H == 1` |

`rect` and `ramp` exist so filter goldens can exercise partial coverage without depending on the
selection tools; they are ordinary script features, not test-only.

#### B0.3 Premultiplication and edge modes

Neighbour-mixing filters (Gaussian Blur, Motion Blur, and Unsharp Mask / High Pass through the
Gaussian) premultiply per C3/D2 at the exact place stated in each section. Two edge modes exist
for reads outside `0..W-1 × 0..H-1`:

- `"clamp"` (default): the coordinate is clamped to the nearest edge pixel (`min(max(i, 0), N-1)`
  per axis). An opaque layer stays opaque at the canvas border.
- `"transparent"`: the read returns the transparent pixel (all premultiplied components 0), per C4.

### B1. Gaussian Blur (3-pass box, O(1) in radius)

**Semantics.** Blurs the layer with an approximate Gaussian of standard deviation `radius`
pixels, built as three successive box blurs per axis (Kovesi, "Fast Almost-Gaussian Filtering",
DICTA 2010, https://www.peterkovesi.com/papers/FastGaussianSmoothing.pdf, eqs. 3–5; the same
widths as `sigma_to_box_radius` in https://github.com/bfraboni/FastGaussianBlur). The whole
pipeline is **integer arithmetic** on premultiplied values, so a running-sum implementation and a
direct-sum implementation produce identical results, and the rounding after every 1-D pass is
normative.

**Box widths from sigma** (scalar, once per op; `n = 3`, `s = radius`):

```
wi = sqrt((((12.0 * s) * s) / 3.0) + 1.0)       # sqrt is correctly rounded (C1)
wl = floor(wi)  (as int)
if wl % 2 == 0: wl = wl - 1                      # largest odd integer <= wi
wu = wl + 2
mi = (((((12.0 * s) * s) - (3 * wl * wl)) - (12 * wl)) - 9) / ((-4.0 * wl) - 4.0)
                                                  # 3*wl*wl, 12*wl, 9 are exact ints (= n*wl^2, 4*n*wl, 3*n)
m  = clamp(rhaz(mi), 0, 3)
widths = [wl if j < m else wu  for j in 0, 1, 2]  # the m narrow boxes first (Kovesi)
```

Examples (normative, checkable): radius 0.5 → [1,1,1] (identity); 1.0 → [1,1,3]; 1.5 → [3,3,3];
2.0 → [3,3,5]; 3.0 → [5,5,7]; 4.5 → [9,9,9]; 8.0 → [15,15,17]; 25 → [49,49,51]; 250 → [499,499,501].

**Pipeline.**

```
Stage 1, premultiply (exact ints):   Pc = C8 * A8   for c in R,G,B     # 0..65025
                                      Pa = A8 * 255                     # 0..65025
Stage 2, horizontal:  for w in widths (in order):  every row:  P = box(P, w, axis = x)
Stage 3, vertical:    for w in widths (in order):  every column: P = box(P, w, axis = y)
                      # (mutation 12 hook: Stage 3 before Stage 2)
Stage 4, unpremultiply:
    a = Pa / 65025.0
    C = (Pc / Pa) if Pa > 0 else 0.0              # int/int as doubles: float(Pc) / float(Pa)
    out = (q(C_R), q(C_G), q(C_B), q(a)), canonical rule
```

One 1-D box pass of odd width `w`, radius `r = (w - 1) // 2`, on each of the four planes
independently:

```
box(P, w, axis)[i] = ( Σ_{j = i-r .. i+r} E(P, j) + r ) // w
```

where `E` applies the edge mode along that axis (`clamp`: `P[min(max(j,0), N-1)]`;
`transparent`: `0` outside). `w` is odd, so `(S + r) // w` is exact round-half-up of `S / w` and a
tie (`S/w` exactly `k + 0.5`) cannot occur. `w = 1` is the identity. The maximum sum is
`65025 × 501 < 2^26`.

Because every pass rounds to an integer, horizontal-then-vertical and vertical-then-horizontal
differ (by ±1 in the 65025 scale at many pixels), which is what makes mutation 12 observable; the
H-then-V order above is normative. (In exact arithmetic the two orders would commute — separable
box filters on different axes commute for either edge mode — so the order is only visible through
this rounding.)

Stage 4 notes: `Pc` can exceed `Pa` by rounding; `q` clamps `C`. `Pa > 0` is the division guard.

**Op.**

```json
{"op": "filter_gaussian_blur", "layer": "p", "radius": 3.0, "edge": "clamp"}
```

| field | type | default | range |
|---|---|---|---|
| `layer` | string | required | id of a raster layer |
| `radius` | double | 1.0 | 0.1..250.0 (it is σ in pixels) |
| `edge` | string | `"clamp"` | `"clamp"`, `"transparent"` |
| `coverage` | object | omitted | B0.2 |

### B2. Motion Blur

**Semantics.** Averages the layer along a straight line segment of length `distance` pixels,
centred on each pixel, at `angle` degrees (0 = horizontal, positive = counter-clockwise on
screen, i.e. toward the top-right for 0 < angle < 90). Sub-pixel sample positions are bilinearly
interpolated in premultiplied space.

Provenance: sample count `ceil(length) + 1`, parameter `t = step/(num_steps-1) - 0.5`, offsets
`length·cos θ`, `length·sin θ`, the nested-lerp bilinear form, premultiplied ("RaGaBaA") space and
clamped reads follow GEGL `motion-blur-linear`
(https://gitlab.gnome.org/GNOME/gegl/-/blob/master/operations/common/motion-blur-linear.c,
`process`: `num_steps = (gint)ceil(o->length) + 1`, `mixy0 = dy*(pix2-pix0)+pix0`, …). Deliberate
changes: (1) the y offset is negated so positive angles rotate counter-clockwise in a y-down
image; (2) the per-step offset is split into integer and fractional parts **before** adding the
pixel position, so the bilinear weights are identical for every pixel (GEGL adds `px + t·ox`
first, which makes the fraction depend on `px` in the last bits); (3) the edge mode is a
parameter.

**Per-op scalar precompute** (once):

```
th = P.angle * K_DEG
ox = P.distance * cos(th)                        # libm cos
oy = -(P.distance * sin(th))                     # libm sin; negation exact
if abs(ox) < 1e-9: ox = 0.0                      # snap: 90° gives exactly vertical
if abs(oy) < 1e-9: oy = 0.0
N  = ceil(P.distance) + 1                        # int, >= 2 because distance >= 1
for s in 0..N-1:
    t      = (s / (N - 1)) - 0.5                  # s and N-1 as doubles
    tx     = t * ox ;   ty = t * oy
    IX[s]  = floor(tx) (int) ;  FX[s] = tx - floor(tx)
    IY[s]  = floor(ty) (int) ;  FY[s] = ty - floor(ty)
```

**Premultiplied plane** (doubles): `pc = (C8 * A8) / 65025.0` for c in R,G,B (integer product,
then one division), `pa = A8 / 255.0`.

**Per pixel `(x, y)`**, for each of the four components independently, `acc = 0.0`, then for `s`
in increasing order:

```
x0 = x + IX[s] ; y0 = y + IY[s]
p0 = E(x0,     y0)     ; p1 = E(x0 + 1, y0)
p2 = E(x0,     y0 + 1) ; p3 = E(x0 + 1, y0 + 1)          # E = edge mode (B0.3), both axes
m0 = (FY[s] * (p2 - p0)) + p0
m1 = (FY[s] * (p3 - p1)) + p1
acc = acc + ((FX[s] * (m1 - m0)) + m0)
```

Then:

```
a = acc_a / N
C = (acc_c / N) / a  if a > 0.0 else 0.0
out = (q(C_R), q(C_G), q(C_B), q(a)), canonical rule
```

When `FX[s] == 0` the `p1`/`p3` terms still enter as `0.0 * (…)`; the read must still happen
(edge mode applies) but contributes exactly 0 unless it is `inf`/`NaN`, which cannot occur.

**Op.**

```json
{"op": "filter_motion_blur", "layer": "p", "angle": 30.0, "distance": 7.5, "edge": "clamp"}
```

| field | type | default | range |
|---|---|---|---|
| `layer` | string | required | raster layer id |
| `angle` | double | 0.0 | -360.0..360.0 degrees |
| `distance` | double | 10.0 | 1.0..2000.0 pixels |
| `edge` | string | `"clamp"` | `"clamp"`, `"transparent"` |
| `coverage` | object | omitted | B0.2 |

### B3. Unsharp Mask

**Semantics.** Sharpens by adding back `amount` times the difference between the image and its
Gaussian blur, per colour channel, but only where that difference is at least `threshold` levels
(so flat noise below the threshold is left alone). Alpha is not sharpened.

**Formula.** `Bl = GaussianBlur(O, radius, edge)` — the complete B1 output bytes (Stages 1–4,
canonical rule included, no coverage, no lock). Precompute `k = P.amount / 100.0`. Per pixel, per
colour channel `c`:

```
d = O.c - Bl.c                                  # int, -255..255
if abs(d) < P.threshold:                         # integer comparison in LEVELS (mutation 28 hook)
    F.c = O.c
else:
    F.c = q((O.c + (k * d)) / 255.0)             # O.c and d converted to double exactly
F.A = O.A ; canonical rule
```

Provenance: the "original + amount × (original − blurred), gated by |difference| vs threshold,
per channel" structure is the classic unsharp mask as in GIMP's legacy unsharp plug-in; GIMP gates
on `abs(2*diff)`, we gate on `abs(diff)` (threshold in plain levels, matching the user-facing
0..255 "levels" unit).

**Op.**

```json
{"op": "filter_unsharp_mask", "layer": "p", "amount": 150.0, "radius": 2.0, "threshold": 0}
```

| field | type | default | range |
|---|---|---|---|
| `layer` | string | required | raster layer id |
| `amount` | double | 50.0 | 1.0..500.0 (percent) |
| `radius` | double | 1.0 | 0.1..250.0 (σ, passed to B1) |
| `threshold` | int | 0 | 0..255 levels |
| `edge` | string | `"clamp"` | edge mode of the inner blur |
| `coverage` | object | omitted | B0.2 |

### B4. Add Noise

**Semantics.** Adds random noise to R, G, B (alpha untouched). `uniform` draws from
`[-A, +A)`; `gaussian` draws from a normal distribution with the **same standard deviation** as
the uniform one (`σ = A/√3`). `monochromatic` adds the same value to all three channels (grey
noise); otherwise each channel gets an independent value. Deterministic in `(seed, x, y)` only.

**Scalars** (once): `A = (P.amount * 255.0) / 100.0`; `sigma = A / 1.7320508075688772`
(the double nearest √3).

**Streams** (C6 `k` values): channel index `c` = 0 (R), 1 (G), 2 (B).

| distribution | monochromatic = false | monochromatic = true |
|---|---|---|
| uniform | `k = c` | `k = 0` for all channels |
| gaussian | `k1 = 2*c`, `k2 = 2*c + 1` | `k1 = 0`, `k2 = 1` for all channels |

**Per pixel `(x, y)`, per colour channel** (scalar evaluation; `u = unit(pixel_hash(seed, x, y, k))`):

```
uniform:   n = A * ((2.0 * u) - 1.0)
gaussian:  u1 = unit(pixel_hash(seed, x, y, k1))
           u2 = unit(pixel_hash(seed, x, y, k2))
           rr = sqrt(-2.0 * log(1.0 - u1))        # 1.0 - u1 is exact and in (0, 1]; libm log
           z  = rr * cos(6.283185307179586 * u2)  # libm cos; 6.283185307179586 = 2*K_PI
           n  = sigma * z
F.c = q((O.c + n) / 255.0)                        # O.c converted to double exactly
F.A = O.A ; canonical rule
```

Box–Muller transform (G. E. P. Box and M. E. Muller, "A Note on the Generation of Random Normal
Deviates", Ann. Math. Statist. 29(2), 1958), cosine branch only. The reference evaluates the
gaussian branch with Python `math.log`, `math.cos`, `math.sqrt` on scalars per pixel (C1); the
uniform branch has no transcendentals and may be vectorised (`numpy.uint64` arithmetic wraps
modulo 2^64 as C6 requires). `-2.0 * log(1.0)` is `-0.0` and `sqrt(-0.0) = -0.0`, harmless.

**Op.**

```json
{"op": "filter_add_noise", "layer": "p", "amount": 25.0, "distribution": "gaussian",
 "monochromatic": false, "seed": 7}
```

| field | type | default | range |
|---|---|---|---|
| `layer` | string | required | raster layer id |
| `amount` | double | 10.0 | 0.0..400.0 (percent of 255 levels) |
| `distribution` | string | `"uniform"` | `"uniform"`, `"gaussian"` |
| `monochromatic` | bool | false | |
| `seed` | int | 0 | 0..9007199254740991 (2^53 − 1, so every JSON parser holds it exactly) |
| `coverage` | object | omitted | B0.2 |

### B5. High Pass

**Semantics.** Keeps only detail finer than `radius`: flat areas become mid-grey 128, edges
become lighter/darker than grey by their difference from the blurred image. Alpha is kept.

**Formula.** `Bl = GaussianBlur(O, radius, edge)` (full B1 output bytes). Per pixel, per colour
channel:

```
F.c = q(((O.c - Bl.c) / 255.0) + 0.5)            # O.c - Bl.c is an exact int
F.A = O.A ; canonical rule
```

`O == Bl` gives `q(0.5) = 128`.

Provenance: GEGL `high-pass` builds `over(input, opacity(0.5, invert(blur)))` then a contrast
multiply around 0.5, i.e. `0.5 + contrast·(input − blur)/2`
(https://gitlab.gnome.org/GNOME/gegl/-/blob/master/operations/common/high-pass.c, `attach`). Ours is
that structure at contrast 2 (full-scale difference), in gamma-encoded space with no gamma
round trip.

**Op.**

```json
{"op": "filter_high_pass", "layer": "p", "radius": 3.0}
```

| field | type | default | range |
|---|---|---|---|
| `layer` | string | required | raster layer id |
| `radius` | double | 10.0 | 0.1..250.0 (σ, passed to B1) |
| `edge` | string | `"clamp"` | edge mode of the inner blur |
| `coverage` | object | omitted | B0.2 |

### B6. Offset

**Semantics.** Moves the layer's pixels by `(dx, dy)` whole pixels (positive = right/down) and
fills the vacated area according to `mode`: transparent, repeat the edge pixels, or wrap around.

**Formula** (exact byte copy, no arithmetic on colour):

```
sx = x - dx ; sy = y - dy
transparent: F(x,y) = O(sx,sy) if 0 <= sx < W and 0 <= sy < H else (0,0,0,0)
repeat:      F(x,y) = O(min(max(sx,0),W-1), min(max(sy,0),H-1))
wrap:        F(x,y) = O(sx mod W, sy mod H)            # floor-mod: result in [0, W) even for sx < 0
```

In C++ `mod` is `((sx % W) + W) % W`; in Python `%` already floors.

**Op.**

```json
{"op": "filter_offset", "layer": "p", "dx": 20, "dy": -13, "mode": "wrap"}
```

| field | type | default | range |
|---|---|---|---|
| `layer` | string | required | raster layer id |
| `dx` | int | 0 | -65536..65536 |
| `dy` | int | 0 | -65536..65536 |
| `mode` | string | `"transparent"` | `"transparent"`, `"repeat"`, `"wrap"` |
| `coverage` | object | omitted | B0.2 |

---

## Part C — Golden cases

All scripts live in `tests/scripts/` and end with `"out": "png8"`. Content ops (`add_layer` with
`fill`, `merge_down`) are doc 10's; the fixtures below are written in the BUILD-SPEC example
style and must be adjusted to doc 10's final field names (see open questions).

### C0. Fixtures (shared prefixes)

| name | canvas | ops |
|---|---|---|
| **RAMP** | 256×16, bg `#00000000` | `add_layer id=base fill=gradient from=#000000ff to=#ffffffff dir=h` |
| **SWEEP** | 192×96, bg `#00000000` | `add_layer id=base gradient #ff0000ff→#0000ffff dir=h`; `add_layer id=top gradient #00ff00ff→#00ff0000 dir=v`; (Normal, opacity 1) — gives saturated hues red→magenta→blue mixed with green top→bottom, plus a grey-ish region; `add_layer id=grey gradient #20202080→#e0e0e080 dir=h` on top at Normal — adds low-saturation colours |
| **ALPHA** | 128×32, bg `#00000000` | `add_layer id=base gradient #ff800000→#3060ffff dir=h` — alpha 0 at the left edge, partial alpha across, opaque at right |
| **FXA** | 64×48, bg `#00000000` | `add_layer id=p gradient #ff000000→#0000ffff dir=h`; `add_layer id=q gradient #00ff00c0→#ffff0000 dir=v`; `merge_down q` → one raster layer `p` with 2-D colour and alpha variation |
| **FXR** | 64×64, bg `#ffffffff` | `add_layer id=p fill=solid color=#20c060ff`; `filter_offset p dx=16 dy=12 mode=transparent` → an opaque green rectangle with hard edges over a white background layer |
| **FXS** | 96×64, bg `#ffffffff` | FXR's construction on 96×64, then `add_layer id=s fill=solid color=#d02040ff`; `filter_offset s dx=-40 dy=30 mode=transparent`; `merge_down s` → layer `p` holding two overlapping hard-edged rectangles (three colours incl. transparency) |

For adjustment goldens, the adjustment layer is added on top of the fixture with default
placement and the document composite is the output.

### C1. Adjustment goldens (24)

| # | file | fixture + op | what bug this catches |
|---|---|---|---|
| A01 | `adj_levels_identity` | RAMP + levels `{}` | LUT build drifts at defaults (e.g. pow applied at gamma 1, output formula `ow*v` vs `ob+v*(ow-ob)` off by an ulp that flips a byte) |
| A02 | `adj_levels_gamma_up` | RAMP + levels `rgb.gamma=2.2` | gamma applied as `v^gamma` instead of `v^(1/gamma)` (mutation 24) |
| A03 | `adj_levels_gamma_down` | SWEEP + levels `rgb.gamma=0.45` | mutation 24 on colour content; gamma applied to R only |
| A04 | `adj_levels_input_clip` | RAMP + levels `rgb.in_black=30, in_white=220` | missing input clamp (values <30 go negative → pow of negative) |
| A05 | `adj_levels_output_invert` | RAMP + levels `rgb.out_black=255, out_white=0, gamma=1.3` | crossed output range special-cased or swapped |
| A06 | `adj_levels_channel_then_composite` | SWEEP + levels `r={in_black:40,gamma:1.8}`, `b={out_white:200}`, `rgb={in_black:25,in_white:230,gamma:0.8}` | composite applied before the channel map; channel keys routed to the wrong channel |
| A07 | `adj_curves_identity` | RAMP + curves `{}` | identity curve not exact (solver run on 2 points, `B` not exactly 1.0) |
| A08 | `adj_curves_s` | RAMP + curves `rgb=[[0,0],[64,40],[192,215],[255,255]]` | wrong spline family (monotone/Catmull-Rom/Bezier instead of natural cubic); Thomas sweep errors |
| A09 | `adj_curves_endpoints` | RAMP + curves `rgb=[[48,20],[100,160],[208,230]]` | no flat extension outside the end points (mutation 25); output not clipped where the cubic overshoots |
| A10 | `adj_curves_16pt_red` | SWEEP + curves `r=` 16 points `[[0,0],[17,40],[34,20],[51,70],[68,50],[85,110],[102,90],[119,150],[136,130],[153,190],[170,170],[187,220],[204,200],[221,245],[238,230],[255,255]]` | long tridiagonal system (index off-by-one in the sweep), 16-point limit, curve routed only to R |
| A11 | `adj_curves_channel_then_composite` | SWEEP + curves `g=[[0,30],[128,100],[255,255]]`, `rgb=[[0,0],[90,140],[255,255]]` | composite before channel; composite not evaluated at non-integer input |
| A12 | `adj_bc_bright` | RAMP + brightness_contrast `brightness=0.5` | brightness not halved; brightness added instead of the proportional lerp toward white |
| A13 | `adj_bc_dark_contrast` | SWEEP + brightness_contrast `brightness=-0.6, contrast=0.4` | negative branch uses the positive formula; contrast pivot not 0.5; tan argument off by π/4 |
| A14 | `adj_bc_contrast_max` | RAMP + brightness_contrast `contrast=1.0` | slant `1.633123935319537e16` must give a clean cut (levels 0..127 → 0, 128..255 → 255) with no NaN/inf; implementations that special-case +1 differently |
| A15 | `adj_hs_hue_plus60` | SWEEP + hue_saturation `hue=60` | hue shift sign inverted (mutation 26); hue in degrees fed as sextants |
| A16 | `adj_hs_hue_minus150` | SWEEP + hue_saturation `hue=-150` | missing wrap below 0; `hsl_value` wrap using `>=6` vs `>6` inconsistently |
| A17 | `adj_hs_desat_light` | SWEEP + hue_saturation `saturation=-100, lightness=40` | desaturate not producing HSL-lightness grey; lightness positive branch |
| A18 | `adj_hs_sat_boost_dark` | SWEEP + hue_saturation `saturation=80, lightness=-50` | saturation not clamped to 1 after scaling; negative lightness branch |
| A19 | `adj_hs_colorize` | SWEEP + hue_saturation `colorize=true, hue=200, saturation=60, lightness=10` | colorize using source hue/saturation; hue 200° → sextant 3.333 conversion |
| A20 | `adj_bw_default` | SWEEP + black_white `{}` | min/mid/max decomposition wrong; reds/magentas or yellows/cyans weights swapped; plain luma used instead |
| A21 | `adj_bw_custom_tint` | SWEEP + black_white `reds=-50, yellows=250, greens=120, cyans=-10, blues=300, magentas=0, tint="#e1d3b3"` | no clamp of out-of-range grey before tint; tint not via HSL lightness; negative weights |
| A22 | `adj_invert_alpha` | ALPHA + invert | alpha modified; canonical transparent rule not applied (alpha-0 pixels must stay `(0,0,0,0)`, not `(255,255,255,0)`) |
| A23 | `adj_posterize_4` | RAMP + posterize `levels=4` | `n` used instead of `n-1` (mutation 27); truncation instead of rounding in the snap |
| A24 | `adj_threshold` | SWEEP + threshold `level=128` | D3 0.30/0.59/0.11 or Rec.709 used; `>` instead of `>=`; luma not rounded to an integer before comparing |

### C2. Filter goldens (18, plus one pending)

| # | file | fixture + op | what bug this catches |
|---|---|---|---|
| G01 | `flt_gauss_r3` | FXA + `filter_gaussian_blur p radius=3.0` | V-then-H order (mutation 12); straight-alpha blur (mutation 29: colour of transparent pixels bleeds in); per-pass rounding omitted |
| G02 | `flt_gauss_r1_mixed_widths` | FXS + `filter_gaussian_blur p radius=1.0` (widths [1,1,3]) | Kovesi `m` rounding / narrow-boxes-first order; even widths; width-1 pass not identity |
| G03 | `flt_gauss_r8_transparent_edge` | FXR + `filter_gaussian_blur p radius=8.0 edge=transparent` | edge mode ignored (opacity must fade at canvas borders); mutation 12 at large widths |
| G04 | `flt_gauss_r05_identity` | FXS + `filter_gaussian_blur p radius=0.5` | widths [1,1,1] must be an exact identity; an implementation using a true sampled Gaussian here differs |
| G05 | `flt_gauss_r45_ramp_coverage` | FXS + `filter_gaussian_blur p radius=4.5 coverage={src:ramp,dir:h}` | coverage lerp in straight instead of premultiplied space; `M==0`/`M==255` short-cuts missing; ramp formula |
| G06 | `flt_motion_h10` | FXS + `filter_motion_blur p angle=0 distance=10` | N = ceil(d)+1 taps; tap spacing; horizontal kernel |
| G07 | `flt_motion_30_subpixel` | FXA + `filter_motion_blur p angle=30 distance=7.5` | bilinear weights per tap (split before adding x); ceil of non-integer distance; premultiplied accumulation |
| G08 | `flt_motion_90_transparent` | FXR + `filter_motion_blur p angle=90 distance=12 edge=transparent` | missing `1e-9` snap (tiny horizontal leak); transparent edge mode |
| G09 | `flt_motion_neg45` | FXS + `filter_motion_blur p angle=-45 distance=20` | y offset sign (clockwise vs counter-clockwise: -45 must smear toward bottom-right/top-left) |
| G10 | `flt_usm_basic` | FXS + `filter_unsharp_mask p amount=150 radius=2.0 threshold=0` | amount scale (percent), difference sign, alpha sharpened |
| G11 | `flt_usm_threshold` | FXS + `filter_unsharp_mask p amount=300 radius=1.5 threshold=8` | threshold compared on the 0..1 scale (mutation 28: nothing sharpened); `<=` vs `<` gate |
| G12 | `flt_noise_uniform` | FXS + `filter_add_noise p amount=25 distribution=uniform seed=7` | `k` stream assignment per channel; `unit` mapping `2u-1`; seed mixing; x/y swapped in `pixel_hash` |
| G13 | `flt_noise_gaussian` | FXS + `filter_add_noise p amount=40 distribution=gaussian seed=7` | Box–Muller form (`log(u1)` instead of `log(1-u1)`, sin instead of cos, σ not `A/√3`), k1/k2 pairing |
| G14 | `flt_noise_mono_alpha` | FXA + `filter_add_noise p amount=60 distribution=gaussian monochromatic=true seed=99` | mono must share one value across channels; alpha untouched; canonical rule on alpha-0 pixels |
| G15 | `flt_high_pass` | FXS + `filter_high_pass p radius=3.0` | 0.5 offset (flat = 128), GEGL's halved difference used, alpha changed |
| G16 | `flt_offset_transparent` | FXS + `filter_offset p dx=20 dy=-13 mode=transparent` | sign of dx/dy; vacated area not transparent |
| G17 | `flt_offset_wrap_neg` | FXS + `filter_offset p dx=-70 dy=5 mode=wrap` | truncating `%` for negative source coordinates; `|dx| > W` |
| G18 | `flt_offset_repeat_rect_cov` | FXS + `filter_offset p dx=11 dy=9 mode=repeat coverage={src:rect,x:8,y:8,w:48,h:40,value:128}` | repeat-edge clamping; partial rect coverage at a constant 128 |
| G19 | `flt_gauss_lock_alpha` *(pending doc 10 op name)* | FXA + lock-transparency on `p` (doc 10 op) + `filter_gaussian_blur p radius=3.0` | filter result alpha must be replaced by the original alpha before coverage |

`G01`'s ability to catch mutations 12 and 29 was checked with a NumPy prototype of B1 on an
equivalent 64×48 2-D colour/alpha fixture while writing this document (V-then-H changed 14 pixels,
no-premultiply changed 2,881). The golden generator must still run the mutation gate to confirm
it on the final fixture.

---

## Part D — Mutation hooks owned by this doc

Each hook is a single, precise change at one formula; `--selftest-mutate=N` enables exactly that
change. `refcomp.py` never implements mutations.

| id | location | defect injected | caught by |
|---|---|---|---|
| 12 | B1 pipeline | Stage 3 (all vertical passes) runs before Stage 2 (all horizontal passes) | G01, G03 (and every blur-derived golden: G02, G05, G10, G11, G15 wherever rounding differs) |
| 24 | A1 `L_S` | `v = pow(v, S.gamma)` instead of `pow(v, inv)` | A02, A03, A06 |
| 25 | A2 `S(x)` | the first line `x = clamp(x, X[0], X[m])` is removed: inputs below `X[0]` use segment 0 with negative `t`, inputs above `X[m]` use segment `m-1` with `t > H[m-1]` (cubic extrapolation, still clipped to 0..255) | A09 |
| 26 | A4 master formula | `h6 = h6 - dh` instead of `h6 + dh` | A15, A16 |
| 27 | A7 | `kd` is `n` instead of `n - 1` in both the scale and the divide | A23 |
| 28 | B3 | the gate compares `abs(d) / 255.0 < P.threshold` (normalised difference against a level count) | G11 |
| 29 | B1 Stage 1 / Stage 4 | no premultiply: Stage 1 uses `Pc = C8 * 255`; Stage 4 uses `C = Pc / 65025.0` (straight colour blurred independently of alpha) | G01, G03, G05 |

---

## Part E — Parity notes (feeds `docs/PARITY.md`)

Nothing below was checked against Adobe software or its output (BUILD-SPEC prohibitions). Every
row is either a documented design decision or a stated unknown.

1. **Levels** — order of per-channel vs composite application in Photoshop is **unknown**; we use
   GIMP's (channel first). We keep full double precision between the two stages; an 8-bit
   editor may compose byte LUTs instead (unknown; could differ by 1 level). Gamma range
   0.1–9.99; whether Photoshop's gamma is exactly `v^(1/gamma)` is unknown. "Auto" levels,
   eyedroppers and clipping display are not in v0.1.
2. **Curves** — interpolation: natural cubic spline (Krita). Photoshop's curve interpolant is
   **unknown**; curves with sharp bends will differ. We cap at 16 points; the public PSD spec
   allows 2–19 points per curve, so a PSD with 17–19 points cannot be represented losslessly by
   this adjustment in v0.1 (import policy belongs to the PSD doc). Pencil (freehand) curves are
   not supported.
3. **Brightness/Contrast** — Adobe's non-legacy formula is **unpublished**. We use GIMP's
   (`brightness/2` lerp, `tan` slope around 0.5) with parameters in −1..1. This is a deliberate
   divergence: results will differ from Photoshop at the same slider values, and the mapping of
   Photoshop's −150..150 / −50..100 slider ranges onto ours is undecided. "Use Legacy" is not
   implemented.
4. **Hue/Saturation** — HSL model with GIMP's multiplicative saturation and un-halved lightness
   lerp. Photoshop's lightness is widely described as a blend toward black/white after hue and
   saturation, and its saturation curve is **unknown**; results will differ, especially for
   saturation > 0 and lightness ≠ 0. Per-range (Reds/Yellows/…) editing and the range sliders are
   not in v0.1 (master only). Colorize uses source HSL lightness (unknown whether Photoshop does).
5. **Black & White** — the min/mid/max decomposition is our own; it matches the documented
   behaviour on pure primaries/secondaries and greys, but equality with Photoshop on mixed colours
   is **unknown**. Tint is HSL recolouring at the grey's lightness (Photoshop's tint method
   unknown). Presets and "Auto" are not implemented.
6. **Invert** — exact (`255 − v`); no known divergence.
7. **Posterize** — nearest of `n` evenly spaced levels (GIMP form). Photoshop's level placement
   and rounding are **unknown**.
8. **Threshold** — BT.601 integer luma, `>=` comparison. Photoshop's luma weights and comparison
   are **unknown**.
9. **Gaussian Blur** — a 3-box approximation, not a true Gaussian; `radius` is interpreted as σ.
   Whether Photoshop's "radius" equals σ is **unknown** (commonly believed to be close). For
   radius < ~0.9 the Kovesi widths are all 1 and the filter is an exact identity — Photoshop blurs
   visibly at 0.3–0.9 px. Edge default is `clamp` (repeat edge pixels); Photoshop's edge handling
   on layers that do not cover the canvas is unknown.
10. **Motion Blur** — uniform line kernel, bilinear taps, `ceil(d)+1` samples (GEGL). Photoshop's
    kernel shape and its exact interpretation of "distance" are **unknown**; angle convention
    (counter-clockwise positive) is believed to match.
11. **Unsharp Mask** — per-channel threshold gate on `|orig − blur|` in levels. Photoshop's
    threshold semantics (per channel vs luminance, strict vs non-strict) are **unknown**. Alpha is
    never sharpened.
12. **Add Noise** — amount = percent of 255 levels; gaussian σ chosen equal to the uniform's
    standard deviation. Photoshop's amount scaling and gaussian σ are **unknown**; its noise is
    of course not the same random sequence (deliberate, C6).
13. **High Pass** — `0.5 + (orig − blur)` with the Gaussian above. Photoshop's radius mapping for
    High Pass is **unknown**; the 128 neutral grey is believed to match.
14. **Offset** — exact; "wrap", "repeat edge" and "transparent" match Photoshop's three
    documented choices. Photoshop's default for layers is not asserted; ours is `transparent`.
15. **Filters and selections** — a filter is computed on the whole layer and then mixed with the
    selection coverage in premultiplied space. Whether Photoshop confines neighbour reads to the
    selection is **unknown**. Filters on layer masks (mask as target) are not in v0.1.
16. **No destructive Image → Adjustments** menu in v0.1; the eight adjustments exist only as
    adjustment layers.
