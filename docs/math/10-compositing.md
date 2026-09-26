# Rasterloom normative math — 10: compositing

Status: normative for v0.1. Inherits every rule of `00-conventions.md` (C1–C8). Where this file
and `00-conventions.md` seem to disagree, `00-conventions.md` wins and the disagreement is a bug
to report.

This file defines: the layer tree and its node kinds, the straight-alpha composite primitive, all
27 blend modes plus Pass Through, opacity versus fill, visibility, layer masks, clipping masks
(clip groups, `clbl`), groups (pass-through and isolated), where an adjustment layer's output
enters the stack, lock-transparency, Merge Down / Merge Visible / Flatten, the canvas background,
the core render-script ops, the compositing golden set, and mutation hooks 0–6, 14 and 16–23.

The adjustment functions themselves (what Invert, Levels, … compute) are doc 20. This file only
consumes them as a black box `ADJ(node, Rb, Gb, Bb) -> (R', G', B')` on bytes.

Notation used throughout:

- A byte is an integer in `0..255`. `n(v) = v / 255.0` is the C2 decode (a division).
- `q(x)` is the C2 quantiser. `clamp01(x) = min(max(x, 0.0), 1.0)`.
- Every formula is evaluated exactly as written (C1): parentheses as shown, left to right for equal
  precedence, no algebraic rewriting, no FMA.
- "Canonicalise" means the C3 rule: if the quantised alpha byte is 0, the stored pixel is
  `(0, 0, 0, 0)`. **Every pixel written by any formula in this file is canonicalised.**

---

## 1. Semantics in words

A document is a canvas of `w × h` pixels, a background colour `bg`, and an ordered tree of nodes.
The root and every group hold an ordered child list, **index 0 = bottom**. Rendering starts from a
buffer filled with `bg` and composites the root's children bottom to top onto it. Each node kind:

- **Raster layer** — its own 8-bit straight RGBA pixels (canvas-sized, sparse; pixels never written
  are `(0,0,0,0)`).
- **Adjustment layer** (doc 20) — no pixels; it recolours whatever is already below it, within its
  own coverage (mask × fill × opacity), and never changes the alpha of what is below.
- **Group** — a child list plus a mode: **pass-through** (`pass`, the default for new groups: the
  children composite directly onto the running backdrop, so their blend modes and any adjustment
  layers inside reach everything below the group) or **isolated** (any other blend mode: the
  children are rendered onto a transparent buffer first, then that buffer is composited like a
  layer with the group's blend mode).

Every node has: `visible` (bool, default true), `opacity` (double in [0,1], default 1.0), an
optional 8-bit layer mask with an enabled flag. Raster and adjustment layers additionally have a
blend mode (default `norm`), `fill` (double in [0,1], default 1.0), `clip` (bool, default false),
`clbl` (bool, default true), a Dissolve `seed` (default 0). Raster layers have `lock_alpha`
(default false). Groups have a mode and a Dissolve `seed`, and **no fill** in v0.1.

**Opacity versus fill.** For a lone raster or adjustment layer both simply scale how much of the
layer reaches the backdrop, in every blend mode (the two differ only in the order of one
multiplication, pinned in §4.1). They differ structurally on a **clip base**: the base's *opacity*
fades the whole clip group (base and everything clipped to it); the base's *fill* fades only the
base's own pixels, while the clipped layers still appear inside the base's shape. A base with fill 0
is an invisible stencil: its clipped layers show through its shape, its own colour does not. See
§6 and the Parity notes for the Photoshop "special eight modes" question.

**Clipping.** A layer with `clip = true` is clipped to the nearest non-clipped raster layer below it
in the same container (its *base*); the base plus the consecutive clipped layers above it form a
*clip group*. Clipped layers only appear inside the base's shape (base pixel alpha × base mask).
With `clbl = true` on the base (default, "blend clipped layers as group"), the clipped layers are
blended onto the base's colour and the finished clip group is composited onto the backdrop with the
base's blend mode and opacity (BUILD-SPEC D4). With `clbl = false`, the base composites on its own
and each clipped layer composites directly onto the running backdrop with its own blend mode,
restricted to the base's shape and faded by the base's opacity.

**Masks.** A layer mask is an 8-bit grey plane; its value multiplies the node's coverage (255 = full,
0 = hidden). A disabled mask is ignored as if absent; delete removes it; apply bakes it into the
layer's pixel alpha and removes it.

**Lock transparency.** Painting into a raster layer with `lock_alpha = true` changes colour only;
every pixel's alpha byte is preserved exactly (§10).

---

## 2. The value domain of blend inputs (an invariant the formulas rely on)

Every backdrop and every source colour that enters a blend function `B` in this file is a byte
decoded with `n(v)`: layer pixels are bytes, every intermediate buffer is bytes (C5), and adjustment
output is bytes (C5). Coverage (alpha × mask × fill × opacity) is a double and never enters `B`.
Two modes use this invariant to compare **integers** instead of doubles (Darker Color, Lighter
Color, Hard Mix), which removes every floating-point tie question from them.

Consequence: `cs = 0.5` never occurs (127.5 is not a byte), so `cs <= 0.5` is the same as
`Sbyte <= 127`; implementations may test either.

---

## 3. Blend functions `B(cb, cs)`

`cb` = backdrop channel, `cs` = source channel, both `n(byte)`. Separable modes are applied per
channel R, G, B independently. Non-separable modes take and return a 3-vector. **Every mode's
result passes through `clamp01` per channel before use** (§4.1 step 4); where a formula below also
clamps internally, both clamps are applied.

### 3.1 JSON spelling

