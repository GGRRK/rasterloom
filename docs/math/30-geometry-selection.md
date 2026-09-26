# Rasterloom normative math — 30: geometry, selections, transform, gradient, paint bucket

Status: normative for v0.1. Inherits every rule of `00-conventions.md` (C1–C8). Where this file
says "q", "clamp", "decode", "canonical transparent" or "row-major" it means exactly the
definitions in C1–C4.

Scope: selection masks (marquee, lasso/polygon, magic wand, boolean modes, Select All / Deselect /
Reselect / Inverse, Feather, Expand, Contract) and how every other op consumes "the selection";
free transform through a 3×3 homography; the Image menu (Image Size, Canvas Size, Crop, Rotate
Canvas, Flip); the Gradient tool (linear, radial); the Paint Bucket; plus `fill_selection`, the op
the goldens use to make a selection visible.

Nothing in this document was derived from Adobe Photoshop or its outputs. Every adapted formula
names its open source next to it.

---

## 1. Shared definitions

### 1.1 Decoding and colours

- `dec(v) = v / 255.0` for a byte `v` (C2).
- A script colour `"#RRGGBB"` or `"#RRGGBBAA"` (case-insensitive hex) parses to bytes
  `(R, G, B, A)`, where `A = 255` when the alpha pair is absent. Its doubles are
  `cr = dec(R)`, `cg = dec(G)`, `cb = dec(B)`, `ca = dec(A)`. Script colours are **not**
  canonicalised: `"#ff000000"` keeps `R = 255` for interpolation purposes (§11).
- A stored pixel is 4 bytes, straight RGBA. After every write, a pixel whose alpha byte is 0 is
  stored as `(0, 0, 0, 0)` (C3).

### 1.2 `paint_over`: writing a colour into a layer's pixels

Every op in this file that deposits colour into an existing raster layer (`fill_selection`,
`gradient`, `bucket_fill`) uses this one Normal-mode, straight-alpha "source over" on the layer's
own pixels. It is not the compositor (doc 10 owns that); it edits one layer's stored bytes.

Inputs per pixel: source straight colour `(sr, sg, sb)` (doubles in `[0,1]`), effective source
alpha `as` (double in `[0,1]`, already multiplied by every coverage/opacity term the calling op
lists, in the order that op states), destination bytes `(Rb, Gb, Bb, Ab)`.

```
if as == 0.0:                       # exact zero: pixel untouched, bytes identical
    return
ab = dec(Ab)
if layer.lock_transparency:         # doc 10 flag; alpha is preserved
    if Ab == 0: return              # nothing to recolour
    for each channel k in (r, g, b):
        ck = (dec(Kb) * (1.0 - as)) + (sk * as)
        store q(ck)
    alpha byte unchanged
else:
    ao = as + (ab * (1.0 - as))     # ao > 0 because as > 0
    for each channel k in (r, g, b):
        ck = ((sk * as) + ((dec(Kb) * ab) * (1.0 - as))) / ao
        store q(ck)
    store q(ao)
apply canonical transparent (C3)
```

The channel formula is W3C Compositing-1 §5.1 "simple alpha compositing"
(`co = cs αs + cb αb (1 − αs)`, `αo = αs + αb (1 − αs)`), divided by `αo` to return to straight
colour: <https://www.w3.org/TR/compositing-1/#simplealphacompositing>.

### 1.3 Sub-pixel sample pattern (all anti-aliased rasterisation)

Anti-aliased shape coverage is **fixed 16 × 16 supersampling on a regular grid**. For pixel
`(x, y)` the 256 sample points are

```
o(i) = (2*i + 1) / 32.0             # i = 0..15; exact dyadic doubles 1/32, 3/32, ..., 31/32
sample(i, j) = (x + o(i), y + o(j))  # exact: integer + dyadic, |x| < 2^16
```

`n` = number of samples for which the shape's inside test (§3) is true, `0 <= n <= 256`.
Coverage byte: `S = q(n / 256.0)`. (`(n / 256.0) * 255.0` is exact, so this equals the integer
`(255*n + 128) // 256`; either form is acceptable because they are identical.)

Non-anti-aliased coverage uses the single sample at the pixel centre `(x + 0.5, y + 0.5)`:
`S = 255` if inside, else `0`.

An implementation may skip pixels outside the shape's bounding box (they have `n = 0`) or
otherwise shortcut, **only** if it provably yields the identical `n` for every pixel. Pixels are
rasterised only inside the canvas `[0, W) × [0, H)`; the shape may extend past the canvas.

---

## 2. Selection state model

The document holds:

- `S`: a `W × H` array of bytes, the **selection coverage** (BUILD-SPEC req 4). `S = 0` means
  unselected, `255` fully selected, values between are partial (soft edges).
- `Saved`: either *absent* or a `W × H` byte array, used by Reselect.

At document creation `S` is all 0 and `Saved` is absent.

**Empty means none.** A selection whose every byte is 0 is *the empty selection*, and the empty
selection means **"no selection"**: ops that honour the selection then act on the whole canvas.
There is no separate "active" flag. (This is GIMP's model, where an empty selection channel is
"no selection"; it makes every state representable as one byte array and removes a flag both
implementations could disagree on.)

**Effective coverage** consumed by all painting/filtering ops (§9):

```
E(x, y) = 255        if S is empty
E(x, y) = S(x, y)    otherwise
e(x, y) = dec(E(x, y))          # 1.0 exactly when E = 255
```

**Canvas-geometry ops clear the selection.** `image_size`, `canvas_size`, `crop`,
`rotate_canvas` and `flip` *without* a `layer` field set `S` to empty (new size) and `Saved` to
absent. `transform` and `flip` *with* a `layer` field leave `S` and `Saved` untouched.

---

## 3. Shape inside-tests

All coordinates are canvas doubles (C4: pixel `(x, y)` covers `[x, x+1) × [y, y+1)`).

### 3.1 Rectangle `(x, y, w, h)`, `w > 0`, `h > 0`

```
x1 = x + w
y1 = y + h
inside(px, py) = (x <= px) and (px < x1) and (y <= py) and (py < y1)
```

Half-open on both axes. With integer `x, y, w, h`, AA and non-AA give identical results: exactly
the pixels `[x, x+w) × [y, y+h)`.

### 3.2 Ellipse inscribed in the box `(x, y, w, h)`, `w > 0`, `h > 0`

```
rx = w / 2.0
ry = h / 2.0
cx = x + rx
cy = y + ry
inside(px, py):
    dx = (px - cx) / rx
    dy = (py - cy) / ry
    return ((dx * dx) + (dy * dy)) <= 1.0
```

### 3.3 Polygon (polygonal and freehand lasso)

A lasso, freehand or polygonal, is a closed polygon given as its vertex list
`P[0..n-1] = (xk, yk)`, `n >= 3`; the closing edge `P[n-1] → P[0]` is implicit. Freehand lasso in
the GUI records the pointer positions as vertices; there is no smoothing.

**Fill rule: even-odd.** Inside test is W. R. Franklin's PNPOLY crossing test, evaluated exactly
as below (source: <https://wrfranklin.org/Research/Short_Notes/pnpoly.html>; even-odd matches
GIMP's lasso scan conversion, `app/core/gimpscanconvert.c` `cairo_set_fill_rule (cr,
CAIRO_FILL_RULE_EVEN_ODD)`,
<https://github.com/GNOME/gimp/blob/gimp-2-10/app/core/gimpscanconvert.c>):

```
inside(px, py):
    c = false
    for k in 0..n-1:                       # ascending
        (xi, yi) = P[k]
        (xj, yj) = P[(k + n - 1) mod n]    # previous vertex
        if (yi > py) != (yj > py):
            xc = (((xj - xi) * (py - yi)) / (yj - yi)) + xi
            if px < xc:
                c = not c
    return c
