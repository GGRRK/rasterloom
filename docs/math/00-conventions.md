# Rasterloom normative math — 00: conventions

Status: normative for v0.1. Every other file in `docs/math/` inherits these rules.

Two implementations are written against `docs/math/`: the C++ core (`librasterloomcore.a`) and the
NumPy reference (`tests/reference/refcomp.py`). They are written by different authors, and neither
may read the other. They must agree **byte for byte** on every render script. A formula that
is ambiguous, or whose evaluation order is unstated, will make them disagree in the last bit,
and that shows up as a golden failure at a rounding boundary. So these documents state every
formula in evaluation order, not just as mathematics.

## C1. Number format

- All per-pixel arithmetic is IEEE-754 **binary64** (`double` / `numpy.float64`). No `float` anywhere
  in a pixel path, no extended precision, no fused multiply-add (C++ builds with
  `-ffp-contract=off -fno-fast-math`; the reference never calls `math.fma`).
- Formulas are written in a code-like form. **Evaluate exactly as written**: left to right for
  operators of equal precedence, with the parentheses shown, no algebraic rewriting, and no
  hoisting that changes the operation sequence. `a*b*c` means `(a*b)*c`. `1 - a*b` means
  `1 - (a*b)`. If you want a different form for speed, prove it gives identical doubles or don't
  use it.
- `+ - * /` and `sqrt` are correctly rounded by IEEE-754, so both languages agree on them
  automatically (`numpy.sqrt` is fine).
- **Transcendentals** (`pow`, `exp`, `log`, `sin`, `cos`, `tan`, `atan2`, `hypot`, `cbrt`) are *not*
  required to be correctly rounded, and NumPy's SIMD versions differ from glibc in the last ulp. Rule:
  C++ calls the glibc `libm` function (`std::pow` etc. on `double`); the reference calls the
  **Python `math` module on scalars** (which is the same glibc libm), never a NumPy ufunc for these.
  Where a transcendental is needed per pixel, the math docs must restructure it as a 256-entry
  lookup table built from scalars or a per-dab/per-row scalar precompute, so the reference
  stays fast without vectorised transcendentals.
- `min`, `max`, `abs`, `floor`, `ceil` and comparisons are exact in both languages.

## C2. Channel normalisation and quantisation

- Decode: `c = v / 255.0` for an 8-bit value `v` (a division, not `v * (1/255.0)`).
- Encode, the **single shared quantisation rule** used everywhere a double becomes a byte:
  ```
  q(x):  y = clamp(x, 0.0, 1.0) * 255.0          # one multiply of the clamped double
         return round_half_away_from_zero(y)      # y >= 0, so: floor(y) + ((y - floor(y)) >= 0.5)
  ```
  In C++: `static_cast<uint8_t>(std::round(y))`. In NumPy: `f = np.floor(y); f + ((y - f) >= 0.5)`.
  Never `int(y + 0.5)` (the addition rounds), never Python `round` or `np.round` (half-to-even),
  never truncation.
- `clamp(x, lo, hi) = min(max(x, lo), hi)`. NaN must never reach `q`; if an operation can divide
  by zero, its doc states the guard.

## C3. Pixels, alpha, storage

- Storage is 8-bit **straight (unassociated)** RGBA, channel order R, G, B, A.
- **Canonical transparent pixel:** whenever a quantised alpha is 0, the stored pixel is
  `(0, 0, 0, 0)` regardless of the computed colour. Both implementations apply this after every
  write, which keeps byte equality from depending on the colour of invisible pixels.
- Colour division by a result alpha uses the **unquantised** double alpha, then quantises colour
  and alpha independently with `q`.
- Premultiplication happens transiently, only for neighbour-mixing operations (resample, blur,
  warp, transform) and the display upload, per BUILD-SPEC D2: `P = C * a` in double, mix, then
  `C = P / a` if `a > 0` else 0, then `q`. A math doc that mixes neighbours states exactly where
  it premultiplies.

## C4. Geometry

- Pixel `(x, y)` covers the half-open square `[x, x+1) × [y, y+1)`; its centre is
  `(x + 0.5, y + 0.5)`. Origin top-left, `y` grows downward. Canvas coordinates are doubles.
- Tiles are 64×64. Tile `(tx, ty)` holds pixels `x ∈ [64*tx, 64*tx+64)`, `y ∈ [64*ty, 64*ty+64)`.
- **Iteration order** is row-major everywhere: tiles by `(ty, tx)`, pixels by `(y, x)`. An
  operation whose result depends on visiting order (flood fill, error diffusion, anything
  with running state) is specified in that order unless its doc states another.
- Out-of-canvas reads return the transparent pixel `(0, 0, 0, 0)` unless an operation's doc
  specifies an edge mode (clamp, wrap).

## C5. Compositing granularity

- The compositor composites **one layer (or one finished group) at a time onto an 8-bit
  backdrop, and quantises the result immediately** with `q` per C2/C3. There is no
  higher-precision accumulator spanning several layers. This makes every intermediate buffer (and
  any cache the GUI keeps) exact, so a render never depends on which layer is active or which
  tiles were cached.