Script `mode` strings are the PSD blend-mode 4CCs from the Adobe PSD file-format specification
("Blend mode key", Layer records section, https://www.adobe.com/devnet-apps/photoshop/fileformatashtml/),
**with trailing spaces removed**, case-sensitive. A string with a trailing space, or any string not
in this table, is a script error.

| # | Mode | PSD key | JSON `mode` | Kind |
|---|---|---|---|---|
| — | Pass Through | `'pass'` | `"pass"` | groups only |
| 1 | Normal | `'norm'` | `"norm"` | special |
| 2 | Dissolve | `'diss'` | `"diss"` | special |
| 3 | Darken | `'dark'` | `"dark"` | separable |
| 4 | Multiply | `'mul '` | `"mul"` | separable |
| 5 | Color Burn | `'idiv'` | `"idiv"` | separable |
| 6 | Linear Burn | `'lbrn'` | `"lbrn"` | separable |
| 7 | Darker Color | `'dkCl'` | `"dkCl"` | non-separable (integer) |
| 8 | Lighten | `'lite'` | `"lite"` | separable |
| 9 | Screen | `'scrn'` | `"scrn"` | separable |
| 10 | Color Dodge | `'div '` | `"div"` | separable |
| 11 | Linear Dodge (Add) | `'lddg'` | `"lddg"` | separable |
| 12 | Lighter Color | `'lgCl'` | `"lgCl"` | non-separable (integer) |
| 13 | Overlay | `'over'` | `"over"` | separable |
| 14 | Soft Light | `'sLit'` | `"sLit"` | separable |
| 15 | Hard Light | `'hLit'` | `"hLit"` | separable |
| 16 | Vivid Light | `'vLit'` | `"vLit"` | separable |
| 17 | Linear Light | `'lLit'` | `"lLit"` | separable |
| 18 | Pin Light | `'pLit'` | `"pLit"` | separable |
| 19 | Hard Mix | `'hMix'` | `"hMix"` | separable (integer) |
| 20 | Difference | `'diff'` | `"diff"` | separable |
| 21 | Exclusion | `'smud'` | `"smud"` | separable |
| 22 | Subtract | `'fsub'` | `"fsub"` | separable |
| 23 | Divide | `'fdiv'` | `"fdiv"` | separable |
| 24 | Hue | `'hue '` | `"hue"` | non-separable |
| 25 | Saturation | `'sat '` | `"sat"` | non-separable |
| 26 | Color | `'colr'` | `"colr"` | non-separable |
| 27 | Luminosity | `'lum '` | `"lum"` | non-separable |

For groups only, the extra alias `"isolated"` is accepted and means `"norm"` (an isolated group in
Normal mode). `"pass"` on a raster or adjustment layer is a script error.

### 3.2 W3C separable modes

Source: W3C Compositing and Blending Level 1, §10.1, https://www.w3.org/TR/compositing-1/#blendingseparable
(verbatim pseudo-code read on 2026-09-26). Helper forms, used by several modes below:

```
Multiply(a, b) = a * b
Screen(a, b)   = 1.0 - ((1.0 - a) * (1.0 - b))
ColorDodge(cb, s):                 # W3C #blendingcolordodge; the guard ORDER is normative
    if cb == 0.0:  return 0.0
    if s  == 1.0:  return 1.0
    return min(1.0, cb / (1.0 - s))
ColorBurn(cb, s):                  # W3C #blendingcolorburn; the guard ORDER is normative
    if cb == 1.0:  return 1.0
    if s  == 0.0:  return 0.0
    return 1.0 - min(1.0, (1.0 - cb) / s)
HardLight(cb, s):                  # W3C #blendinghardlight
    if s <= 0.5:   return Multiply(cb, 2.0 * s)
    return Screen(cb, (2.0 * s) - 1.0)
```

| JSON | `B(cb, cs)` in evaluation order | Source |
|---|---|---|
| `norm` | `cs` (see §4.1: Normal skips the `mixed` lerp) | W3C #blendingnormal |
| `mul` | `Multiply(cb, cs)` = `cb * cs` | W3C #blendingmultiply |
| `scrn` | `Screen(cb, cs)` = `1.0 - ((1.0 - cb) * (1.0 - cs))` | W3C #blendingscreen |
| `over` | `HardLight(cs, cb)`, i.e. `if cb <= 0.5: cs * (2.0 * cb)` else `1.0 - ((1.0 - cs) * (1.0 - ((2.0 * cb) - 1.0)))` | W3C #blendingoverlay |
| `dark` | `min(cb, cs)` | W3C #blendingdarken |
| `lite` | `max(cb, cs)` | W3C #blendinglighten |
| `div` | `ColorDodge(cb, cs)` | W3C #blendingcolordodge |
| `idiv` | `ColorBurn(cb, cs)` | W3C #blendingcolorburn |
| `hLit` | `HardLight(cb, cs)`, i.e. `if cs <= 0.5: cb * (2.0 * cs)` else `1.0 - ((1.0 - cb) * (1.0 - ((2.0 * cs) - 1.0)))` | W3C #blendinghardlight |
| `sLit` | see below | W3C #blendingsoftlight |
| `diff` | `abs(cb - cs)` | W3C #blendingdifference |
| `smud` | `(cb + cs) - ((2.0 * cb) * cs)` | W3C #blendingexclusion |

Soft Light, exactly W3C (BUILD-SPEC D3; never Pegtop):

```
if cs <= 0.5:
    B = cb - (((1.0 - (2.0 * cs)) * cb) * (1.0 - cb))
else:
    if cb <= 0.25:  D = ((((16.0 * cb) - 12.0) * cb) + 4.0) * cb
    else:           D = sqrt(cb)                                    # correctly rounded; np.sqrt is fine
    B = cb + (((2.0 * cs) - 1.0) * (D - cb))
```

The W3C text `Cb - (1 - 2 x Cs) x Cb x (1 - Cb)` is read left to right as
`((1 - 2*Cs) * Cb) * (1 - Cb)`, which is what the block above spells out. `D` depends on `cb` (the
backdrop), not on `cs`.

### 3.3 The eleven non-W3C modes (plus Dissolve, §3.5)

Sources: Krita `KoCompositeOpFunctions.h`
(https://github.com/KDE/krita/blob/master/libs/pigment/compositeops/KoCompositeOpFunctions.h, GPL,
read 2026-09-26) and GIMP `gimpoperationlayermode-blend.c`
(https://gitlab.gnome.org/GNOME/gimp/-/blob/master/app/operations/layer-modes/gimpoperationlayermode-blend.c,
GPL, read 2026-09-26). Where they differ, the choice is stated. Rasterloom's own evaluation order
is the one written here (it is not copied from either).

| JSON | `B(cb, cs)` in evaluation order | Guards / notes | Provenance |
|---|---|---|---|
| `lbrn` | `max(0.0, (cb + cs) - 1.0)` | — | Krita `CFLinearBurn`; GIMP `blend_linear_burn` |
| `lddg` | `min(1.0, cb + cs)` | — | Krita `cfAddition`; GIMP `blend_addition` |
| `fsub` | `max(0.0, cb - cs)` | backdrop minus source | Krita `cfSubtract` (`dst - src`); GIMP `blend_subtract` |
| `fdiv` | `if cb == 0.0: 0.0` / `elif cs == 0.0: 1.0` / `else: min(1.0, cb / cs)` | **guard order normative**: `B(0,0) = 0`, `B(x>0, 0) = 1` | Krita `CFDivide` (identical guards) |
| `vLit` | `if cs <= 0.5: ColorBurn(cb, 2.0 * cs)` / `else: ColorDodge(cb, (2.0 * cs) - 1.0)` | inherits both W3C guards: `cs = 0` → `1.0` if `cb == 1.0` else `0.0`; `cs = 1` → `0.0` if `cb == 0.0` else `1.0` | Krita `CFVividLight` (same guards, different arithmetic form); GIMP `blend_vivid_light` |
| `lLit` | `clamp01((cb + (2.0 * cs)) - 1.0)` | — | GIMP `blend_linear_light` (`in + 2*layer - 1`); Krita `cfLinearLight` |
| `pLit` | `if cs <= 0.5: min(cb, 2.0 * cs)` / `else: max(cb, (2.0 * cs) - 1.0)` | — | GIMP `blend_pin_light`; Krita `CFPinLight` (equivalent on [0,1]) |
| `hMix` | **integer**: `if (Sbyte + Bbyte) >= 255: 1.0 else 0.0` | threshold is `>= 255` (see Parity notes) | GIMP `blend_hard_mix` (`in + layer < 1 ? 0 : 1`) |
| `dkCl` | integer luma `Ls = 30*Rs + 59*Gs + 11*Bs`, `Lb = 30*Rb + 59*Gb + 11*Bb` on bytes; `if Ls < Lb: (cs_r, cs_g, cs_b) else (cb_r, cb_g, cb_b)` | **tie keeps the backdrop** | Krita `CFDarkerColor` (`if lum(dst) > lum(src) → src`, tie keeps dst) |
| `lgCl` | same `Ls`, `Lb`; `if Ls > Lb: source vector else backdrop vector` | **tie keeps the backdrop** | Krita `CFLighterColor` (`if lum(dst) < lum(src) → src`) |

`ColorDodge(cb, (2.0*cs) - 1.0)` hits its `s == 1.0` guard exactly when `cs == 1.0`
(`2.0*1.0 - 1.0 = 1.0` exactly). `ColorBurn(cb, 2.0*cs)` hits `s == 0.0` exactly when `cs == 0.0`.

The integer luma of `dkCl`/`lgCl` uses the same weights as the W3C `Lum` (0.30/0.59/0.11, D3)
scaled by 100; comparing integers is exact, so ties are decided by the stated rule and never by
floating-point noise.

### 3.4 W3C non-separable modes

Source: W3C Compositing-1 §10.2, https://www.w3.org/TR/compositing-1/#blendingnonseparable.
Luma weights 0.3/0.59/0.11 (D3, never Rec.709). `C` is a 3-vector `(r, g, b)` of doubles.

```
Lum(C) = ((0.3 * C.r) + (0.59 * C.g)) + (0.11 * C.b)

ClipColor(C):                           # L, n, x are computed ONCE, on entry (as W3C writes it)
    L = Lum(C)
    n = min(min(C.r, C.g), C.b)
    x = max(max(C.r, C.g), C.b)
    if n < 0.0:
        d1 = L - n
        if d1 > 0.0:                    # Rasterloom guard: no 0/0 (W3C has none)
            for ch in (r, g, b):  C.ch = L + (((C.ch - L) * L) / d1)
    if x > 1.0:
        d2 = x - L
        if d2 > 0.0:                    # Rasterloom guard
            for ch in (r, g, b):  C.ch = L + (((C.ch - L) * (1.0 - L)) / d2)
    return C

SetLum(C, l):
    d = l - Lum(C)
    return ClipColor((C.r + d, C.g + d, C.b + d))

Sat(C) = max(max(C.r, C.g), C.b) - min(min(C.r, C.g), C.b)

SetSat(C, s):
    mx = max(max(C.r, C.g), C.b);  mn = min(min(C.r, C.g), C.b)
    if not (mx > mn):  return (0.0, 0.0, 0.0)
    imax = the FIRST index in order (r, g, b) whose value == mx
    imin = the FIRST index in order (r, g, b) whose value == mn     # differs from imax since mx > mn
    imid = the remaining index
    out[imax] = s
    out[imid] = ((C[imid] - C[imin]) * s) / (C[imax] - C[imin])
    out[imin] = 0.0
    return out
```

The index rule matters: when two channels tie for the maximum, the one labelled `imax` gets exactly
`s` and the other gets `((mx - mn) * s) / (mx - mn)`, which can differ from `s` in the last bit.
`numpy.argmax`/`argmin` return the first occurrence, which is exactly this rule.

| JSON | `B(Cb, Cs)` |
|---|---|
| `hue` | `SetLum(SetSat(Cs, Sat(Cb)), Lum(Cb))` |
| `sat` | `SetLum(SetSat(Cb, Sat(Cs)), Lum(Cb))` |
| `colr` | `SetLum(Cs, Lum(Cb))` |
| `lum` | `SetLum(Cb, Lum(Cs))` |

The result is then `clamp01`'d per channel (§4.1 step 4). The `d1 > 0.0` / `d2 > 0.0` guards only
fire on degenerate near-grey inputs where FP error makes all three channels equal and out of range;
the final `clamp01` then decides the value. This keeps NaN away from `q` (C2).

### 3.5 Dissolve

Dissolve replaces the coverage `c` of the node being composited (after mask, fill, opacity and, for
a clip group, the clip-group factors) by a hard 0 or 1, then composites as Normal:

```
u  = unit(pixel_hash(seed, x, y, 5))        # C6; x, y canvas pixel coordinates; stream k = 5
c' = 1.0 if u < c else 0.0
```

`seed` is the node's Dissolve seed (set by `set_blend`, default 0). Because `u < 1.0` always, a
fully covered pixel (`c = 1.0`) is always kept; `c = 0.0` is always dropped.

---

## 4. The composite primitives

### 4.1 `COMPOSITE(backdrop, Cs, c, mode, seed)` — a pixel source onto a backdrop

Inputs per pixel: backdrop bytes `(Rb, Gb, Bb, Ab)`, source colour bytes `(Rs, Gs, Bs)`, coverage
`c` (a double, computed by the caller), blend mode, Dissolve seed. Output: new backdrop bytes.

```
1. if mode == diss:  c = (1.0 if unit(pixel_hash(seed, x, y, 5)) < c else 0.0);  mode = norm
2. ab = n(Ab);  as = c
3. ao = as + (ab * (1.0 - as))
   if ao == 0.0:  return (0, 0, 0, 0)
4. cb = n(Rb, Gb, Bb) per channel;  cs = n(Rs, Gs, Bs) per channel
   bl = clamp01(B(cb, cs)) per channel          # vector call for non-separable modes
5. per channel:
   if mode == norm:  mixed = cs                 # exactly; no lerp for Normal
   else:             mixed = ((1.0 - ab) * cs) + (ab * bl)
   t1 = as * mixed
   t2 = (ab * cb) * (1.0 - as)
   co = (t1 + t2) / ao
6. return canonicalise(q(co_r), q(co_g), q(co_b), q(ao))
```

This is W3C Compositing-1 §6 "General Formula" (https://www.w3.org/TR/compositing-1/#generalformula:
`Cs = (1 - αb) x Cs + αb x B(Cb, Cs)` followed by source-over `co = αs x Cs + αb x Cb x (1 - αs)`,
`αo = αs + αb x (1 - αs)`, https://www.w3.org/TR/compositing-1/#porterduffcompositingoperators_srcover)
with the premultiplied result divided by the unquantised `ao` (C3) and stored straight.

**Skipping is exact.** If `c == 0.0` the output bytes equal the backdrop bytes: `ao = ab` exactly,
and `q((ab*cb)/ab)` returns the original byte because `(ab*cb)/ab` is within a few ulp of
`k/255`, far from a `.5` boundary. Implementations MAY therefore skip pixels (or whole empty
source tiles) with `c == 0.0`. No other shortcut is permitted unless proven byte-identical.

**SIMD.** The Highway Normal path (BUILD-SPEC addendum) must reproduce steps 2–6 with
`mixed = cs` exactly, in this operation order.

### 4.2 `ADJUST(backdrop, node, c, mode, seed)` — an adjustment layer onto a backdrop

An adjustment layer has no pixels; its "pixel alpha" is 1.0 everywhere. Its source colour is the
doc-20 function of the backdrop bytes; it never changes the backdrop's alpha.

```
1. if Ab == 0:  return (0, 0, 0, 0)              # nothing to recolour
2. if mode == diss:  c = (1.0 if unit(pixel_hash(seed, x, y, 5)) < c else 0.0);  mode = norm
3. (Ra, Ga, Ba) = ADJ(node, Rb, Gb, Bb)          # doc 20; bytes in, bytes out
   cb = n(Rb, Gb, Bb);  cs = n(Ra, Ga, Ba)
   bl = cs if mode == norm else clamp01(B(cb, cs))   (per channel / vector)
4. per channel:  co = ((1.0 - c) * cb) + (c * bl)
5. return canonicalise(q(co_r), q(co_g), q(co_b), Ab)     # alpha byte unchanged
```

`ADJ` may depend on the whole backdrop buffer only if doc 20 says so (v0.1's eight adjustments are
all per-pixel). The backdrop an adjustment reads is whatever buffer it is being composited onto:
the running backdrop (root, pass-through group, `clbl = false` clip), an isolated group's buffer,
or a clip group's interior buffer `G` (§6).

### 4.3 Coverage of a node

```
m   = n(mask byte at (x, y))   if the node has a mask and it is enabled
    = 1.0                      otherwise
raster:      c = ((n(A) * m) * fill) * opacity         # A = the layer's own alpha byte at (x, y)
adjustment:  c = ((1.0 * m) * fill) * opacity
group (isolated, onto its parent's backdrop):  c = (n(A_T) * m) * opacity   # A_T = group buffer alpha
```

Multiplying by `m = 1.0` is exact, so "no mask" and "mask of 255 everywhere" give identical bytes.

---

## 5. Rendering a container

`RENDER(children, D)` composites a child list (index 0 = bottom) onto the byte buffer `D` and returns
the new buffer. The document output is `RENDER(root.children, BG)` where `BG` is a canvas-sized
buffer filled with `canonicalise(bg)`.

```
effclip(node) = node.clip and node.kind in {raster, adjustment}      # a group is never clipped
i = 0
while i < len(children):
    node = children[i]
    if effclip(node):                       # bottom of container, or its base is not a raster layer
        if node.visible:  D = LONE(node, D) # rendered as an ordinary unclipped layer
        i = i + 1;  continue
    j = i + 1;  run = []
    if node.kind == raster:
        while j < len(children) and effclip(children[j]):
            run.append(children[j]);  j = j + 1
    if not node.visible:  i = j;  continue  # a hidden base hides its whole clip group
    vis = [r for r in run if r.visible]
    if vis:  D = CLIPGROUP(node, vis, D)    # §6
    else:    D = LONE(node, D)
    i = j
return D
```

When the base is not a raster layer (adjustment or group), the loop does not collect a run, so each
following clipped layer reaches the `effclip(node)` branch and is rendered unclipped.

```
LONE(node, D):
    raster:      per pixel D = COMPOSITE(D, node.RGB, c, node.mode, node.seed)          # c per §4.3
    adjustment:  per pixel D = ADJUST(D, node, c, node.mode, node.seed)
    group:       GROUP(node, D)                                                          # §7
```

Pixel iteration order is row-major (C4); every formula here is per pixel with no running state, so
the order does not affect the result.

---

## 6. Clip groups

Let the base be raster layer `L0` (bytes `R0, G0, B0, A0`, mask factor `m0` per §4.3, fill `f0`,
opacity `o0`, mode `M0`, seed `s0`) and `vis` its visible clipped layers bottom to top.

**Base shape** (independent of fill and opacity):

```
S = n(A0) * m0
```

### 6.1 `clbl = true` (default): blend clipped layers as a group

The clip group is built in a byte buffer `G` whose alpha channel stores **relative coverage `r`
inside the base shape** (not absolute alpha), then composited once.

```
1. Seed: every pixel of G = canonicalise(R0, G0, B0, q(f0))      # colour = base colour, alpha byte = q(fill)
2. For each clipped node Li in vis, bottom to top:
       ci = the coverage of Li per §4.3 (its own alpha, mask, fill, opacity) — NOT multiplied by S
       raster:     G = COMPOSITE(G, Li.RGB, ci, Li.mode, Li.seed)
       adjustment: G = ADJUST(G, Li, ci, Li.mode, Li.seed)
3. Composite the group:
       cg = (S * n(A_G)) * o0
       D  = COMPOSITE(D, G.RGB, cg, M0, s0)
```

Consequences, each of which a golden pins: the result alpha never exceeds `S * o0` (clipped content
cannot extend the base's shape even where the base is partially transparent); base fill 0 seeds
`r = 0`, so clipped layers appear in the base shape with their own colours and the base's colour
does not show; base opacity 0 hides everything; the base's blend mode and opacity apply to the whole
group; the clipped layers' modes act against the base colour only (not against the backdrop).

A base with no *visible* clipped layer takes the `LONE` path (§5), not this one: the `q(f0)` of
step 1 would otherwise change the rounding of a plain layer.

### 6.2 `clbl = false`: clipped layers blend individually onto the backdrop

```
1. D = LONE(L0, D)                                  # the base on its own: c0 = ((n(A0)*m0)*f0)*o0
2. For each Li in vis, bottom to top:
       ci  = coverage of Li per §4.3
       ci' = (ci * S) * o0
       raster:     D = COMPOSITE(D, Li.RGB, ci', Li.mode, Li.seed)
       adjustment: D = ADJUST(D, Li, ci', Li.mode, Li.seed)
```

Here each clipped layer's blend mode acts against everything below (base already composited onto
the backdrop), restricted to the base shape `S` and faded by the base opacity `o0`, but not by the
base fill `f0`.

### 6.3 The distinction the mutation gate needs

- Correct: clipped coverage inside `G` is `ci` (relative to the shape); the shape `S` is applied
  exactly once, in step 3. Multiplying `ci` by `S` inside `G` applies it twice (mutation 5).
- Correct: `f0` only seeds `r`; `o0` only enters `cg`. Folding `f0` into `o0` (mutation 6) makes a
  fill-0 base hide its clipped layers.
- Correct: `S` includes the base's mask `m0` (mutation 23 drops it).

---

## 7. Groups

`GROUP(g, D)` for a visible group `g` with children `ch`, mode, opacity `og`, mask factor `mg`
(§4.3, a group mask), seed `sg`.

### 7.1 Pass-through (`mode == pass`)

```
1. B0 = D                                     # the pre-group backdrop (bytes)
2. R  = RENDER(ch, B0)                        # children composite directly onto the running backdrop
3. per pixel, lerp in premultiplied space:
       w  = mg * og
       a0 = n(A_B0);  ar = n(A_R)
       ao = ((1.0 - w) * a0) + (w * ar)
       if ao == 0.0:  pixel = (0, 0, 0, 0)
       else per channel:
           p  = (((1.0 - w) * a0) * c0) + ((w * ar) * cr)      # c0 = n(B0 byte), cr = n(R byte)
           co = p / ao
       pixel = canonicalise(q(co_r), q(co_g), q(co_b), q(ao))
4. return the lerped buffer
```

Because step 2 renders onto the real backdrop, an adjustment layer inside a pass-through group
recolours layers below the group (D4), and children's blend modes act against everything below.
Group opacity and mask are **one** lerp of the whole result against the pre-group backdrop, never a
per-child factor (mutation 19). The lerp is premultiplied so a transparent pre-group backdrop does
not bleed its (canonical black) colour into the result (psd-tools
`Compositor._apply_passthrough_source`, MIT, https://github.com/psd-tools/psd-tools/blob/main/src/psd_tools/composite/composite.py,
does the same interpolation).

Where `w == 1.0` the lerp returns `R`'s bytes exactly (`ao = ar` exactly and `q((ar*cr)/ar)`
is stable, as in §4.1). A group with no visible children returns `B0`'s bytes exactly. Either may
therefore be skipped.

### 7.2 Isolated (any other mode, including `"isolated"` = `norm`)

```
1. T  = RENDER(ch, TRANSPARENT)               # children onto a (0,0,0,0) buffer
2. per pixel:  c = (n(A_T) * mg) * og         # groups have no fill
               D = COMPOSITE(D, T.RGB, c, g.mode, sg)
```

An adjustment layer inside an isolated group recolours only the group's own content below it (it
reads `T`, whose transparent pixels stay transparent per §4.2 step 1).