```

Properties that follow and that goldens rely on: horizontal edges never count; a sample exactly
on a vertex row counts the edge whose other end is strictly above it (half-open in `y`); a sample
exactly on an edge (`px == xc`) is outside for that edge. Fewer than 3 vertices, or a polygon of
zero area, rasterises to all 0.

---

## 4. Boolean combine modes

Every shape op and `select_wand` produces a candidate byte array `B` (full canvas; 0 outside the
shape), then combines it with the current `S` by `mode`. Integer arithmetic on bytes, so exact:

| mode | result `S'(x, y)` | note |
|---|---|---|
| `new` | `B` | replaces |
| `add` | `max(S, B)` | union |
| `subtract` | `max(S - B, 0)` | clamped difference |
| `intersect` | `min(S, B)` | |

`S` here is the stored array (an empty selection is all zeros, so `add` onto empty = `B`,
`intersect` with empty = empty, `subtract` from empty = empty).

Justification and provenance: `subtract` and `intersect` are GIMP's mask-combine arithmetic
(`MAX (*p - value, 0.0)`, `MIN (*mask, *add_on)` in `app/gegl/gimp-gegl-mask-combine.cc`,
<https://github.com/GNOME/gimp/blob/gimp-2-10/app/gegl/gimp-gegl-mask-combine.cc>). For `add`, GIMP
uses a clamped sum; Rasterloom deliberately uses `max`, because a clamped sum makes adding the
same anti-aliased shape twice turn every 128 edge into 255 (a hard, aliased edge), while `max` is
idempotent: `add(A, A) = A`, `intersect(A, A) = A`, `subtract(A, A) = empty`. The clamped
difference (rather than the fuzzy-logic `min(S, 255 - B)`) is chosen for the last identity:
subtracting a shape from itself leaves no ghost ring at its soft edge.

---

## 5. Select All, Deselect, Reselect, Inverse

```
select_all:      S = 255 everywhere                    (Saved unchanged)
deselect:        if S is not empty: Saved = copy(S)
                 S = all 0
reselect:        if Saved is present: S = copy(Saved)  (Saved kept; else no-op)
select_inverse:  S(x, y) = 255 - S(x, y) for every pixel
```

`select_inverse` of the empty selection is all 255 (selecting everything, which paints the same as
"no selection"). No other op writes `Saved`.

---

## 6. Region matching: magic wand and paint bucket

`select_wand` and `bucket_fill` share one function, `region`, so mutation 15 hits both.

### 6.1 Sampling source

The region is computed from the **stored bytes of the named raster layer only**: its own
pixels, ignoring its visibility, opacity, fill, blend mode, layer mask and every other layer.
(No "sample all layers" in v0.1; §22.) The layer must be a raster layer.

### 6.2 Colour distance

Seed `(sx, sy)` is an integer pixel. Seed bytes `(Rs, Gs, Bs, As)` = the stored pixel there
(canonical, so a transparent seed is `(0,0,0,0)`). For every pixel with bytes `(R, G, B, A)`:

```
d(x, y) = max(|R - Rs|, |G - Gs|, |B - Bs|, |A - As|)       # integers 0..255
match(x, y) = d(x, y) <= t                                  # t = tolerance, integer 0..255
```

Max-of-channel-differences is GIMP's `GIMP_SELECT_CRITERION_COMPOSITE` metric
(`pixel_difference` in `app/core/gimppickable-contiguous-region.cc`,
<https://github.com/GNOME/gimp/blob/gimp-2-10/app/core/gimppickable-contiguous-region.cc>),
here on bytes and including the alpha channel as a fourth component (GIMP drops alpha in that
branch). Both `d` and `t` are on the 0–255 byte scale; nothing is normalised.

### 6.3 Region

```
region(L, sx, sy, t, contiguous, aa) -> byte array Rg[H][W]:
    if (sx, sy) is outside the canvas: return all 0
    M = { (x, y) in canvas : match(x, y) }             # the seed is always in M (d = 0)
    if contiguous:
        C = the 8-connected component of M containing (sx, sy)
            # neighbours of (x, y): the 8 pixels (x+dx, y+dy), dx, dy in {-1, 0, 1}, not both 0,
            # that lie inside the canvas
    else:
        C = M
    Rg(x, y) = 255 for (x, y) in C
    for every (x, y) not in C:
        if aa and t > 0 and at least one of its 8 in-canvas neighbours is in C:
            a = 1.5 - (d(x, y) / t)                    # both converted to double first
            Rg(x, y) = 0 if a <= 0.0 else q(a * 2.0)
        else:
            Rg(x, y) = 0
```

The component `C` is a set; it does not depend on visiting order, so implementations may flood in
any order (row-major scanline fill is suggested per C4). A fringe pixel always has `d > t`
(otherwise it would be 8-adjacent to `C` and in `M`, hence in `C`), so `a < 0.5` and the fringe
value is in `[0, 255)`. The fringe formula `aa = 1.5 − max / threshold`, `aa * 2` below 0.5, is
GIMP's anti-alias rule from the same `pixel_difference`, applied here only to the 1-pixel ring
around the region (GIMP applies it to every pixel it floods through).

### 6.4 `select_wand`

`B = region(layer, x, y, tolerance, contiguous, antialias)`, then combine with `mode` (§4).

---

## 7. Feather

Feather softens the whole current selection by an almost-Gaussian blur defined here in full, in
exact integer arithmetic. It does not depend on doc 20.

```
feather(r):                                   # r: double, 0 <= r <= 250
    if r == 0.0 or S is empty: no-op
    sigma = r / 2.0
    (w1, w2, w3) = box_widths(sigma)          # odd integers >= 1
    T = S as 64-bit integers
    for w in (w1, w2, w3):                    # in this order
        h = (w - 1) / 2                       # exact integer
        T(x, y) = sum over k = -h..h of T(clampx(x + k), y)     # horizontal box sum
        T(x, y) = sum over k = -h..h of T(x, clampy(y + k))     # vertical box sum
    D = (w1 * w2 * w3) * (w1 * w2 * w3)
    S(x, y) = (2 * T(x, y) + D) // (2 * D)    # round half up; every term is non-negative
    (if the result is all 0, S is empty)

clampx(i) = min(max(i, 0), W - 1)            # edge replicate
clampy(j) = min(max(j, 0), H - 1)
```

Nothing is divided until the end, so the passes commute and their order cannot change the
result; the sums are exact integers. Bound: `w <= 251` gives `D < 2.5e14` and `2T + D < 1.3e17`,
inside int64.

Edge replicate means a selection touching the canvas border stays selected at the border
(`select_all` + `feather` = `select_all`).

`box_widths(sigma)`, the three-box approximation of a Gaussian (P. Kovesi, "Fast Almost-Gaussian
Filtering", DICTA 2010; `boxesForGauss` in I. Kutskir, "Fastest Gaussian Blur",
<https://blog.ivank.net/fastest-gaussian-blur.html>), with `n = 3` and every step fixed:

```
s12    = (12.0 * sigma) * sigma
wIdeal = sqrt((s12 / 3.0) + 1.0)
wl     = floor(wIdeal)                       # integer
if wl is even: wl = wl - 1
wu     = wl + 2
mIdeal = (((s12 - (3 * wl * wl)) - (12 * wl)) - 9) / ((-4 * wl) - 4)
             # integer sub-expressions exact, then converted to double
m      = floor(mIdeal + 0.5)                 # NOT banker's rounding
m      = min(max(m, 0), 3)
widths = [wl if i < m else wu  for i in 0, 1, 2]
```

Check values: `r = 2 → [1, 1, 3]`, `r = 4 → [3, 3, 5]`, `r = 8 → [7, 7, 9]`,
`r = 10 → [9, 9, 11]`. (`mIdeal` is exactly 1.5 for many radii, which is why the rounding form
is fixed.)

**Radius units.** `sigma = r / 2`: 95 % of the transition lies within `±r` of the original edge.
GIMP uses `sigma = r / 3.5` ("3.5 is completely magic", `gimp_gegl_apply_feather` in
`app/gegl/gimp-gegl-apply-operation.c`). The Photoshop relation is unknown (§22).

---

## 8. Expand and Contract

Grey-scale dilation/erosion by a Euclidean disc of integer radius `N` (circular structuring
element, as in GIMP's grow/shrink and Krita's `KisSelectionFilter::computeBorder`,
<https://github.com/KDE/krita/blob/master/libs/image/kis_selection_filters.cpp>):

```
disc(N) = { (dx, dy) integers : dx*dx + dy*dy <= N*N }        # exact integer test

expand(N):   S'(x, y) = max over (dx, dy) in disc(N) of Sx(x + dx, y + dy)
             where Sx(i, j) = S(i, j) inside the canvas, 0 outside
contract(N): S'(x, y) = min over (dx, dy) in disc(N) of Sc(x + dx, y + dy)
             where Sc(i, j) = S(i, j) inside the canvas, 255 outside
```

`N` is an integer, `1 <= N <= 100`. Empty selection: no-op. Contract treats outside-canvas as
selected, so a selection touching the canvas edge does not shrink away from it
(`select_all` + `contract` = `select_all`). Max/min are exact, so any decomposition gives the
same bytes; a fast one is per-row spans: for each `dy` in `-N..N`,
`half = isqrt(N*N - dy*dy)` (exact integer square root) and a 1-D running max/min of half-width
`half` on row `y + dy`.

---

## 9. How every other op consumes the selection

This is the contract for docs 10/20/40 and for §10, §17, §18 here. Every op that changes the
**pixels of an existing layer** honours the effective coverage `E` (§2) in exactly one of two
forms. An op's doc must say which form it uses. Ops that create layers, change layer properties,
or change canvas geometry ignore the selection.

**Coverage form** (ops that already compute a per-pixel source alpha: brush dabs, fill, gradient,
bucket). Multiply the op's per-pixel source alpha by `e` **as the last factor**, then write with
`paint_over` (§1.2) or the op's own write rule:

```
as_final = as_op * e(x, y)
```

**Replace form** (ops that compute a whole new pixel `N` from the old pixel `O`: filters and
destructive adjustments). Mix old and new by `E` in premultiplied space:

```
if E == 255: store N                  # bytes as computed by the op
elif E == 0: store O                  # unchanged
else:
    m  = dec(E)
    ao = (dec(Oa) * (1.0 - m)) + (dec(Na) * m)
    if ao == 0.0: store (0, 0, 0, 0)
    else for k in (r, g, b):
        ck = (((dec(Ok) * dec(Oa)) * (1.0 - m)) + ((dec(Nk) * dec(Na)) * m)) / ao
        store q(ck)
    store q(ao), then canonical transparent
```

The two exact branches make "no selection" (E = 255 everywhere) byte-identical to the unmasked
op, and an unselected pixel byte-identical to its input.

---

## 10. `fill_selection`

Fills the named raster layer with a colour through the selection (Edit → Fill). With no
selection it fills the whole layer. For each pixel:

```
(sr, sg, sb, ca) = parsed colour
as = (ca * opacity) * e(x, y)
paint_over(sr, sg, sb, as)
```

On a transparent layer with an opaque colour and opacity 1, the stored alpha byte equals
`E(x, y)` exactly (`ao = as`, `q(dec(E)) = E`), which is how the goldens show a selection.

---

## 11. Resampling kernel and premultiplied mixing

### 11.1 Kernel

Keys cubic convolution with `a = -0.5` (R. G. Keys, "Cubic convolution interpolation for digital
image processing", IEEE Trans. ASSP 29(6), 1981, doi:10.1109/TASSP.1981.1163711), evaluated in
the exact form used by Pillow's `bicubic_filter`
(<https://github.com/python-pillow/Pillow/blob/main/src/libImaging/Resample.c>):

```
K(d):
    x = abs(d)
    if x < 1.0: return (((1.5 * x) - 2.5) * x) * x + 1.0
    if x < 2.0: return ((((x - 5.0) * x) + 8.0) * x - 4.0) * -0.5
    return 0.0
```

(`1.5 = a + 2`, `2.5 = a + 3`, exact. Written fully parenthesised:
`((((1.5 * x) - 2.5) * x) * x) + 1.0` and `(((((x - 5.0) * x) + 8.0) * x) - 4.0) * (-0.5)`.)
`K(0) = 1`, `K(±1) = K(±2) = 0` exactly.

### 11.2 Premultiplied tap values

Per D2 and C3, every tap is premultiplied before mixing:

```
pa = dec(A)
pk = dec(K) * pa          for k in (r, g, b)
```

Out-of-source taps (where a path says "transparent outside") contribute `pa = pk = 0`.

### 11.3 Finishing a mixed pixel

Given accumulated doubles `(accr, accg, accb, acca)`:

```
A' = clamp(acca, 0.0, 1.0)
if A' == 0.0: store (0, 0, 0, 0); done
for k in (r, g, b):
    P' = clamp(acck, 0.0, A')          # overshoot clamp: premultiplied colour never exceeds alpha
    store q(P' / A')
store q(A'); canonical transparent if that byte is 0
```

Clamping happens **before** un-premultiplying, so Keys overshoot on a hard edge can never produce
a straight colour above 1 or a dark ring below 0.

### 11.4 Grey channels (layer masks)

A layer mask is a single straight channel with no alpha: taps are `dec(m)`, no premultiply, the
same weights and sum order, then `q(clamp(acc, 0.0, 1.0))`. Where a path says "transparent
outside", a mask tap outside the source is `1.0` (255, reveal).

---

## 12. Transform (free transform: move / scale / rotate / skew / distort / perspective)

Transforms one raster layer's pixels by a forward homography `M` (source canvas coordinates →
destination canvas coordinates). The layer's mask and the selection are not changed. Content
mapped outside the canvas is discarded (layers are canvas-bounded in v0.1). No resample
prefilter: this path always uses a 4 × 4 footprint (§22).

### 12.1 Matrix conventions

`M = [m0 m1 m2; m3 m4 m5; m6 m7 m8]`, row-major, acting on column vectors:

```
X' = (m0*X + m1*Y) + m2 ,  Y' = (m3*X + m4*Y) + m5 ,  W' = (m6*X + m7*Y) + m8
dest = (X' / W', Y' / W')
```

Product `C = A · B` (3 × 3):
`C[i][j] = ((A[i][0] * B[0][j]) + (A[i][1] * B[1][j])) + (A[i][2] * B[2][j])`.

Primitive matrices (degrees converted per C7: `rad = deg * (3.141592653589793 / 180.0)`, the
division evaluated first; `cos`, `sin`, `tan` are libm scalars, C1):

```
Tr(tx, ty)  = [1 0 tx; 0 1 ty; 0 0 1]
Sc(sx, sy)  = [sx 0 0; 0 sy 0; 0 0 1]
Ro(deg)     = [c -s 0; s c 0; 0 0 1]      c = cos(rad), s = sin(rad)
Sk(kx, ky)  = [1 hx 0; hy 1 0; 0 0 1]     hx = tan(rad(kx)), hy = tan(rad(ky))
```

With `y` pointing down, `Ro(+θ)` turns content **clockwise** on screen.

### 12.2 The three script forms

Exactly one form per op:

1. **Matrix**: `"matrix": [m0..m8]` used as given.
2. **Params** (any of `translate`, `scale`, `rotate`, `skew`, `pivot`; missing ones take their
   defaults): pivot `(px, py)`, then, left-multiplying in this order,
   ```
   M = Tr(-px, -py)
   M = Sc(sx, sy) · M
   M = Sk(kx, ky) · M
   M = Ro(deg)    · M
   M = Tr(px + tx, py + ty) · M          # px + tx, py + ty: one double add each
   ```
   i.e. scale, then skew, then rotate, all about the pivot, then translate.
3. **Quad** (distort / perspective): `"rect": [rx, ry, rw, rh]` (source rectangle, default the
   whole canvas) and `"quad": [[x0,y0],[x1,y1],[x2,y2],[x3,y3]]`, the destination positions of the
   rectangle's corners in the order top-left, top-right, bottom-right, bottom-left. Square-to-quad
   mapping after P. Heckbert, "Fundamentals of Texture Mapping and Image Warping", UCB/CSD 89/516,
   1989, §2.2.3 (<https://www2.eecs.berkeley.edu/Pubs/TechRpts/1989/CSD-89-516.pdf>), for the unit
   square `(0,0),(1,0),(1,1),(0,1) → p0, p1, p2, p3`:
   ```
   sx = ((x0 - x1) + x2) - x3
   sy = ((y0 - y1) + y2) - y3
   if sx == 0.0 and sy == 0.0:                     # parallelogram: affine
       g = 0.0 ; h = 0.0
       a = x1 - x0 ; b = x3 - x0 ; c = x0
       d = y1 - y0 ; e = y3 - y0 ; f = y0
   else:
       dx1 = x1 - x2 ; dx2 = x3 - x2 ; dy1 = y1 - y2 ; dy2 = y3 - y2
       den = (dx1 * dy2) - (dx2 * dy1)             # den == 0.0: invalid script
       g = ((sx * dy2) - (dx2 * sy)) / den
       h = ((dx1 * sy) - (sx * dy1)) / den
       a = (x1 - x0) + (g * x1) ; b = (x3 - x0) + (h * x3) ; c = x0
       d = (y1 - y0) + (g * y1) ; e = (y3 - y0) + (h * y3) ; f = y0
   Q = [a b c; d e f; g h 1.0]
   N = [1.0/rw  0  (-rx)/rw ; 0  1.0/rh  (-ry)/rh ; 0 0 1]   # rect → unit square
   M = Q · N
   ```

### 12.3 Inverse

Adjugate over determinant, exactly:

```
i0 = (m4*m8) - (m5*m7)   i1 = (m2*m7) - (m1*m8)   i2 = (m1*m5) - (m2*m4)
i3 = (m5*m6) - (m3*m8)   i4 = (m0*m8) - (m2*m6)   i5 = (m2*m3) - (m0*m5)
i6 = (m3*m7) - (m4*m6)   i7 = (m1*m6) - (m0*m7)   i8 = (m0*m4) - (m1*m3)
det = ((m0*i0) + (m1*i3)) + (m2*i6)
if not (abs(det) >= 1e-12): invalid script (also catches NaN)
n_k = i_k / det           for k = 0..8
```

For any affine `M` (`m6 = m7 = 0`, `m8 = 1`) this gives `n6 = n7 = ±0` and `n8 = 1.0` exactly,
so `w` below is exactly 1.

### 12.4 Per-pixel inverse mapping

For every destination pixel `(x, y)` of the layer, row-major:

```
X = x + 0.5 ; Y = y + 0.5                              # destination pixel centre
u = ((n0 * X) + (n1 * Y)) + n2
v = ((n3 * X) + (n4 * Y)) + n5
w = ((n6 * X) + (n7 * Y)) + n8
if not (w > 0.0): store (0, 0, 0, 0); continue          # behind the projection's horizon
sx = u / w ; sy = v / w                                 # continuous source position
if not (sx >= -4.0 and sx <= W + 4.0 and sy >= -4.0 and sy <= H + 4.0):
    store (0, 0, 0, 0); continue                        # also rejects NaN/inf
```

**Bicubic** (`interp = "bicubic"`, default):

```
fx = sx - 0.5 ; fy = sy - 0.5                           # source pixel i has its centre at i + 0.5
ix = floor(fx) ; iy = floor(fy)                         # integers
for kx in 0..3:  cxk = ix - 1 + kx ;  wxk = K(fx - cxk)  # cxk converted to double, one subtraction
for ky in 0..3:  cyk = iy - 1 + ky ;  wyk = K(fy - cyk)
for each channel ch in (r, g, b, a):
    out = 0.0
    for ky in 0..3:
        row = 0.0
        for kx in 0..3:
            row = row + (wxk * p_ch(cxk, cyk))          # premultiplied tap, §11.2
        out = out + (wyk * row)
    acc_ch = out
finish (§11.3)
```

Taps outside `[0, W) × [0, H)` are transparent (C4). Weights are not renormalised.

**Nearest** (`interp = "nearest"`): `ix = floor(sx)`, `iy = floor(sy)` (no `−0.5`: the pixel
whose half-open square contains the point); copy that pixel's bytes, or transparent if outside.

After the whole layer is computed, it replaces the layer's pixels.

---

## 13. Image Size

Resamples **every raster layer and every layer mask** of the document (hidden ones included) from
`W × H` to `W2 × H2`, clears the selection (§2). Canvas `bg` is a document colour and is unchanged.

`interp = "nearest"`: `new(x, y) = old(min(floor((x + 0.5) * (W / W2)), W - 1),
min(floor((y + 0.5) * (H / H2)), H - 1))`, with `W / W2` a double division of integers; bytes
copied.

`interp = "bicubic"` (default): separable convolution with a kernel widened for downscaling (area
prefilter), after Pillow's `precompute_coeffs` (same URL as §11.1). Per axis, from `Nin` to `Nout`:

```
scale  = Nin / Nout                       # double
fs     = max(scale, 1.0)                  # filterscale
supp   = 2.0 * fs
inv_fs = 1.0 / fs
for each output index o in 0..Nout-1:
    center = (o + 0.5) * scale
    lo = max(floor((center - supp) + 0.5), 0)
    hi = min(floor((center + supp) + 0.5), Nin)          # taps lo .. hi-1
    k_i = K(((i - center) + 0.5) * inv_fs)   for i = lo .. hi-1 ascending
    ww  = 0.0 ; ww = ww + k_i  (ascending)
    wt_i = k_i / ww  if ww != 0.0 else k_i
```

Taps never leave the source (no out-of-bounds reads; the renormalisation by `ww` keeps an opaque
image opaque to its edges).

Order: premultiply every pixel (§11.2) into doubles. If `W2 != W`, horizontal pass: for each
source row, each output column `o`: `acc = 0.0; acc = acc + (wt_i * p(i, row))` ascending `i`.
Then, if `H2 != H`, vertical pass on that double result, same form over rows. **No quantisation
between passes.** Then finish each pixel (§11.3). An axis whose size is unchanged is skipped
(copied); if both are unchanged the op is a no-op. Masks: §11.4 with the same weights.

---

## 14. Canvas Size and Crop

Both are exact index mappings (no resampling), applied to every raster layer and every layer mask,
and both clear the selection.

**Crop** to integer rectangle `(cx, cy, cw, ch)`, `cw, ch >= 1` (it may extend past the canvas):

```
new canvas: cw × ch
new(x, y) = old(x + cx, y + cy)  if that is inside the old canvas
          = (0, 0, 0, 0)         otherwise           (masks: 255)
```

**Canvas Size** to `W2 × H2` with `anchor`:

```
ox = 0                         for anchors tl, l, bl
ox = floor_div(W2 - W, 2)      for anchors t, c, b          # floor division, rounds toward -inf
ox = W2 - W                    for anchors tr, r, br
oy = 0                         for anchors tl, t, tr
oy = floor_div(H2 - H, 2)      for anchors l, c, r
oy = H2 - H                    for anchors bl, b, br
result = crop(-ox, -oy, W2, H2)
```

(`floor_div(-1, 2) = -1`; C++ must not use truncating `/` for negative operands.) New area is
transparent in every layer; the canvas `bg` shows through (§22).

---

## 15. Rotate Canvas

`angle` in degrees, clockwise positive. Normalise:
`a = angle - (360.0 * floor(angle / 360.0))`, so `0 <= a < 360`.

**Exact paths** (index mappings, every layer and mask, no resampling):

```
a == 0.0:    no-op (selection still cleared)
a == 90.0:   new size H × W ; new(x, y) = old(y, H - 1 - x)
a == 180.0:  same size      ; new(x, y) = old(W - 1 - x, H - 1 - y)
a == 270.0:  new size H × W ; new(x, y) = old(W - 1 - y, x)
```

(For 90: old top-left `(0, 0)` lands at new top-right `(H - 1, 0)`.)

**Arbitrary** `a`: the canvas grows to the bounding box of the rotated old canvas.

```
rad = a * (3.141592653589793 / 180.0)
c = cos(rad) ; s = sin(rad)                               # libm scalars
bw = abs(W * c) + abs(H * s)
bh = abs(W * s) + abs(H * c)
W2 = max(ceil(bw - 1e-6), 1) ; H2 = max(ceil(bh - 1e-6), 1)   # integers; > 16384: invalid
M  = Ro(a) · Tr(-(W / 2.0), -(H / 2.0))
M  = Tr(W2 / 2.0, H2 / 2.0) · M
```

Every raster layer is resampled through §12.3–12.4 (bicubic, outside transparent) into a
`W2 × H2` buffer; every mask through §11.4 with outside = 255. The `1e-6` keeps an angle whose
exact bounding box is an integer from growing by one pixel through a last-ulp error.

---

## 16. Flip

`axis = "h"`: `new(x, y) = old(W - 1 - x, y)`. `axis = "v"`: `new(x, y) = old(x, H - 1 - y)`.
Exact byte copies. Without `layer`: every raster layer and mask (Image → Flip Canvas), selection
cleared. With `layer`: that raster layer's pixels only (Edit → Transform → Flip, about the canvas
centre), mask and selection unchanged.

---

## 17. Gradient tool

Paints a two-colour gradient into a raster layer through the selection, at an opacity.

Parameters: `type` (`linear` | `radial`), start `p0 = (x0, y0)`, end `p1 = (x1, y1)`, colours
`c0`, `c1` (straight RGBA), `opacity`, `reverse`.

```
dx = x1 - x0 ; dy = y1 - y0
L2 = (dx * dx) + (dy * dy)
if L2 == 0.0: no-op                                     # zero-length drag paints nothing
radius = sqrt(L2)                                       # radial only
for each pixel (x, y), row-major:
    ex = (x + 0.5) - x0 ; ey = (y + 0.5) - y0            # pixel centre relative to p0
    linear: t = ((ex * dx) + (ey * dy)) / L2
    radial: t = sqrt((ex * ex) + (ey * ey)) / radius
    t = clamp(t, 0.0, 1.0)                               # pad beyond both ends
    if reverse: t = 1.0 - t
    for k in (r, g, b, a):
        gk = c0k + ((c1k - c0k) * t)                     # straight interpolation
    as = (ga * opacity) * e(x, y)
    paint_over(gr, gg, gb, as)
```

Interpolation is in **straight** colour, with colour and alpha as independent ramps. This matches
the common gradient-editor model of separate colour stops and opacity stops, and it is why "colour
to transparent" is written with both endpoints in the same RGB (`"#ff0000ff"` → `"#ff000000"`).
Using `"#00000000"` as the transparent end deliberately darkens the midpoint. No dithering (§22).

---

## 18. Paint Bucket

```
Rg = region(layer, x, y, tolerance, contiguous, antialias)      # §6, sampled from the target layer
for each pixel:
    as = ((ca * opacity) * dec(Rg(x, y))) * e(x, y)
    paint_over(cr, cg, cb, as)
```

The region is computed first, from the layer's bytes before any write. The flood is not limited
by the selection; the selection only masks the write. Seed outside the canvas: no-op.

---

## 19. Render-script ops

Render-script shape is fixed by the harness:
`{"canvas":{"w":int,"h":int,"bg":"#RRGGBBAA"}, "ops":[...], "out":"png8"}`.
Layers are referenced by the string id given to `add_layer`/`add_group` (doc 10). Any field
outside its range, an unknown field, a missing required field, or a reference to a non-raster
layer where a raster layer is required makes the script **invalid**: the renderer exits non-zero
and writes no PNG. Goldens only contain valid scripts. "num" = JSON number (double); "int" = JSON
integer. Coordinates are canvas pixels (C4). Common fields:

- `mode`: string, one of `"new"`, `"add"`, `"subtract"`, `"intersect"`; default `"new"`.
- `color`: `"#RRGGBB"` or `"#RRGGBBAA"`; required where listed.
- `opacity`: num in `[0, 1]`, default `1.0`.

| op | fields (type, default, range) |
|---|---|
| `select_rect` | `x` num req, `y` num req, `w` num req `(0, 65536]`, `h` num req `(0, 65536]`, `antialias` bool `false`, `mode` |
| `select_ellipse` | `x`, `y`, `w`, `h` as `select_rect` (bounding box), `antialias` bool `true`, `mode` |
| `select_polygon` | `points` array of `[num, num]` req, 3..4096 entries (freehand and polygonal lasso), `antialias` bool `true`, `mode` |
| `select_wand` | `layer` string req (raster), `x` int req, `y` int req, `tolerance` int `32` `[0, 255]`, `contiguous` bool `true`, `antialias` bool `true`, `mode` |
| `select_all` | none |
| `deselect` | none |
| `reselect` | none |
| `select_inverse` | none |
| `feather` | `radius` num req `[0, 250]` |
| `expand` | `by` int req `[1, 100]` |
| `contract` | `by` int req `[1, 100]` |
| `fill_selection` | `layer` string req (raster), `color` req, `opacity` |
| `transform` | `layer` string req (raster), `interp` `"bicubic"`\|`"nearest"` default `"bicubic"`; then exactly one form: **matrix** `matrix` array of 9 num; **params** `translate` `[num,num]` default `[0,0]`, `scale` `[num,num]` default `[1,1]` (each nonzero, abs `<= 1000`), `rotate` num deg default `0` `[-3600, 3600]`, `skew` `[num,num]` deg default `[0,0]` each `[-89, 89]`, `pivot` `[num,num]` default `[W/2.0, H/2.0]`; **quad** `quad` array of 4 `[num,num]` req, `rect` `[num,num,num,num]` default `[0,0,W,H]` (`rw, rh > 0`). Mixing fields of two forms is invalid; a script with no form fields is the identity params form. |
| `image_size` | `w` int req `[1, 16384]`, `h` int req `[1, 16384]`, `interp` `"bicubic"`\|`"nearest"` default `"bicubic"` |
| `canvas_size` | `w` int req `[1, 16384]`, `h` int req `[1, 16384]`, `anchor` one of `"tl" "t" "tr" "l" "c" "r" "bl" "b" "br"`, default `"c"` |
| `crop` | `x` int req, `y` int req, `w` int req `[1, 16384]`, `h` int req `[1, 16384]` |
| `rotate_canvas` | `angle` num req deg, clockwise positive, `[-3600, 3600]`; resulting size `<= 16384` per axis |
| `flip` | `axis` `"h"`\|`"v"` req, `layer` string optional (raster) |
| `gradient` | `layer` string req (raster), `type` `"linear"`\|`"radial"` default `"linear"`, `p0` `[num,num]` req, `p1` `[num,num]` req, `c0` colour req, `c1` colour req, `opacity`, `reverse` bool `false` |
| `bucket_fill` | `layer` string req (raster), `x` int req, `y` int req, `color` req, `opacity`, `tolerance` int `32` `[0, 255]`, `contiguous` bool `true`, `antialias` bool `true` |

**Referencing the selection.** No op names the selection explicitly: it is document state
(§2). Every pixel-editing op, in any doc, reads `E` implicitly per §9. To paint without the
selection, a script issues `deselect` first (and `reselect` after).

Examples:

```json
{"op":"select_ellipse","x":8,"y":8,"w":48,"h":32,"mode":"add"}
{"op":"select_polygon","points":[[32,4],[60,60],[4,60]],"antialias":true}
{"op":"transform","layer":"L","rotate":30,"pivot":[32,32]}
{"op":"transform","layer":"L","quad":[[8,4],[56,12],[60,60],[2,52]]}
{"op":"gradient","layer":"L","type":"radial","p0":[32,32],"p1":[60,32],"c0":"#ff0000ff","c1":"#ff000000"}
{"op":"bucket_fill","layer":"L","x":3,"y":3,"color":"#00ff00","tolerance":10}
```

---

## 20. Golden cases

Conventions for the sketches: "**A**" = canvas 64 × 64, `bg "#00000000"`, then
`{"op":"add_layer","id":"L"}` (an empty transparent raster layer, doc 10). Unless stated, each
selection golden ends with `fill_selection L "#1060e0ff"`, so the output alpha channel equals `S`
(or 255 everywhere when `S` is empty). "**H**" (soft halo content) = canvas 64 × 64,
`bg "#ffffffff"`, `add_layer L`, `select_ellipse 16,16,32,32 aa`, `feather 4`,
`fill_selection L "#ff2000ff"`, `deselect`: a red disc whose alpha falls from 255 to 0 over about
8 px, on pixels that are `(0,0,0,0)` outside, composited over white, so any dark fringe is
visible. "**K**" (checker blocks) = **A** plus `select_rect` of the 8 × 8 blocks at block
coordinates where `(bx + by)` is even (mode `add`, 32 rects, 64 × 64 canvas),
`fill_selection L "#000000ff"`, `deselect`: black blocks touching only at corners.

### 20.1 Selections (24)

| id | script sketch | what bug this catches |
|---|---|---|
| SEL-01 | A; `select_rect 8,8,40,24` | half-open bounds: off-by-one at the right/bottom edge |
| SEL-02 | A; `select_rect 8.25,8.5,40.3,23.75 aa` | AA coverage formula: wrong sample grid, `n/255` instead of `n/256`, or missing `q` |
| SEL-03 | A; `select_rect 10.5,6.4,20.2,30.6` (non-AA) | non-AA must sample the pixel centre, not the corner |
| SEL-04 | A; `select_ellipse 4,6,56,40 aa` | ellipse centre/radius arithmetic, `<=` vs `<` on the boundary |
| SEL-05 | A; `select_ellipse 5,5,33,21` (non-AA, odd size) | half-pixel centre of odd-sized ellipses |
| SEL-06 | A; `select_ellipse 20.3,20.7,2.5,3 aa` | sub-pixel shapes: sample offsets `(2i+1)/32`, not `i/16` |
| SEL-07 | A; `select_polygon [[4,60],[32,3.5],[61,57]] aa` | crossing-point formula and its evaluation order |
| SEL-08 | A; `select_polygon [[32,4],[48,58],[5,24],[59,24],[16,58]] aa` (pentagram) | even-odd fill rule: centre pentagon must be empty (**mutation 33**) |
| SEL-09 | A; `select_polygon [[8.5,8.5],[40,8.5],[40,20.5],[20,20.5],[20,40],[8.5,40]]` non-AA | vertices and horizontal edges exactly on sample rows: half-open `y` rule |
| SEL-10 | A; `select_polygon [[-10,10],[70,20],[30,30],[70,50],[-5,60]]` non-AA | concave polygon partly outside the canvas: clipping and parity with off-canvas vertices |
| SEL-11 | A; `select_ellipse 4,4,36,36 aa`; `select_ellipse 24,20,36,36 aa mode add` | add = max; a clamped sum makes the overlap edges hard |
| SEL-12 | A; `select_ellipse 4,4,40,40 aa`; `select_ellipse 20.5,18.25,40,40 aa mode subtract` | subtract formula where both edges are partial (**mutation 34**) |
| SEL-13 | A; `select_ellipse 10,10,40,30 aa`; same ellipse `mode subtract`; fill | subtract(A, A) must be empty, hence "no selection" and a full fill; the fuzzy `min` leaves a ring (**mutation 34**) |
| SEL-14 | A; `select_rect 4.5,4.5,40,40 aa`; `select_ellipse 16,16,44,44 aa mode intersect` | intersect = min over two soft edges |
| SEL-15 | A; `select_rect 8,8,20,20`; `deselect`; `select_ellipse 30,30,20,20 aa mode add`; `deselect`; `reselect` | Reselect restores the last deselected mask, and only `deselect` writes `Saved` |
| SEL-16 | A; `select_all`; `contract 3`; `select_rect 20,20,24,24 mode subtract`; `contract 2` | contract treats outside-canvas as selected; disc erosion around an inner hole |
| SEL-17 | A; `select_ellipse 8,8,48,40 aa`; `select_inverse` | inverse is `255 - S` including soft edges |
| SEL-18 | K; `select_wand L 2,2 tol 0 contiguous aa false`; fill | 8-connectivity crosses block corners: every black block is selected (**mutation 15**) |
| SEL-19 | A; `select_rect 4,4,16,16`; `select_rect 40,40,16,16 mode add`; `fill_selection L "#000000ff"`; `select_rect 40,4,16,16`; `fill_selection L "#141414ff"`; `deselect`; `select_wand L 8,8 tol 20 contiguous false aa false` | global mode selects every match, including the two blocks not connected to the seed (one of them `d = 20`, exactly at tolerance) |
| SEL-20 | A; `gradient L linear p0 [0,0] p1 [64,0] "#000000ff"→"#ffffffff"`; `select_wand L 10,32 tol 32 aa false` | `d <= t` on the byte scale: exact tolerance boundary (**mutation 35**) |
| SEL-21 | as SEL-20 with `tol 40 aa true` | wand AA fringe formula `q(2·(1.5 − d/t))`, fringe only in the 1-pixel ring |
| SEL-22 | canvas 64×64 `bg "#00000000"`; `add_layer M`; `select_rect 0,0,32,64`; `fill_selection M "#ff0000ff"`; `select_inverse`; `fill_selection M "#ff000080"`; `deselect`; `add_layer L` (above M); `select_wand M 10,10 tol 100 aa false`; `fill_selection L "#1060e0ff"` | alpha is part of the distance: same RGB, alpha differing by 127 must not match at tol 100 (only the left half is selected) |
| SEL-23 | A; `select_rect 16,16,32,32`; `feather 8` | box widths from `sigma = r/2`, integer rounding, edge replicate (**mutation 32**) |
| SEL-24 | A; `select_rect 20,20,10,10`; `select_ellipse 40,8,12,12 aa mode add`; `expand 4` | Euclidean disc dilation: rounded corners on the rect, soft edge preserved on the ellipse |

### 20.2 Transform (14)

Content for TR goldens: "**T**" = canvas 64 × 64, `bg "#ffffffff"`, `add_layer L`,
`gradient L linear p0 [8,8] p1 [56,56] "#ff0000ff"→"#0000ffff"` through
`select_polygon [[12,10],[50,14],[44,52],[16,40]]` (non-AA), then `deselect`: an asymmetric,
hard-edged, coloured quadrilateral that shows orientation, sign and sub-pixel errors.

| id | script sketch | what bug this catches |
|---|---|---|
| TR-01 | T; `transform L matrix [1,0,0,0,1,0,0,0,1]` | identity must reproduce the input bytes; a missing `−0.5` blurs and shifts by half a pixel (**mutation 30**) |
| TR-02 | T; `transform L translate [5,-3]` | integer translation is an exact shift; content leaving the canvas is dropped; transposed matrix turns it into a projective warp (**mutation 31**) |
| TR-03 | T; `transform L translate [0.5,0.25]` | Keys weights at fractional phases, tap indices `ix−1..ix+2` (**mutation 30**) |
| TR-04 | T; `transform L scale [2,2] pivot [32,32]` | upscale via inverse mapping, pivot composition order |
| TR-05 | T; `transform L scale [0.5,0.5] pivot [0,0]` | transform path has no prefilter (a prefilter here is a spec divergence) |
| TR-06 | T; `transform L rotate 30` | rotation sign (clockwise on screen) and `rad` conversion site (**mutation 31**: transpose = rotate −30) |
| TR-07 | T; `transform L rotate 90` | `cos(π/2) ≈ 6e-17` through the general path: result near-exact, both implementations must agree to the byte |
| TR-08 | T; `transform L skew [20,0]` | `tan` term placement in `Sk` and composition order skew-before-rotate |
| TR-09 | T; `transform L matrix [1,0,0, 0,1,0, 0.004,0,1]` | perspective divide `u/w`, row-3 usage (**mutation 31**) |
| TR-10 | T; `transform L quad [[4,4],[52,10],[60,58],[12,52]]` | Heckbert affine branch (`sx == sy == 0`) |
| TR-11 | T; `transform L quad [[16,4],[48,4],[62,60],[2,60]]` | Heckbert projective branch (`den`, `g`, `h`) and `Q · N` |
| TR-12 | T; `transform L matrix [1,0,0, 0,1,0, 0,0.03,1]` (inverse row 3 is `[0, -0.03, 1]`) | `w <= 0` rows (below the horizon at `y > 33.3`) must be transparent, never mirrored |
| TR-13 | T; `transform L rotate 45 interp nearest` | nearest uses `floor(sx)` with no half-pixel offset and copies bytes |
| TR-14 | T; `transform L matrix [-1,0,64, 0,1,0, 0,0,1]` compared visually to `flip h layer L` (golden stores the transform result) | negative determinant; mirror must land exactly on integer phases (identical to the flip) |

### 20.3 Resample / alpha regression (10)

| id | script sketch | what bug this catches |
|---|---|---|
| RA-01 | H; `transform L rotate 30` | black halo: straight-alpha bicubic pulls `(0,0,0,0)` neighbours into soft edges (**mutation 11**) |
| RA-02 | H; `transform L scale [1.7,1.7] translate [0.3,0.3]` | halo on upscale with fractional phase (**mutation 11**, **mutation 30**) |
| RA-03 | H; `transform L scale [0.6,0.6]` | halo on downscale in the transform path (**mutation 11**) |
| RA-04 | H; `image_size 100 100` | Pillow coefficients, premultiplied separable passes, no mid quantisation (**mutation 11**) |
| RA-05 | canvas 128×128 `bg "#ffffffff"`; `add_layer L`; `select_ellipse 32,32,64,64 aa`; `feather 6`; `fill_selection L "#ff2000ff"`; `deselect`; `image_size 48 48` | downscale prefilter: `support = 2·scale`, weight argument `((i − center) + 0.5) / fs`, renormalisation |
| RA-06 | H; `image_size 96 40` | axis order (horizontal first) and independent per-axis scale |
| RA-07 | H; `rotate_canvas 30` | canvas growth `ceil(bw − 1e-6)` = 88 × 88, centring, halo on the canvas path (**mutation 11**) |
| RA-08 | canvas 64×48 `bg "#ffffffff"`; `add_layer L`; `select_ellipse 6,20,30,20 aa`; `feather 3`; `fill_selection L "#ff2000ff"`; `deselect`; `rotate_canvas 90`; `rotate_canvas 180`; `rotate_canvas -90`; `flip v` | exact index mappings on a non-square canvas, `-90` normalising to the 270 path, no resampling on right-angle paths |
| RA-09 | H; `canvas_size 81 51 anchor c`; `crop -3,5,60,70` | `floor_div` for odd deltas (including negative), crop past the canvas fills transparent |
| RA-10 | canvas 32 × 32 bg `#808080ff`; `add_layer L`; opaque white rect 4,4,8,24 and opaque black rect 12,4,8,24 via `fill_selection`; `deselect`; `transform L scale [2.3,2.3] pivot [0,0]` | Keys overshoot: premultiplied colour clamped to `[0, A']` before dividing (no colour > 1, no negative) |

### 20.4 Tools (extra, not counted in the 48 above)

| id | script sketch | what bug this catches |
|---|---|---|
| GR-01 | A; `gradient L linear p0 [5.5,10] p1 [58,50] "#ff0000ff"→"#0000ffff"` | projection `t` at pixel centres, clamp beyond both ends |
| GR-02 | A; `gradient L radial p0 [32,32] p1 [60,40] "#00ff00ff"→"#0000ff00" reverse true` | straight RGBA interpolation (not premultiplied), radius `sqrt(L2)`, reverse after clamp |
| GR-03 | A; `select_ellipse 8,8,48,48 aa`; `gradient L linear p0 [0,0] p1 [64,0] "#ff0000ff"→"#ffff00ff" opacity 0.5` | coverage form: `(ga · opacity) · e` |
| GR-04 | A; `fill_selection L "#808080ff"`; `gradient L linear p0 [10,10] p1 [10,10] ...` | zero-length gradient is a no-op |
| BK-01 | K; `bucket_fill L 2,2 "#00ff00ff" tol 0 aa false` | bucket shares the 8-connected region: every black block turns green (**mutation 15**) |
| BK-02 | SEL-20 content; `bucket_fill L 10,32 "#ff00ffff" tol 16 contiguous false aa false` | global bucket, tolerance on the target layer's own bytes |
| BK-03 | SEL-20 content; `select_rect 0,0,40,64`; `bucket_fill L 10,32 "#00ffffff" tol 24 opacity 0.6` | product order `((ca · opacity) · r) · e` and AA fringe in the bucket |
| BK-04 | A (empty layer) with one opaque rect; `bucket_fill L 0,0 "#ff8000ff" tol 0` | transparent seed matches canonical `(0,0,0,0)` pixels; the region is computed before writing |

---

## 21. Mutation hooks

Each is one defect in one formula; `--selftest-mutate=N` switches it on in the C++ core.

| id | defect (exact site) | caught by |
|---|---|---|
| 11 | §11.2/§11.3 in every bicubic path (transform, image_size, rotate_canvas arbitrary): taps are **not** premultiplied; `r, g, b, a` straight values are mixed with the same weights; finish is `A' = clamp(acca, 0, 1)`, `C = clamp(acck, 0, 1)` with no division | RA-01, RA-02, RA-03, RA-04, RA-07 |
| 15 | §6.3 `region`: the contiguous component `C` uses the **4**-neighbourhood (`dx·dy = 0`); the fringe test is unchanged | SEL-18, BK-01 |
| 30 | §12.4 bicubic: `fx = sx`, `fy = sy` (the `− 0.5` pixel-centre offset is dropped) | TR-01, TR-03, RA-02 (and every bicubic transform golden) |
| 31 | §12.3: the inverse is computed from `Mᵀ` (`m1↔m3`, `m2↔m6`, `m5↔m7` swapped) instead of `M` | TR-02, TR-06, TR-09 |
| 32 | §7: `sigma = r` instead of `sigma = r / 2.0` | SEL-23 |
| 33 | §3.3: nonzero winding instead of even-odd (`wind += (yi > yj) ? +1 : -1` on each crossing; inside iff `wind != 0`) | SEL-08 |
| 34 | §4 `subtract`: `min(S, 255 - B)` instead of `max(S - B, 0)` | SEL-12, SEL-13 |
| 35 | §6.2: match test `(d / 255.0) <= t` instead of `d <= t` (difference normalised, tolerance left on the byte scale) | SEL-20, SEL-21, BK-02 |

---

## 22. Parity notes (feeds `docs/PARITY.md`)

Everything below is either a deliberate divergence or an unknown relative to Photoshop. No claim
here was checked against Photoshop, and by the BUILD-SPEC `<prohibitions>` none may be.

1. **Anti-aliasing algorithm**: 16 × 16 regular supersampling for marquee and lasso edges.
   Photoshop's edge coverage algorithm is **unknown**; expect last-level differences on every
   soft edge.
2. **Rectangle marquee** accepts fractional coordinates and an `antialias` flag in scripts; the GUI
   snaps to whole pixels, where AA has no effect. Divergence only for scripted fractional input.
3. **Lasso fill rule** is even-odd (GIMP's). Photoshop's rule for self-overlapping lassos is
   **unknown**.
4. **Boolean add** is `max`; **subtract** is clamped difference; **intersect** is `min`. The
   Photoshop arithmetic on partially selected pixels is **unknown**. GIMP's `add` is a clamped sum,
   so we diverge from GIMP there deliberately (§4).
5. **Empty selection means no selection** (GIMP model). Photoshop distinguishes "nothing selected"
   and warns when a selection becomes invisible ("no pixels are more than 50 % selected"); we emit
   no warning, and a selection with every byte 0 simply stops restricting edits.
6. **Magic wand metric**: max over R, G, B, A byte differences, tolerance 0–255, default 32
   (the tolerance range and default match the familiar UI; the metric itself is **unknown** in
   Photoshop). Wand and bucket sample only the target layer: **no "Sample All Layers"**, and no
   sample-size averaging. The wand's anti-alias is a 1-pixel colour-distance fringe (GIMP's formula),
   not Photoshop's (unknown) edge smoothing.
7. **Feather radius → sigma = r/2** with an almost-Gaussian three-box blur and replicated edges.
   Photoshop's radius-to-kernel relation and its canvas-edge behaviour are **unknown**; GIMP uses
   `r/3.5`.
8. **Expand/Contract** use a Euclidean disc; Photoshop's structuring shape is **unknown**. Contract
   does not shrink from the canvas border (outside counts as selected), with no option to change it.
   Range 1–100 px (Photoshop allows more).
9. **Transform with an active selection** transforms the whole layer, not the selected pixels
   (Photoshop floats and transforms only the selection). **Divergence.** The layer mask is not
   transformed with the layer (Photoshop transforms linked masks). **Divergence.** Content moved
   outside the canvas is discarded (Photoshop keeps off-canvas pixels in the layer). **Divergence.**
10. **Transform resampling** is Keys `a = −0.5` over a fixed 4 × 4 footprint with no downscale
    prefilter, so large reductions alias. Photoshop's "Bicubic Automatic / Smoother / Sharper" are
    **unknown**; we offer only bicubic and nearest (no bilinear) in v0.1.
11. **Image Size** uses Pillow-style widened Keys (`a = −0.5`) with renormalised edges; Photoshop's
    kernels are **unknown**. No "Preserve Details" or other methods.
12. **Canvas Size** fills new area with transparency in every layer; the document `bg` shows
    through. Photoshop extends a locked Background layer with a "canvas extension colour"; Rasterloom
    v0.1 has no Background-layer concept in this doc (depends on doc 10). Odd-delta centring uses
    floor division; Photoshop's rounding is **unknown**.
13. **Canvas-geometry ops clear the selection and Reselect memory.** Photoshop's behaviour per op is
    **unknown** (some keep or transform the selection).
14. **Rotate Canvas (arbitrary)** grows the canvas to `ceil` of the rotated bounding box (with a
    `1e-6` tolerance) and fills new area with transparency. Photoshop's rounding is **unknown**.
15. **Gradient**: two colours only, no midpoint, no multi-stop editor, **no dithering** (Photoshop's
    gradient tool dithers by default, so smooth gradients will differ by ±1 level), linear and radial
    only (angle, reflected and diamond are v0.2). Straight colour interpolation with independent
    alpha ramp.
16. **Paint Bucket** samples the target layer only, fills in Normal mode only (no bucket blend mode),
    and its anti-alias is the same colour-distance fringe as the wand.

---

## 23. Cross-document assumptions (to be confirmed by the owning docs)

- Doc 10: `add_layer` with no fill creates a fully transparent, canvas-sized raster layer;
  `lock_transparency` is a boolean layer flag; layer masks are canvas-sized single-channel byte
  arrays; the render output is the flattened composite over `canvas.bg`; a hide/visibility op
  exists (used by SEL-22).
- Docs 20 and 40 state for each pixel-editing op whether it uses the coverage form or the replace
  form of §9.