- Adjustment layers are "layers" for this purpose: they read the 8-bit backdrop, produce an
  8-bit adjusted colour, and that result is blended onto the backdrop like a layer's pixels.

## C6. Deterministic randomness

Anything random (Dissolve, Add Noise, dab jitter if ever added) uses only this generator:

```
splitmix64(z):                       # all arithmetic modulo 2^64 on unsigned 64-bit
    z = z + 0x9E3779B97F4A7C15
    z = (z XOR (z >> 30)) * 0xBF58476D1CE4E5B9
    z = (z XOR (z >> 27)) * 0x94D049BB133111EB
    return z XOR (z >> 31)

pixel_hash(seed, x, y, k) = splitmix64(seed XOR splitmix64((y << 32) OR x) XOR (k * 0xD1B54A32D192ED03))
unit(h) = (h >> 11) * 2^-53          # a double in [0, 1), exact
```

`x`, `y` are canvas pixel coordinates (non-negative, < 2^32), `k` is a per-use channel/stream
index stated by the calling doc, and `seed` is an explicit parameter of the operation. In Python,
mask every step with `& 0xFFFFFFFFFFFFFFFF`. There is no global RNG state, so results never depend
on call order.

## C7. Units and parameter conventions

- Opacity, fill, flow, hardness and similar parameters are doubles in `[0, 1]` in render
  scripts and in the core API. The GUI's 0–100 % is a presentation detail.
- Angles are degrees in scripts, converted once to radians with `deg * (π / 180)` where
  `π = 3.141592653589793`. A doc that uses angles states the exact conversion site.
- Colours in scripts are `"#RRGGBB"` or `"#RRGGBBAA"` straight-alpha hex strings.

## C8a. Shared model facts (resolved 2026-09-26 across docs 10–40)

- **Layers and masks are canvas-sized** in v0.1: a raster layer is a W×H RGBA8 grid in canvas
  coordinates (sparse by tile in C++), with no layer offset; a layer mask is a W×H single-channel
  byte grid. Content moved outside the canvas is discarded.
- **Selection** is one canvas-sized 8-bit coverage mask owned by doc 30. No active selection means
  coverage 1.0 (byte 255) everywhere; a selection that becomes all-zero is the same as no selection.
  An op reads it as `s = sel / 255.0`.
- **Lock transparency** is the raster-layer flag `lock_alpha` (op `lock_transparency`, doc 10 §10).
  Painting ops (brush, clone, gradient tool, bucket) use source-atop: colour
  `(Cd * (1.0 - a)) + (Cs * a)`, alpha byte unchanged, alpha-0 pixels untouched; the eraser is a no-op
  on a locked layer. Filters use doc 20 §B0 (filter normally, then restore the original alpha).
- **Adjustment layers** are created by `{"op":"add_adjustment","id":…,"type":…}` (doc 20), placed
  exactly like `add_layer` (doc 10).

## C9. Render-script parsing and errors

- JSON numbers parse to the nearest binary64, correctly rounded (Python `float()`, glibc `strtod`,
  nlohmann-json's default number parser). Integers must be integral JSON numbers where a doc says
  `int`.
- An unknown op, an unknown field, a wrong type, an out-of-range value, a reference to an unknown
  id or to the wrong node kind, a singular matrix, or a result larger than 16384 px per axis is a
  **script error**: the renderer exits non-zero and writes no PNG. Values are never silently
  clamped; the one exception is a stroke sample's `pressure`, which doc 40 clamps to `[0, 1]`.
- `"out": "png8"` writes the final composite (doc 10 RENDER over `canvas.bg`) as 8-bit RGBA PNG,
  without altering any byte (alpha-0 pixels are already canonical `(0,0,0,0)`).

## C10. History (undo) semantics

- Every document-changing op pushes **exactly one** history record; a brush/eraser/clone stroke
  is one op and therefore one record (BUILD-SPEC D5). Selection ops are history records too.
- `{"op":"undo","steps":n}` (`n` int ≥ 1, default 1) restores the document state from before the
  last `n` records. Undoing past the start of history is a script error.
- Render scripts run with history depth 1000; the GUI default is 50 (configurable 1–1000).
- The reference implements undo by snapshotting the whole document before each op; the C++ core
  uses copy-on-write tile references. Only the resulting pixels are compared.

## C8. What each math doc must contain

1. The semantics in words: what the user sees.
2. The exact formulas in evaluation order per C1.
3. Edge cases: zero alpha, division guards, out-of-canvas behaviour, parameter clamping.
4. The **render-script ops** that exercise it (`{"op": ..., ...}`, with every field, type, default
   and valid range), so the C++ CLI and `refcomp.py` accept identical JSON.
5. Suggested golden cases (what to render, why that case catches a real bug).
6. The **mutation hooks** it owns: which defect in which formula, so `--selftest-mutate=N` has a
   precise target and the golden set demonstrably catches it.
7. Where the semantics are a deliberate divergence from, or an unknown relative to, Photoshop,
   say so plainly: that text feeds `docs/PARITY.md`.