Nested groups recurse through `RENDER`; clip groups are formed among siblings of any container.

---

## 8. Masks

- A mask is a canvas-sized byte plane (sparse; the value outside any written area is the mask's
  `outside` value from `add_mask`, default 0). Its factor is `m = n(byte)` per §4.3.
- **Enabled/disabled**: a disabled mask gives `m = 1.0`; its bytes are kept.
- **Delete**: the mask is removed; `m = 1.0`.
- **Apply** (raster layers only): per pixel `A' = q(n(A) * n(M))` with `M` the mask byte (applied even
  if the mask is disabled — Apply bakes the stored mask), colour bytes unchanged, then canonicalise,
  then the mask is removed. Apply ignores `lock_alpha` (it is a structural operation, not painting).
- A group mask enters `w` (pass-through) or `c` (isolated). An adjustment mask enters `c`.
- A clip base's mask is part of the clip shape `S` (§6).

---

## 9. Merge Down, Merge Visible, Flatten, background

All three produce raster pixels with this file's formulas; none of them re-renders later.

### 9.1 `merge_down` (upper `U` into the node `L` directly below it in the same container)

Preconditions (else script error): `U` is a raster or adjustment layer; `U` is not at index 0; `L`
is a raster layer; both are visible; the node directly above `U` (if any) does not have
`clip = true` (so no clipped layer changes its base).

```
1. L' pixels: A' = q((n(A_L) * m_L) * f_L), colour = L's colour, canonicalise    # bake L's mask and fill
2. if U.clip and not L.clip:               # U is clipped to L: bake the clip group
       G = §6.1 steps 1–2 with base L (using L's original A_L, m_L, f_L) and vis = [U]
       result pixel = canonicalise(G.RGB, q(S * n(A_G)))          # S = n(A_L) * m_L
   else:
       c_U per §4.3;  result = COMPOSITE(L', U.RGB, c_U, U.mode, U.seed)   (ADJUST for an adjustment U)
3. U is removed. L keeps its id, index, opacity, mode, seed, visible, clip, clbl, lock_alpha;
   L.fill = 1.0; L has no mask.
```

`L`'s own blend mode and opacity are not baked (they remain properties), so the merged layer renders
as "U applied onto L, then L composited as before". This matches the document render exactly only
when `L` is Normal-mode over a transparent backdrop; that is expected and recorded in Parity notes.

### 9.2 `merge_visible` (`id`, default `"merged"`)

```
1. P = RENDER(root.children, TRANSPARENT)       # NOT onto bg; hidden nodes do not render anyway
2. Remove every visible top-level node (with its whole subtree, hidden descendants included).
3. Insert a new raster layer with id, pixels P, into root at index k = the number of remaining
   (hidden) top-level nodes that were below the bottommost removed node; mode norm, opacity 1.0, fill 1.0, no mask, clip false,
   clbl true, seed 0, visible true, lock_alpha false.
4. Hidden top-level nodes stay, in their order. bg is unchanged.
If no top-level node is visible, merge_visible does nothing. A duplicate id (after step 2) is a
script error.
```

### 9.3 `flatten` (`id`, default `"flattened"`)

```
1. P = RENDER(root.children, BG)            # the full document render, bg included
2. Remove every node (hidden ones are discarded).
3. Insert one raster layer with id and pixels P (properties as in 9.2 step 3).
4. Set bg = #00000000.
```

Step 4 makes the flattened document render to exactly `P` (compositing `P` onto a transparent
buffer in Normal with `c = n(A)` returns `P`'s bytes, by the §4.1 stability argument). Without it a
partially transparent bg would be composited twice.

### 9.4 Canvas background

`bg` is the initial backdrop of the root render only; it is not a layer, cannot be selected, has no
mode, and is never included in `merge_visible`. A `bg` with alpha byte 0 is canonicalised to
`(0,0,0,0)`.

---

## 10. Lock transparency (for every pixel-writing op in other docs)

When a raster layer has `lock_alpha = true`, an op that *paints* its pixels with a source colour (brush, eraser, bucket,
gradient tool, clone — docs 30 and 40; filters follow their own rule in doc 20 §B0) computes, per pixel, its normal result **treating the
destination as the stored colour with alpha 1.0**, producing colour bytes `C'`; the stored pixel is
then `canonicalise(C', A_old)`. So: the alpha byte never changes; a pixel with `A_old == 0` stays
`(0,0,0,0)`; colour changes are not weakened by the old alpha. The eraser only removes alpha, so an
eraser stroke on a locked layer is a no-op in v0.1. Merge Down, Apply Mask and Flatten ignore the
lock.

---

## 11. Render-script ops owned by this doc

Script shape (fixed): `{"canvas":{"w":int,"h":int,"bg":"#RRGGBBAA"}, "ops":[...], "out":"png8"}`.

Common rules:

- `canvas.w`, `canvas.h`: integers in `1..16384`. `canvas.bg`: colour string, default
  `"#00000000"`.
- Colour strings: `"#RRGGBB"` (alpha FF) or `"#RRGGBBAA"`, hex digits case-insensitive, straight
  alpha.
- Grey values (masks): JSON integers `0..255`.
- Doubles (`opacity`, `fill`): JSON numbers in `[0.0, 1.0]`, parsed to the nearest binary64
  (correctly rounded, as Python `float()` and glibc `strtod` do). Out of range is a script error,
  never clamped.
- Seeds: JSON integers in `[0, 2^53)`.
- Rects: `[x, y, w, h]` integers, `w, h >= 1`; may extend past the canvas (only on-canvas pixels
  exist; formulas still use the unclipped rect). Default: the whole canvas `[0, 0, W, H]`.
- Ids: non-empty strings, unique across all nodes; `"root"` is reserved and names the root
  container. Referencing an unknown id, or an op on the wrong node kind, is a script error: the CLI
  and `refcomp.py` both exit non-zero and write no PNG.
- An unknown field is a script error (so typos cannot silently fall back to defaults).
- `"out": "png8"` writes `RENDER(root.children, BG)` as 8-bit RGBA PNG, every pixel canonical.

### 11.1 Content fills (shared by `add_layer` and `add_mask`)

For a pixel `(x, y)` inside `rect = [rx, ry, rw, rh]`:

- **solid**: the given colour / grey.
- **gradient**, `dir = "h"`: `t = 0.0 if rw == 1 else (x - rx) / (rw - 1)` (the double quotient of
  two integers; endpoints land exactly on the first and last pixel centres of the rect).
  `dir = "v"`: same with `y, ry, rh`. Per channel `k` (R, G, B, A for colours; the single grey for
  masks), with endpoint bytes `F_k`, `T_k`:
  `v_k = n(F_k) + (t * (n(T_k) - n(F_k)))`, byte `= q(v_k)`. Colour gradients interpolate
  **straight** RGBA per channel (not premultiplied). Then canonicalise.
- **checker**: `cx = (x - rx) // cell`, `cy = (y - ry) // cell` (non-negative integer division);
  colour `a` if `(cx + cy)` is even, else `b`. Then canonicalise.
- **noise** (layers): `h_k = pixel_hash(seed, x, y, k)` for `k = 0, 1, 2` (R, G, B) and, if
  `alpha == "random"`, `k = 3` for A; each byte is `h_k >> 56` (the top 8 bits; identical to
  `floor(unit(h_k) * 256)`). If `alpha` is an integer, A is that byte. Then canonicalise.
- **noise** (masks): byte `= pixel_hash(seed, x, y, 4) >> 56`.

Outside the rect: layers are `(0,0,0,0)`; masks are `outside`.

### 11.2 Op table

| op | fields (type, default, range) | semantics |
|---|---|---|
| `add_layer` | `id` str (required); `parent` str, default `"root"` (a group id or `"root"`); `fill` `"empty"`\|`"solid"`\|`"gradient"`\|`"checker"`\|`"noise"`, default `"empty"`; `color` colour, default `"#000000FF"` (solid); `from`, `to` colours, defaults `"#000000FF"`, `"#FFFFFFFF"` (gradient); `dir` `"h"`\|`"v"`, default `"h"`; `a`, `b` colours, defaults `"#FFFFFFFF"`, `"#CCCCCCFF"`, `cell` int `1..4096` default 8 (checker); `seed` int default 0 (the noise seed, not the Dissolve seed), `alpha` `"random"` or int `0..255`, default `"random"` (noise); `rect` rect, default whole canvas (allowed with every fill) | Creates a raster layer with default properties (§1, Dissolve seed 0) at the **top** of `parent`'s child list, pixels per §11.1. Fill-specific fields that do not belong to the chosen `fill` are errors. |
| `add_group` | `id` str; `parent` str default `"root"`; `mode` a §3.1 JSON mode, `"pass"` or `"isolated"`, default `"pass"` | Creates an empty group at the top of `parent`. |
| `move_layer` | `id` str; `parent` str, default the node's current parent; `index` int, default = top | Removes the node, then inserts it into `parent` at `index` (0 = bottom), counted after removal; `index` must be in `0..len`. Moving a group into itself or a descendant is an error. |
| `set_blend` | `layer` str; `mode` §3.1 string; `seed` int, optional | Sets the mode (`"pass"`/`"isolated"` only for groups). If `seed` is present, sets the node's Dissolve seed; if absent the seed is unchanged. |
| `set_opacity` | `layer` str; `value` double | Any node kind. |
| `set_fill` | `layer` str; `value` double | Raster and adjustment layers only (group: error). |
| `set_visible` | `layer` str; `value` bool | Any node kind. |
| `set_clip` | `layer` str; `value` bool | Raster and adjustment layers only (group: error). Allowed on any index; §5 decides the rendering. |
| `set_clbl` | `layer` str; `value` bool | Raster and adjustment layers only; only affects rendering when the node is a clip base. |
| `add_mask` | `layer` str; `fill` `"solid"`\|`"gradient"`\|`"noise"`, default `"solid"`; `value` int `0..255` default 255 (solid); `from`, `to` int, defaults 0, 255, `dir` default `"h"` (gradient); `seed` int default 0 (noise); `rect` rect, default whole canvas; `outside` int `0..255`, default 0 | Creates (or replaces) the node's mask, enabled. Any node kind. A "rect mask" is `fill:"solid"` with a `rect`. |
| `set_mask_enabled` | `layer` str; `value` bool | Error if no mask. |
| `delete_mask` | `layer` str | Error if no mask. |
| `apply_mask` | `layer` str | Raster only; error if no mask. §8. |
| `lock_transparency` | `layer` str; `value` bool, default true | Raster only. §10. |
| `merge_down` | `layer` str (the upper node `U`) | §9.1. |
| `merge_visible` | `id` str, default `"merged"` | §9.2. |
| `flatten` | `id` str, default `"flattened"` | §9.3. |

`add_adjustment` is owned by doc 20; it must create an adjustment node exactly like `add_layer`
(same `id`/`parent`/top-insertion rules, default properties per §1) and every `set_*`/mask op above
applies to it.

Example (the BUILD-SPEC example, valid under this grammar):

```json
{ "canvas": {"w":256,"h":256,"bg":"#00000000"},
  "ops": [ {"op":"add_layer","id":"bg","fill":"gradient","from":"#000000","to":"#ffffff","dir":"h"},
           {"op":"add_layer","id":"fg","fill":"gradient","from":"#ff0000","to":"#0000ff","dir":"v"},
           {"op":"set_blend","layer":"fg","mode":"sLit"},
           {"op":"set_opacity","layer":"fg","value":0.5},
           {"op":"flatten"} ],
  "out": "png8" }
```

---

## 12. Golden cases

Location: `tests/scripts/10-compositing/<name>.json`. Each is rendered by `rasterloom-cli` and by
`refcomp.py` and compared byte-exact. Totals: blend 108 + fill-vs-opacity 16 + groups 12 + clipping
8 + masks 6 = **150 counted**, plus 6 extras (blend ties, merge/flatten).

### 12.1 Blend modes: 27 modes × 4 alpha configurations = 108

Name `blend_<json>_<cfg>` (e.g. `blend_mul_opq`, `blend_div_bda`). Canvas `256×256`, bg
`"#00000000"`. The two gradient layers make the **R, G and B channels each sweep all 65 536
(backdrop byte, source byte) pairs** (`R: (x, y)`, `G: (255-x, y)`, `B: (255-x, 255-y)`), so every
guard and threshold of §3 is hit on exact bytes.

| cfg | ops (after which `set_blend s <mode>` with `"seed":7`) | alpha situation | what it catches |
|---|---|---|---|
| `opq` | `add_layer b` gradient h `#00FFFFFF`→`#FF0000FF`; `add_layer s` gradient v `#0000FFFF`→`#FFFF00FF` | `as = 1`, `ab = 1`: output = `B` itself | any wrong `B`, every guard (dodge/burn/divide/vivid at 0 and 255), Hard Mix diagonal, soft-light branch, luma weights |
| `op50` | `opq` + `set_opacity s 0.5` | `as = 0.5`, `ab = 1` | opacity lerp; many exact `.5` ties at `q` (rounding rule), Dissolve's 50 % pattern |
| `bda` | `opq` + `add_mask b` noise seed 11 (before `s` is added) | `as = 1`, `ab` random `0..255` per pixel | the `(1 - ab) * cs + ab * B` term, straight vs premultiplied storage, `ao` division |
| `rnd` | `add_layer b` noise seed 21 alpha random; `add_layer s` noise seed 22 alpha random; `set_opacity s 0.75` | both alphas random, including 0 | full random RGB for non-separable modes; `ao == 0` canonical pixels; everything at once |

Per mode, what its four goldens specifically catch (beyond the per-cfg column):

| mode | specific bug caught |
|---|---|
| `norm` | Normal lerp not short-circuited / SIMD path order differs from scalar |
| `diss` | wrong hash stream `k`, wrong coordinate (tile-local instead of canvas), `<` vs `<=` |
| `dark` | min/max swapped with `lite` |
| `mul` | premultiplied operands (mutation 2), linear-light blending (mutation 3) |
| `idiv` | burn guard order (`cb == 1` must win over `cs == 0`) |
| `lbrn` | missing `max(0, …)` clamp |
| `dkCl` | per-channel instead of whole-vector selection; float luma |
| `lite` | min/max swapped |
| `scrn` | `a + b - a*b` form instead of W3C form (last-bit differences at ties) |
| `div` | dodge guard order (mutation 16): `B(0, 1)` must be 0 |
| `lddg` | missing `min(1, …)` |
| `lgCl` | per-channel selection |
| `over` | overlay = hard-light with arguments not swapped |
| `sLit` | Pegtop (mutation 0); `D` thresholded on `cs` instead of `cb` |
| `hLit` | branch on `cb` instead of `cs` |
| `vLit` | vivid guards at `cs = 0` / `255`; `2(1 - cs)` vs `1 - (2cs - 1)` form |
| `lLit` | missing clamp; `2*cs` vs `cs + cs` order |
| `pLit` | branch mix-up |
| `hMix` | threshold off by one (mutation 17): `S + B >= 255` |
| `diff` | signed instead of absolute |
| `smud` | `cb + cs - 2*cb*cs` order |
| `fsub` | reversed operands (`cs - cb`) |
| `fdiv` | divide guard order (mutation 21): `B(0, 0)` must be 0; reversed operands |
| `hue` | SetSat tie index rule; Rec.709 luma (mutation 1) |
| `sat` | Sat of the wrong operand |
| `colr` | ClipColor branch order / guards |
| `lum` | SetLum operand swap with `colr`; Rec.709 luma (mutation 1) |

### 12.2 Fill versus opacity (16)

Canvas `128×128` unless stated. "Backdrop" means `add_layer k` gradient h `#2040C0FF`→`#F0C020FF`.
"Base" means `add_layer base` gradient v `#FF000000`→`#FF0000FF` (red, alpha ramp) with rect
`[16,16,96,96]`. "Clip" means `add_layer c` noise seed 5 alpha 255, then `set_clip c true`.

| name | setup | what it catches |
|---|---|---|
| `fo_lone_fill` | backdrop + layer `u` solid `#10E080FF` rect `[32,32,64,64]`, `set_fill u 0.5` | fill ignored on a lone layer |
| `fo_lone_opacity` | same with `set_opacity u 0.5` | baseline for the one above; product order `((a*m)*f)*o` |
| `fo_lone_both` | same with fill 0.6, opacity 0.7 | fill and opacity order swapped (last-bit) |
| `fo_lone_mul_fill` | same, `set_blend u mul`, fill 0.4 | fill applied after the blend instead of as coverage |
| `fo_lone_lddg_fill` | same, `set_blend u lddg`, fill 0.6 | "special eight" behaviour invented (v0.1 treats fill as coverage) |
| `fo_clip_fill0` | backdrop + base + clip, `set_fill base 0` | mutation 6 (clipped layer must show inside the base shape) |
| `fo_clip_opacity0` | same with `set_opacity base 0` | opacity must hide the whole clip group |
| `fo_clip_fill50` | same with `set_fill base 0.5` | `r` seeding: base colour at half, clipped layer at full |
| `fo_clip_opacity50` | same with `set_opacity base 0.5` | whole group at half |
| `fo_clip_fill0_mul` | `fo_clip_fill0` + `set_blend c mul` | clipped Multiply against a fill-0 base shows the clip colour (not multiplied with the base, not with the backdrop) |
| `fo_clip_fill30_op70` | backdrop + base + clip + `add_layer c2` solid `#FFFFFF80`, `set_clip c2 true`, `set_blend c scrn`, `set_fill base 0.3`, `set_opacity base 0.7` | mutations 5 and 6 together on a partial-alpha base |
| `fo_clip_fill0_clbl0` | `fo_clip_fill0_mul` + `set_clbl base false` | clbl=false: clipped Multiply acts on the backdrop inside the shape (mutation 20) |
| `fo_clip_basemul_fill50` | backdrop + base + clip, `set_blend base mul`, `set_fill base 0.5` | base mode applied to the group, fill still only seeds `r` |
| `fo_clipped_fill` | backdrop + base + clip, `set_fill c 0.5` | fill of a clipped (non-base) layer = its coverage |
| `fo_adjust_fill` | backdrop + `add_adjustment a` (invert, doc 20), `set_fill a 0.5` | fill on an adjustment layer; `ADJUST` alpha preservation |
| `fo_near_zero` | canvas bg `#00000000`, `add_layer u` solid `#FF8000FF`, `set_opacity u 0.001`, `add_layer v` gradient h `#00FF0000`→`#00FF0004` with `rect [64,0,64,128]` (v must leave the left half untouched: any later composite with `c == 0` over a non-canonical pixel returns `(0,0,0,0)` because `ao == 0`) | canonical transparent pixel when `q(ao) == 0` but `ao > 0` (mutation 22) |

### 12.3 Groups (12)

Canvas `128×128`. "Backdrop" as in 12.2. Children use `rect`s so they overlap partially.

| name | setup | what it catches |
|---|---|---|
| `grp_pass_plain` | backdrop + pass group with two normal children | pass-through machinery changes nothing when `w = 1` |
| `grp_pass_mul_child` | backdrop + pass group with one `mul` child | mutation 4 (isolated would multiply against transparency) |
| `grp_pass_opacity` | backdrop + pass group opacity 0.5 with two overlapping opaque children | mutation 19 (per-child opacity shows the lower child through the upper) |
| `grp_pass_mask` | `grp_pass_mul_child` + `add_mask` on the group, gradient v 0→255 | group mask as the lerp weight |
| `grp_pass_transparent_bd` | bg transparent, backdrop = noise seed 3 alpha random, pass group opacity 0.5 with a `scrn` child | premultiplied lerp (straight lerp bleeds canonical black) |
| `grp_iso_mul_child` | backdrop + group mode `isolated` with one `mul` child | isolated path: child multiplies onto transparency, group then Normal |
| `grp_iso_screen` | backdrop + group mode `scrn` opacity 0.6 with two children | group blended as one unit with its own mode and opacity |
| `grp_iso_mask_diss` | backdrop + group mode `diss` seed 9, opacity 0.7, mask noise seed 4 | group coverage `(n(A_T)*mg)*og`, Dissolve on groups |
| `grp_pass_adjust` | backdrop + pass group containing `add_adjustment` invert | adjustment inside pass-through affects the layer below the group (D4, mutation 4) |
| `grp_iso_adjust` | backdrop + isolated group containing a child and an invert adjustment above it | adjustment affects only the group's content; transparent stays transparent |
| `grp_nested` | pass group ⊃ isolated `mul` group ⊃ pass group ⊃ `over`… children, 3 deep, with opacities 0.8/0.6/0.5 | recursion, backdrop threading through nested pass-through |
| `grp_hidden_empty` | an empty pass group, an empty isolated group, a hidden group with visible children, a visible group whose only child is hidden, over the backdrop | hidden/empty handling (output must equal the backdrop alone) |

### 12.4 Clipping masks (8)

Canvas `128×128`, "backdrop", "base", "clip" as in 12.2.

| name | setup | what it catches |
|---|---|---|
| `clip_partial_base` | backdrop + base + clip | mutation 5; result alpha must not exceed `S` |
| `clip_base_mask` | `clip_partial_base` + `add_mask base` solid 255 rect `[16,16,48,96]` | mutation 23 (base mask is part of the shape) |
| `clip_clbl_on` | backdrop + base (`set_blend base scrn`) + clip (`set_blend c mul`) | clip group blended onto the base colour, then screened as a unit |
| `clip_clbl_off` | same + `set_clbl base false` | mutation 20 (clbl ignored) |
| `clip_base_opacity_two` | backdrop + base opacity 0.5 + two clipped layers (normal, `over` opacity 0.6) | base opacity applies once to the whole group |
| `clip_hidden` | backdrop + base + hidden clip + visible clip; then a second clip group whose base is hidden | hidden clipped layer skipped; hidden base hides its clipped layers |
| `clip_invalid_base` | a clipped layer at index 0; a group followed by a clipped layer | clip flag ignored without a raster base (rendered unclipped) |
| `clip_adjust` | backdrop + base + clipped invert adjustment | clipped adjustment acts only inside the base shape (reads `G`) |

### 12.5 Layer masks (6)

Canvas `128×128`, backdrop as in 12.2.

| name | setup | what it catches |
|---|---|---|
| `mask_gradient` | layer noise seed 8 alpha 255 + mask gradient h 0→255 | mask multiplies coverage; mask gradient formula |
| `mask_disabled` | same + `set_mask_enabled false` | disabled mask must be ignored exactly |
| `mask_deleted` | same + `delete_mask` | delete path |
| `mask_applied` | layer checker `#FF0000FF`/`#0000FF80` cell 8 + mask gradient v 255→0 + `apply_mask` + `set_opacity 0.8` | apply bakes `q(n(A)*n(M))` into alpha before later coverage |
| `mask_group_rect` | isolated group with two children + group mask solid 255 rect `[20,20,60,60]` outside 64 | group mask and `outside` value |
| `mask_order` | layer `over` noise seed 6 alpha random, fill 0.8, opacity 0.5, mask noise seed 12 | coverage product order `((n(A)*m)*f)*o` |

### 12.6 Extras (not counted in the BUILD-SPEC inventory)

| name | setup | what it catches |
|---|---|---|
| `blend_dkCl_tie` | canvas `16×16`, layer solid `#3B0000FF` (lum 1770), layer solid `#001E00FF` (lum 1770) in `dkCl` | tie keeps the backdrop (mutation 18) |
| `blend_lgCl_tie` | same in `lgCl` | tie keeps the backdrop (mutation 18) |
| `merge_down_plain` | backdrop + `u` (`mul`, opacity 0.5, mask) + `merge_down u` + `set_opacity k 0.8` | §9.1 plain path, L's properties kept |
| `merge_down_clip` | backdrop + base (fill 0.5) + clip, `merge_down c` | §9.1 clip path |
| `merge_visible_hidden` | bg `#808080FF`; three layers with the middle one hidden; `merge_visible` then `set_visible` on the hidden one true | merged onto transparent, hidden layer kept in place |
| `flatten_partial_bg` | bg `#FF000080`, one `scrn` layer, `flatten` | bg baked once and reset to transparent |

---

## 13. Mutation hooks

Each hook is injected in the C++ core only (`--selftest-mutate=N`); `refcomp.py` never mutates. Every
hook must turn at least one golden red.

| id | defect, precisely | caught by |
|---|---|---|
| 0 | `sLit` uses Pegtop: `B = ((1.0 - cb) * (cb * cs)) + (cb * Screen(cb, cs))` | `blend_sLit_opq` (and the other three `sLit`) |
| 1 | `Lum(C) = ((0.2126 * r) + (0.7152 * g)) + (0.0722 * b)` (Rec.709) in §3.4 | `blend_hue_opq`, `blend_colr_opq`, `blend_lum_opq`, `blend_sat_rnd` |
| 2 | §4.1 step 4 evaluates `B` on premultiplied operands: `bl = clamp01(B(ab * cb, as * cs))` | `blend_mul_bda`, `blend_*_rnd` (every non-Normal mode) |
| 3 | §4.1 in linear light: `cb`, `cs` pass through the sRGB decode (`c <= 0.04045 ? c/12.92 : ((c+0.055)/1.055)^2.4`) before steps 4–5 and `co` through the sRGB encode before `q` | `blend_norm_op50`, `blend_mul_opq` |
| 4 | a pass-through group is rendered as isolated `norm` (§7.2 with mode `norm`) | `grp_pass_mul_child`, `grp_pass_adjust` |
| 5 | inside `G` (§6.1 step 2) the clipped coverage is `ci * S` instead of `ci` (the base alpha multiplied in twice) | `clip_partial_base`, `fo_clip_fill30_op70` |
| 6 | fill of a clip base treated as opacity: `G` seeded with `q(1.0)` and `cg = (S * n(A_G)) * (o0 * f0)` | `fo_clip_fill0`, `fo_clip_fill50`, `fo_clip_fill0_mul` |
| 14 | `q` truncates: `floor(clamp01(x) * 255.0)` | every golden with fractional results; first: `blend_norm_op50` |
| 16 | `ColorDodge` checks `s == 1.0` before `cb == 0.0`, so `B(0, 1) = 1` | `blend_div_opq`, `blend_vLit_opq` |
| 17 | `hMix` threshold off by one: `(Sbyte + Bbyte) > 255` | `blend_hMix_opq`, `blend_hMix_op50` |
| 18 | `dkCl`/`lgCl` ties pick the source (`Ls <= Lb` / `Ls >= Lb`) | `blend_dkCl_tie`, `blend_lgCl_tie` |
| 19 | pass-through opacity/mask applied per child (each child's coverage `* w`) instead of the §7.1 lerp | `grp_pass_opacity`, `grp_pass_mask` |
| 20 | `clbl` ignored: `clbl = false` bases rendered through §6.1 | `clip_clbl_off`, `fo_clip_fill0_clbl0` |
| 21 | `fdiv` checks `cs == 0.0` before `cb == 0.0`, so `B(0, 0) = 1` | `blend_fdiv_opq` |
| 22 | canonicalisation skipped when `ao > 0` but `q(ao) == 0` (stores `(q(co), 0)`) | `fo_near_zero` |
| 23 | clip shape ignores the base mask: `S = n(A0)` | `clip_base_mask` |

Ids 7–13 and 15 belong to other docs (BUILD-SPEC seed list); ids 24+ are unallocated here.

---

## 14. Parity notes (feed `docs/PARITY.md`)

Every behaviour below is either an unknown relative to Photoshop or a deliberate divergence. None
was checked against Photoshop (BUILD-SPEC prohibitions); all come from the W3C text, the public PSD
specification, and open-source implementations as cited.

1. **Blend space and precision.** Blending is in gamma-encoded sRGB (D3) with per-layer 8-bit
   quantisation (C5). Photoshop's internal precision and rounding are unknown; last-bit differences
   against Photoshop are expected everywhere.
2. **Soft Light** is the W3C/ISO 32000 formula (D3). Photoshop's own soft-light formula is not
   publicly specified; it is commonly reported to differ from W3C. Divergence possible.
3. **The eleven non-W3C modes** follow Krita/GIMP as cited; Photoshop's exact formulas, guards and
   rounding are not public. Unknown.
4. **Hard Mix** uses `S + B >= 255` (GIMP). Krita uses `S + B > 255` (in 8-bit terms `>= 256`). The two
   open-source references disagree on the diagonal; Photoshop's rule is unknown. Divergence on one
   diagonal possible.
5. **Divide/Color Dodge/Color Burn/Vivid Light at 0 and 1** use the W3C/Krita guards (`B(0,0) = 0`
   for Divide and Dodge). Photoshop's values at these points are unknown.
6. **Darker/Lighter Color** use luma 0.30/0.59/0.11 (as integers ×100) with ties keeping the
   backdrop. Photoshop's luma weights and tie rule for these modes are unknown.
7. **Non-separable modes** add `d1 > 0` / `d2 > 0` guards and a first-index tie rule absent from W3C.
   Only degenerate inputs are affected.
8. **Dissolve** is a deterministic hash pattern (C6); it will never match Photoshop's pattern.
   Divergence by design.
9. **Fill opacity and the "special eight" modes.** Photoshop is widely reported to treat Fill
   differently from Opacity for Color Burn, Linear Burn, Color Dodge, Linear Dodge, Linear Light,
   Vivid Light, Hard Mix and Difference, but the math is not in any public specification we may
   use. v0.1 treats fill as plain coverage in every mode; fill and opacity differ only on clip
   bases (§6). **Known divergence.** The PSD `iOpa` fill key is also not described in the public
   PSD spec (doc 40 handles the round trip).
10. **Clip groups.** The relative-coverage model of §6.1 (`G` alpha = coverage within the base
    shape) and the `clbl = false` rule (clipped coverage `× S × o0`, not `× f0`) are Rasterloom
    definitions consistent with D4; Photoshop's exact math is unknown. A clipped adjustment over a
    fill-0 base has no effect here (it recolours `G`, which is transparent); Photoshop's behaviour
    is unknown.
11. **Clip base kinds.** Groups and adjustment layers cannot be clip bases, and groups cannot be
    clipped, in v0.1; such layers render unclipped. Photoshop allows both. **Divergence.**
12. **Groups have no Fill** in v0.1. Unknown relative to Photoshop.
13. **Pass-through opacity/mask** is one premultiplied lerp against the pre-group backdrop (as
    psd-tools implements). Exact Photoshop math unknown.
14. **Merge Down** bakes the lower layer's mask and fill and keeps its opacity and mode; **Merge
    Visible** renders onto transparency (not the bg); **Flatten** bakes the bg and resets it to
    transparent rather than producing an opaque "Background" layer filled with white. Photoshop's
    exact merge semantics with non-Normal lower layers are unknown. **Divergence** for Flatten.
15. **Lock transparency**: the eraser on a locked layer is a no-op in v0.1 (Photoshop paints the
    background colour). **Divergence.**
16. **Not implemented** (v0.2): Knockout (`knko`), Blend If ranges, `tsly`, `lmgm`, `infx`, layer
    styles, vector masks, mask density/feather, mask "invert when blending" flag.
17. **Script opacity/fill are doubles**; PSD stores opacity and fill as bytes. How a byte maps to a
    double on import is doc 40's decision.

---

## 15. Implementation notes for the two authors (non-normative, but binding where they restate)

- Keep every intermediate buffer as bytes; never carry doubles across layers (C5).
- Vectorise `refcomp.py` per layer over whole canvases; nothing in this file needs a
  transcendental (`sqrt` is correctly rounded), so plain NumPy float64 array arithmetic is allowed
  here, evaluated in the stated order (one NumPy expression per stated operation, no `np.fma`, no
  `np.power`, no `np.round`).
- `pixel_hash` in NumPy: build it from `uint64` arrays; NumPy `uint64` arithmetic wraps modulo 2^64,
  which is what C6 needs (mask only where Python ints are used).
- The C++ `q` is `static_cast<uint8_t>(std::round(clamp01(x) * 255.0))`; the reference is
  `f = np.floor(y); f + ((y - f) >= 0.5)` with `y = np.clip(x, 0, 1) * 255.0` (C2).
