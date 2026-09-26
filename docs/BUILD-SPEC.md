# Rasterloom — v0.1 "Foundation" build specification

This document is the contract the implementation is written against. Hand it to an agentic coder
verbatim. Every decision in `<hard_decisions>` is pre-made: do not re-derive it.

## Addendum 2026-09-26 — Linux-local development (overrides the sections it names)

The commissioner has since moved to a Linux development machine. The "Windows, no Linux, CI is the only machine" constraint in `<context>`, `<eyes>` and
`<guardrails>` no longer holds. Where this addendum and the body disagree, the addendum wins.

- **Build, run and verify locally first.** Toolchain present: GCC 16,
  CMake 4.4, Ninja, Qt 6.11.2 (system), lcms2 2.19.1, libpng, libjpeg-turbo, libtiff, zlib, lz4,
  libzip, nlohmann-json, fmt, gtest, highway 1.4, Python 3 + NumPy 2.5 + Pillow, and
  `.venv/` (gitignored, `--system-site-packages`) with `psd-tools`. `gh` is installed and
  authenticated, so the curl recipe in `<eyes>` is superseded. Do not `pacman -S` anything without
  asking the commissioner. Missing locally: docker/podman, Xvfb, odiff, linuxdeploy.
- **Portability floor unchanged:** code must still compile with GCC 11 and Qt 6.9.3 (the CI and
  AppImage configuration). No C++23 features and no Qt API newer than 6.9.
- **GUI tests locally run under `QT_QPA_PLATFORM=offscreen`.** Never open a visible window on the
  commissioner's desktop from a test or an agent.
- **Pixel comparison:** `tests/tools/pixcmp.py` (decoded RGBA byte-equality, writes a diff mask on
  failure) is the local equivalent of `odiff --threshold=0`. CI may use either.
- **Byte-exactness contract:** `docs/math/` is the normative math, starting with
  `00-conventions.md` (binary64 everywhere, one quantisation rule, libm-only transcendentals,
  per-layer 8-bit compositing, splitmix64 randomness). The C++ core and `refcomp.py` are both
  written from `docs/math/` by different authors; neither may read the other.
- **SIMD:** the scalar binary64 path is the reference implementation. A Highway SIMD path is
  allowed only for Normal-mode composite and only with a test proving it byte-equal to scalar.
- **Distro matrix and the ubuntu:22.04 glibc floor** remain CI-only (no containers locally). The
  first real-hardware tester is now the commissioner on their Linux desktop; the friend-AppImage path
  still applies.
- **PSD I/O is our own codec, not a PhotoshopAPI fork** (decided 2026-09-26 on measured evidence,
  `docs/research/psd-and-ora.md`). PhotoshopAPI v0.9.1 (still the newest release) hard-requires
  OpenImageIO (breaks "no OIIO in v1" and the size budget), uses `std::format` at 77 sites (breaks
  the GCC 11 floor), leaks `-O3 -mavx2` as PUBLIC flags (breaks the SIGILL rule), defaults every
  layer's fill to 0, writes a black 3-channel merged image (#179, still open), and drops unknown
  tagged blocks — five fork patches plus Eigen/stduuid/blosc2/simdutf/mio/TBB for a library that
  still would not round-trip. Replacement: `src/core/psd/`, written from Adobe's public PSD/PSB
  file-format specification, zlib + libjpeg-turbo only, raw byte-preserving pass-through of every
  tagged block and image resource we do not interpret, RLE-written channels, merged composite with
  IRB 1036 + 1057 from commit #1. psd-tools (MIT) is the external cross-check and may be read as a
  format reference, with provenance noted. v0.1 imports 16-bit and CMYK files by converting to 8-bit
  RGB (CMYK stored inverted per spec) and says so in PARITY.
- **Qt/packaging findings adopted** (`docs/research/qt-platform.md`): ADS pinned to v5.1.1, linked
  shared; D6 platform choice made in `main()` before `QApplication` (`RASTERLOOM_PLATFORM` if set,
  else `xcb` when `DISPLAY` is set, ignoring an inherited `QT_QPA_PLATFORM`); `RASTERLOOM_CANVAS=gl|raster`
  override, offscreen tests use `raster`; canvas uploads to GL texture arrays/atlas pages, not one
  texture per tile; tablet tests send `QTabletEvent` directly; the bundle checks become
  `grep -qE 'platforms/libqwayland(-generic)?\.so'` and `unsquashfs -o "$(./X.AppImage --appimage-offset)"`;
  add `--exclude-library 'libglib-2.0.so*'`; drop linuxdeploy-plugin-checkrt; uruntime with
  `URUNTIME_EXTRACT=2`. Local Arch AppImages are personal test builds only; the distributable one
  comes from the ubuntu:22.04 + Qt 6.9.3 CI job.
- **Also ship `.ora`:** GIMP recognises only the `.ora` extension, so Save As offers "OpenRaster
  (.ora)" alongside `.orp`; both write the same bytes.
- **Git and release model (commissioner, 2026-09-26):** development happens in the private
  `GGRRK/rasterloom` (branch `v0.1-foundation` for this build, full history, pushed at milestones).
  Releases go to a separate **public** repository: one clean snapshot commit per release under the
  GitHub noreply identity, a `vX.Y.Z` tag, and a GitHub Release with the AppImage and its `.sha256`.
  The public tree is scrubbed of personal details (email, local paths, machine specifics) before
  publishing. CI for the distributable AppImage and the distro matrix may run on the public repo.

---

<role>
You are a senior C++ graphics engineer building a professional raster image editor for Linux.
You have shipped tile-based compositing engines and know that in image editing the subtle bugs
(alpha association, blend space, resample halos, flow accumulation) matter more than feature count.
You work unattended and you never claim something works that you have not proven with output.
</role>

<context>
Rasterloom is a native Linux desktop image editor: layer-based, non-destructive, GPU-composited
for display, reading and writing Photoshop files. It is an original implementation. It is not
affiliated with Adobe and is not derived from Adobe code.

The operating situation, which drives most of the constraints below:

- The repository is `GGRRK/rasterloom` (private, already created, `main`, one scaffold commit).
- The person commissioning this develops on Windows 11 with NO WSL, NO Docker and NO local Linux,
  and has declined to install one. You therefore cannot compile or run this app locally. Ever.
- ALL building, running and verification happens on GitHub Actions Linux runners.
- The only human tester is a friend who receives one `.AppImage` file. Every round trip through him
  costs a day, so a bug that reaches him is roughly 100x more expensive than one CI catches.
- You are on Windows. `gh` CLI is NOT installed. `cmake` is NOT installed locally. Python 3.12 is.
  You reach GitHub through `curl` plus a token from `git credential fill` (recipe in `<eyes>`).
</context>

<task>
Ship Rasterloom v0.1 "Foundation": a Linux AppImage that a person can open a photo in, work with
real layers, masks, selections and a pressure-sensitive brush, and save as `.orp` or `.psd` —
proven correct by automated pixel tests before any human sees it.
</task>

<hard_decisions>
These were researched and settled. Implement them; do not survey alternatives.

**D1. License `GPL-3.0-or-later`**, SPDX header in every file. This is deliberate and it unlocks
lcms2 GPL plugins, LibRaw, and adapting algorithms from Krita/GIMP with provenance noted in commit
messages. Qt stays *dynamically* linked as `.so` files inside the AppImage regardless.

**D2. Straight (unassociated) alpha in all tile storage.** Premultiply transiently at exactly two
places and un-premultiply immediately after: (a) any neighbour-mixing operation — bilinear/bicubic
resample, blur, warp, free transform; (b) upload to the display texture. Never persist a
premultiplied tile. Storing premultiplied makes all 27 blend modes subtly wrong at partial alpha;
resampling straight alpha without premultiplying puts black halos on every transform. Both failures
live exactly on soft brush edges, which is where users look.

**D3. Blend in the document's gamma-encoded space, not linear light.** Luma for the four
non-separable modes is `0.30R + 0.59G + 0.11B`, never Rec.709. `soft-light` uses the W3C
`D(Cb) = Cb<=0.25 ? ((16Cb-12)Cb+4)Cb : sqrt(Cb)`, never the Pegtop approximation.

**D4. Groups have two code paths from commit #1** — pass-through (`pass`, the default for new
groups) and isolated. Retrofitting pass-through later is a compositor rewrite: rendering children to
an offscreen buffer makes it structurally impossible. An adjustment layer inside a pass-through
group must affect layers *below the group*. Clipping masks composite the whole clip group using the
*base* layer's blend mode and opacity onto the backdrop, with `clbl` toggling that.

**D5. Undo is one Edit per user gesture, riding on the copy-on-write copy.** A whole brush stroke
opens a memento at pointer-down and commits ONE record at pointer-up. The pre-existing
`shared_ptr<const Tile>` IS the undo record — zero copies beyond the one CoW already required.
Depth 50, configurable 1–1000.

**D6. Platform default is `xcb` (X11 / XWayland).** Bundle the Wayland plugins; expose native
Wayland as an experimental opt-in via `RASTERLOOM_PLATFORM`, chosen in Settings, applied by AppRun.
Native Wayland today costs missing GNOME window decorations (the app launches with no close button
and reads as "broken"), a silently window-local `QCursor::pos()` (breaks eyedropper and panel
snapping — silently wrong, not obviously broken), and a no-op `QWindow::setPosition()`. It buys
about 3 ms against a ~50 ms perceptual inking threshold. Revisit in 0.6.

**D7. Goldens are byte-exact (`odiff --threshold=0`) against an independently-written NumPy
reference implementation** — never against the app's own prior output. See `<verification>`.

**D8. The `<prohibitions>` list is prohibitions, not omissions.** An agent fills omissions.

The v1 compositor is single-threaded across tiles in a deterministic order, intra-tile SIMD only.
Design `ICompositeScheduler` for threading now, implement it in 0.2, and keep a `--deterministic`
flag that forces the single-threaded path forever. Latency is fine anyway because the architectural
win is not threading — it is never recompositing the full canvas during a stroke: cache BELOW (all
layers under the active one) and ABOVE, and per frame recomposite only stroke-dirtied tiles.
</hard_decisions>

<stack>
| Layer | Decision |
|---|---|
| Language / build | C++20, CMake >= 3.21, Ninja, GCC 11 |
| Toolkit | **Qt 6.9.3 pinned exactly** — `Core Gui Widgets Concurrent Svg OpenGLWidgets WaylandClient` |
| Docking | Qt-Advanced-Docking-System 5.0 via FetchContent (perspectives, auto-hide, float-in-float) |
| Canvas | `QOpenGLWidget`, OpenGL 3.3 Core, display-only: `glTexSubImage2D` dirty tiles + textured quads |
| GL fallback | **Mandatory.** If context creation fails, or `GL_RENDERER` matches `llvmpipe\|softpipe\|swiftshader` and upload throughput is below threshold, fall back to `QWidget` + `QPainter::drawImage`. ~200 lines. Log `GL_VENDOR/RENDERER/VERSION` at startup and surface it in Diagnostics. |
| Tile engine | Own: 64×64, sparse `unordered_map<TileKey, shared_ptr<const Tile>>`, CoW, straight RGBA8, shared all-zero empty tile for absent entries |
| SIMD | `google/highway` 1.2.x with `HWY_DYNAMIC_DISPATCH`, baseline `x86-64-v2`, AVX2 picked at runtime |
| Color | lcms2 2.19.1 (v1 assumes sRGB, no display transform) |
| PSD | Vendored fork of `EmilDohne/PhotoshopAPI` v0.9.1 (BSD-3) at `thirdparty/PhotoshopAPI` |
| Codecs | libpng16, libjpeg-turbo, libtiff5, zlib — linked directly from container apt. **No OpenImageIO in v1**: it silently drops any codec whose `-dev` package was absent at configure time, and `ubuntu:22.04` has no `libjxl-dev`. |
| Native format | `.orp` = ZIP, strict OpenRaster 0.0.6 superset (`mimetype` STORED first, `stack.xml`, `data/layer*.png`, `mergedimage.png`, `Thumbnails/thumbnail.png`) + private `document.json`. GIMP/Krita/MyPaint can open it. Atomic save: tmp + fsync + `rename(2)`. |
| Packaging | `linuxdeploy` + `-plugin-qt` + `-plugin-checkrt`, repacked by `appimagetool --runtime-file ./uruntime-appimage-squashfs-lite-x86_64` v0.6.1 — the static runtime removes the libfuse2 dependency and survives Ubuntu 26.04's FUSE2 removal |
| CI | `runs-on: ubuntu-24.04` with `container: ubuntu:22.04` (glibc 2.35 floor) |

Compile flags, non-negotiable: `-march=x86-64-v2 -O2 -fno-fast-math -ffp-contract=off`.
**Never `-march=native`, never a global `-mavx2`** — the runner's CPU is not the friend's, and the
failure mode is `SIGILL` on launch with zero diagnostics.

Two mandatory patches to the PhotoshopAPI fork:

1. **Merged composite image.** Upstream writes an empty Image Data section (open issue #179). Such
   files open fine *in Photoshop* — so a naive test passes — and render **black in GIMP**, which is
   exactly what a Linux tester will open them in. Render the flattened composite, emit it as
   RLE/PackBits planar channels in document channel order, plus IRB 1036 (thumbnail) and IRB 1057
   (`hasRealMergedData=1`).
2. **CMYK negate** on read and on write (PSD stores CMYK inverted). Not reachable in v1's RGB-only
   scope, but land it with a test now, because it is invisible under RGB testing.

`psd_sdk` is disqualified by its own README: it cannot write nested layer trees, text layers or
smart objects, so it can never be the save path.

Size budget: target <= 200 MB, hard ceiling 350 MB. Krita's AppImage is 322 MiB — that is the
sanity anchor. Anything that pushes past the ceiling is deferred, no exceptions.
</stack>

<requirements>
Priority order. Each is testable.

1. **Split the binary in three on commit #1** — nothing in `<verification>` works otherwise:
   - `librasterloomcore.a` — tiles, blend modes, filters, selections, brush math, PSD I/O, `.orp`
     I/O. **Zero Qt GUI.** An image editor is unusually lucky: ~85% of the correctness surface is
     pure `(pixels, params) -> pixels` and needs no display at all.
   - `rasterloom-cli` — headless driver. No display, no GL, no `QApplication`.
   - `rasterloom` — Qt Widgets GUI, containing no image math whatsoever.
2. **Tile engine and document model.** 8-bit RGBA only. sRGB assumed. Canvas <= 16384×16384.
   Memory: hard limit 50% of RAM, soft 2%, scratch cap 4096 MiB in `$XDG_CACHE_HOME` via
   `O_TMPFILE`, LZ4 for warm tiles.
3. **Layers.** Raster layers; groups (pass-through and isolated, per D4); reorder; opacity; **fill
   opacity** as a distinct property; visibility; lock-transparency; **all 27 blend modes plus Pass
   Through** using W3C Compositing-1 formulas; layer masks (paint / enable / disable / delete /
   apply); clipping masks; merge down / merge visible / flatten.
4. **Selections**, stored as an 8-bit mask rather than a path: rectangular and elliptical marquee,
   freehand and polygonal lasso, magic wand (contiguous + global, tolerance, anti-alias); boolean
   new / add / subtract / intersect; Select All / Deselect / Reselect / Inverse; Feather; Expand /
   Contract.
5. **Brush engine.** Round tip; size / hardness / spacing (default 25%) / opacity / flow / angle /
   roundness; pressure→size and pressure→opacity curves (16 control points baked to a 256-entry
   uint16 LUT at load); **Wash (default) and Build-up** modes; airbrush `dabs_per_second` term; EMA
   stabilizer. Read `libmypaint` (ISC) as a reference for `count_dabs_to()` spacing semantics.
   **Flow is a lerp toward an opacity ceiling within the stroke, never repeated source-over** — the
   latter races every self-crossing stroke to 100% and reads as "the whole engine is bad" rather
   than "one lerp is inverted". Accept `QTabletEvent` unconditionally: Qt compresses synthetic
   mouse events, so rejecting tablet events silently drops brush samples.
6. **Tools (10):** Move, Brush, Eraser, Paint Bucket, Gradient (linear + radial), Clone Stamp,
   Eyedropper, Crop, Hand, Zoom.
7. **Adjustment layers (8):** Levels, Curves, Brightness/Contrast, Hue/Saturation, Black & White,
   Invert, Posterize, Threshold.
8. **Filters (6):** Gaussian Blur (3-pass box, O(1) in radius), Motion Blur, Unsharp Mask, Add
   Noise, High Pass, Offset.
9. **Transform** via 3×3 homography: move / scale / rotate / skew / distort / perspective, bicubic
   resample with premultiply-around per D2. No warp mesh.
10. **Image menu:** Image Size, Canvas Size, Crop, Rotate Canvas 90/180/arbitrary, Flip H/V.
11. **File:** open and save `.orp`; import PSD/PSB; export PSD/PSB **with merged composite**; open
    and export PNG, JPEG, TIFF (8-bit).
12. **History panel**, 50 states default, one entry per user gesture.
13. **Shell:** ADS docking with saved perspectives; dark Fusion palette plus targeted `.qss`;
    Layers, History, Tool Options bar, Color picker, Navigator, canvas.
14. **Diagnostics — the tester's only channel back. Treat as P1, not polish:**
    - Help → **Input Diagnostics**: live `platformName()`, `GL_VENDOR/RENDERER/VERSION`, tablet
      device name, `pointerType`, pressure, tilt, event rate, dab-to-pixel ms, tiles/frame, cache
      hit rate. The friend screenshots this panel. It is the **only possible tablet verification**.
    - `rasterloom --version --verbose` printing glibc, Qt, platform and every bundled lib version.
    - `rasterloom --selftest [--selftest-mutate=N] [--junit-xml=F]`, `--license`,
      `--third-party-notices`.
    - `rasterloom-cli --render-script X.json --out Y.png`.
15. **Release hygiene, or the release job fails:** `LICENSE` (GPL-3.0), `COPYING.LESSER` (LGPL-3.0
    for Qt), `THIRD-PARTY-NOTICES.md` (every bundled `.so`: name, version, SPDX, URL, full text),
    `NOTICE`, `AUTHORS`, and `WRITTEN-OFFER.txt` carrying the Qt source tarball URL, git SHA,
    configure flags and a verbatim relink recipe. All under `usr/share/doc/rasterloom/` inside the
    AppImage, and reachable from Help → About → Licenses.
</requirements>

<prohibitions>
Do NOT build these in v0.1. They are prohibitions, not gaps to be helpfully filled:

Text and type layers · Pen, paths, shape layers, vector masks · Smart Objects and Smart Filters ·
**Layer styles (Drop Shadow, Stroke, etc.)** · Healing / Spot Healing / Patch / any content-aware
fill · Dodge / Burn / Sponge / Smudge / Blur / Sharpen tools · Mixer Brush · Quick Selection /
Object Selection / Select Subject · Refine Edge · Channels panel · 16-bit and 32-bit float · CMYK /
Lab / Indexed / Duotone · ICC display transform and soft proofing · RAW / JXL / AVIF / EXR / WebP /
HEIC · Actions, batch, scripting, plugins · **All ML and ONNX** · **All GPU compute shaders** ·
Multithreaded compositor · Native-Wayland default · Animation and video timeline · Liquify · Lens
Correction · the decorative filter gallery · **3D — Adobe removed it in 22.5, so an agent working
from stale training data will build a domain that no longer exists. Do not.**

Layer styles are the most painful cut and will be the tester's first complaint. That is intended and
correct: they are the first item of v0.2.

Legally barred, with dates — do not implement, and do not accept as a "suggestion":

- PatchMatch / Content-Aware Fill: US8571328B2 until **2031-10-19**, US8818135B1 until **2033-02-23**.
- Seam carving / Content-Aware Scale: US7477800B2 until **2027-03-06**.
- Poisson healing IS free (US6587592B2 expired 2021-11-19) — scheduled for v0.5, not v0.1.

**Never install, launch, decompile, or output-compare Adobe Photoshop.** Adobe's General Terms bar
even black-box "monitoring or tracking the inputs and outputs flowing through a system", and SAS v
World Programming turned exactly that kind of conduct into a $79.1M judgment *after* the copyright
defence had already succeeded. All behavioural reference comes from the public PSD format
specification, W3C/ISO standards, published papers and open-source documentation. The strings
`photoshop`, `adobe` and `ps` must not appear in the product name, repo slug, binary, window title,
domain or icon. `PSD` may appear only in file filters and prose.
</prohibitions>

<verification>
No human will see this app before the friend does. The test suite is the only thing standing between
you and a wasted delivery cycle. This is the most important section in the document.

**Determinism is a build requirement, not an aspiration.**

- `-fno-fast-math -ffp-contract=off`; fixed tile iteration order (row-major by `(y,x)`), always.
- No `unordered_map` iteration order anywhere the output depends on it — sorted key vectors instead.
- One shared, explicit 8-bit quantization rule: round-half-away-from-zero on the binary64 product `clamp(x,0,1)*255.0` (see `docs/math/00-conventions.md` C2; never a `float`). This is
  where two implementations diverge first, so state it once and use it everywhere.
- CI renders every script **twice and `cmp`s the two PNGs**. Any difference fails the build.

**The render-script contract** — `tests/scripts/*.json`, a deterministic op sequence, and also the
future scripting API, so it is not throwaway:

```json
{ "canvas": {"w":256,"h":256,"bg":"#00000000"},
  "ops": [ {"op":"add_layer","id":"bg","fill":"gradient","from":"#000000","to":"#ffffff","dir":"h"},
           {"op":"add_layer","id":"fg","fill":"gradient","from":"#ff0000","to":"#0000ff","dir":"v"},
           {"op":"set_blend","layer":"fg","mode":"sLit"},
           {"op":"set_opacity","layer":"fg","value":0.5},
           {"op":"flatten"} ],
  "out": "png8" }
```

`rasterloom-cli --render-script X.json --out Y.png` must be bit-reproducible across runs, machines
and thread counts.

**Goldens must not use the app as its own oracle.** If the app generates its own goldens they
enshrine its bugs and prove only "nothing changed". Write `tests/reference/refcomp.py` — roughly 600
lines of pure Python + NumPy at float64 — authored **from the W3C Compositing-1 text and the 11
Photoshop-only formulas, deliberately not by porting the C++**. It consumes the same render scripts.

```bash
rasterloom-cli --render-script S.json --out cpp.png
python tests/reference/refcomp.py S.json ref.png
odiff --threshold=0 --fail-on-layout ref.png cpp.png
```

Two independent implementations of a written normative spec agreeing byte-for-byte is genuine
cross-validation — the strongest evidence available with no human eye and no permitted Photoshop.

**Golden inventory, 260 files minimum:** blend modes 27 × 4 alphas = 108 · fill-vs-opacity 16 ·
group semantics 12 · clipping masks 8 · layer masks 6 · selections 24 · brush 20 · filters 18 ·
transform 14 · adjustments 24 · resample/alpha regression set 10.

**The mutation gate.** The single most likely way this ships broken is an unattended agent producing
a suite that only asserts "did not crash", because that is what optimizes for green CI.
`--selftest-mutate=N` injects defect N into the core at runtime. **Every id in the mutation registry
(currently 43 after the brush lane: 0-42) must be caught**, as a build-breaking step:

```bash
for m in $(./rasterloom-cli --list-mutations | cut -f1); do
  ./rasterloom --selftest --selftest-mutate=$m \
    && { echo "MUTATION $m UNDETECTED - SUITE IS VACUOUS"; exit 1; }
done
```

Seed mutations: 0 soft-light Pegtop instead of W3C · 1 Rec.709 luma instead of 0.30/0.59/0.11 ·
2 composite in premultiplied space · 3 composite in linear light · 4 force isolated for pass-through
groups · 5 clipping multiplies by base alpha · 6 fill opacity treated as layer opacity · 7 flow as
repeated source-over · 8 spacing accumulator reset per input event · 9 spacing computed in screen
space · 10 undo recorded per dab · 11 bicubic resample without premultiply · 12 gaussian H/V pass
order swapped · 13 dense tile grid instead of the shared empty tile · 14 truncate instead of round ·
15 magic wand 4-connectivity instead of 8. **Every bug fixed after v1 adds a mutation. The count
only goes up.**

**PSD round-trip corpus** — 12 self-authored files, never Adobe assets: nested groups 3 deep,
pass-through group, clipping stack, layer mask + vector mask, adjustment layer, `lfx2` layer style,
text layer, smart object, 16-bit, CMYK, 30001px PSB promotion, empty layer. Per file: open → save →
reopen → the layer tree structural diff (names, order, blend 4CCs, opacity, fill opacity, mask
presence, group boundaries) must be identical, unparsed tagged blocks must be byte-identical, and
the merged composite written must match our own render of the file. Plus the check that catches the
GIMP-renders-black trap directly:
`python -c "import psd_tools; psd_tools.PSDImage.open('out.psd').composite()"` must be non-black.

**Test the shipped artifact, never the build tree.** Every packaging bug — a missing Qt imageformats
plugin makes PNG export silently write an empty file; a wrong RPATH stops it starting — is invisible
until the AppImage itself runs:

```bash
export APPIMAGE_EXTRACT_AND_RUN=1
xvfb-run -a -s "-screen 0 1920x1080x24" ./Rasterloom-x86_64.AppImage --selftest --junit-xml=results.xml
```

Then a **7-distro matrix against the actual artifact** — `ubuntu:22.04`, `ubuntu:24.04`,
`ubuntu:26.04`, `debian:12`, `fedora:42`, `archlinux:latest`, `opensuse/tumbleweed` — each asserting
exit 0, correct `--version --verbose`, and one byte-exact render. This is the only step that proves
the thing runs on the friend's machine. Plus hard bundling assertions that must fail the job:

```bash
unsquashfs -l Rasterloom-x86_64.AppImage | grep -q libqwayland-generic.so \
  || { echo "linuxdeploy shipped an X11-only bundle"; exit 1; }
unsquashfs -l Rasterloom-x86_64.AppImage | grep -qE 'libGL\.so|libEGL\.so|libdrm\.so' \
  && { echo "driver lib bundled - will break on real GPUs"; exit 1; }
```

Never bundle `libGL`, `libEGL`, `libGLdispatch`, `libGLX`, `libdrm`, `libgbm`, `libX11`, `libxcb`,
`libwayland-client`, `libglib-2.0` or `libfontconfig`. The AppImage excludelist exists for this.

**Job graph, target green-to-green under 15 minutes:** build (6 min cold / 90 s warm with a 2 GB
ccache) → unit 45 s · golden 3 min · mutation 2 min · psd-roundtrip 60 s · gui-smoke 90 s · package
2 min → artifact-selftest 60 s · distro-matrix 3 min. ASan/UBSan and a libFuzzer run on the PSD
parser are nightly, not per-push. Runners give 4 vCPU / 16 GB / 14 GB SSD.

**What cannot be verified — state it out loud rather than papering over it:**

1. **Tablet pressure, tilt and eraser.** No CI runner has a stylus, and neither Xvfb nor headless
   Weston can synthesize tablet-v2 without a virtual device. Mitigation: unit-test the brush against
   a recorded stream of synthetic `QTabletEvent`s injected via `QCoreApplication::sendEvent`, plus
   Help → Input Diagnostics screenshotted by the friend. That is the entire tablet test plan, and it
   is honest.
2. **Vendor GPU drivers.** CI gives llvmpipe only. Mitigation is the mandatory raster fallback plus
   startup renderer logging.
3. **Real input latency.** Runner CPUs are shared and throttled. Assert *throughput* budgets in CI
   ("composite 1000 64×64 RGBA8 tiles in Normal mode in under X ms single-threaded"); measure
   latency only via the in-app HUD on the friend's machine.
4. **Photoshop parity itself.** Asserted against the written spec — W3C, the PSD format
   documentation — never against Adobe output. The parity table documents divergence honestly.
</verification>

<eyes>
You are on Windows with no Linux, but you can read PNGs — so give yourself eyes.

A mandatory CI job launches the real GUI under `xvfb-run`, drives ~25 scripted interactions with
`QTest::mouseClick`/`keyClick`, calls `QWidget::grab()` after each, tiles the results into
`gui-contact-sheet.png` and uploads it. The golden job emits `contact-sheet.png` — all 27 blend modes
tiled and labelled — plus stroke and selection sheets. Failing diffs additionally upload `ref.png`,
`cpp.png` and odiff's `--diff-mask`, so you can see *how* it is wrong rather than only *that* it is.
Cost: about 90 seconds. **After every CI run, download both sheets and look at them.** This is the
closest available substitute for a human.

`gh` is not installed. Use the REST API:

```bash
TOKEN=$(printf 'protocol=https\nhost=github.com\n\n' | git credential fill | sed -n 's/^password=//p')
RUN=$(curl -s -H "Authorization: Bearer $TOKEN" \
  "https://api.github.com/repos/GGRRK/rasterloom/actions/runs?per_page=1" \
  | python -c "import sys,json; print(json.load(sys.stdin)['workflow_runs'][0]['id'])")
URL=$(curl -s -H "Authorization: Bearer $TOKEN" \
  "https://api.github.com/repos/GGRRK/rasterloom/actions/runs/$RUN/artifacts" \
  | python -c "import sys,json; a=json.load(sys.stdin)['artifacts']; print([x for x in a if x['name']=='visual-artifacts'][0]['archive_download_url'])")
curl -sL -H "Authorization: Bearer $TOKEN" -o ci.zip "$URL"
python -m zipfile -e ci.zip ci-out/
```

Then read `ci-out/contact-sheet.png` and `ci-out/gui-contact-sheet.png`. Never mark work done on a
CI run whose sheets you have not opened.
</eyes>

<execution_plan>
1. **Explore agent** — read `EmilDohne/PhotoshopAPI` (its layer-tree API and issue #179), the
   OpenRaster 0.0.6 spec, W3C Compositing-1, and `libmypaint`'s `count_dabs_to()`. Report before
   writing any code. Use the context7 MCP for Qt 6.9 API surfaces rather than recalling them —
   training data on Qt 6 is unreliable at this level of detail.
2. **Main session, sequential** — the three-target CMake split, the tile engine, the CI skeleton and
   the golden harness **before any feature**. The first green CI run must already prove one blend
   mode byte-exact against the NumPy reference. Do not build features on an unproven harness.
3. **Parallel general-purpose agents, disjoint directories only** — (a) blend modes and adjustments,
   (b) selections and transform, (c) brush engine, (d) PSD and `.orp` I/O. Launch in a single
   message so they run concurrently. **One build gate**: never let parallel agents build in the same
   directory, because a file-lock error reads exactly like a compile error and costs an hour.
4. **Main session** — GUI shell, then packaging, then the distro matrix.
5. Run the verification protocol. Download and view the contact sheets. Only then report.

**Suggested model:** HIGH tier for the compositor, brush engine and CI harness — they are the
correctness spine and a subtle error there is invisible for weeks. MEDIUM for GUI wiring and menu
plumbing.
</execution_plan>

<guardrails>
- Touch only the project's own clone and only the project's repository. Every other folder on the
  machine is off limits.
- Never install anything on the Windows machine. No WSL, no Docker, no toolchains.
- Never install, run or probe Adobe Photoshop.
- Pin Qt to 6.9.3 in the workflow AND add a dependabot/renovate ignore rule. Qt 6.10 renamed the
  platform plugins, which makes `linuxdeploy-plugin-qt` **silently** omit the Wayland plugins — CI
  stays green under Xvfb and the app misbehaves only on the friend's machine, unreproducible. A bot
  bumping Qt is a silent-breakage vector. `ubuntu-latest` is banned for the same class of reason:
  pin `ubuntu-24.04`, because `ubuntu-latest` will migrate to 26.04 and shift the host kernel and
  tooling under a build that has been green for months.
- If blocked or missing information, state exactly what is missing and stop. Do not guess through it.
- Never report a step as passing without its command output. A red check is never "basically fine".
</guardrails>

<acceptance_criteria>
- [ ] `librasterloomcore.a` links with zero Qt GUI symbols — assert this in CI, do not assume it.
- [ ] 260+ goldens pass at `odiff --threshold=0` against `refcomp.py`.
- [ ] Every render script produces identical bytes on two consecutive runs.
- [ ] Every id in the mutation registry (currently 43 after the brush lane: 0-42) is detected; the
      loop exits 0.
- [ ] 12/12 PSD round-trips preserve the layer tree; `psd_tools` composites a non-black image.
- [ ] The `.AppImage` self-test passes under `xvfb-run` on all 7 distro containers.
- [ ] Bundling assertions pass: Wayland plugins present, no driver libraries bundled.
- [ ] `contact-sheet.png` and `gui-contact-sheet.png` exist, were downloaded, and were viewed.
- [ ] Artifact <= 350 MB, with `Rasterloom-x86_64.AppImage.sha256` published beside it.
- [ ] `docs/PARITY.md` is generated from `data/parity.yaml`, and no row claims `functional` or
      `parity` without an `evidence` field naming a test that passed in that same CI job.
- [ ] Green-to-green CI under 15 minutes.

**Tester handoff, shipped with the artifact.** Never assume double-click works — GNOME Files does not
execute on double-click by default:

```
chmod +x Rasterloom-x86_64.AppImage
./Rasterloom-x86_64.AppImage
```

Requires glibc >= 2.35 (Ubuntu 22.04+, Debian 12+, Fedora 36+, RHEL 9+, current Arch and openSUSE).
AppRun must detect a lower glibc and print that sentence itself rather than letting `ld.so` emit a
cryptic error. Ask the friend once, before the first delivery: distro and version, `ldd --version`,
X11 or Wayland, GPU vendor and driver, and whether he has a graphics tablet — and if so, whether
`xf86-input-wacom` is installed, because without it X11 cannot distinguish stylus from eraser and
`pointerType()` will never report `Eraser`.
</acceptance_criteria>

<honest_scope>
State this in the README and never inflate it. Roughly 711 Photoshop capabilities are catalogued;
Tier-0 — "if these are missing it is not Photoshop" — is about 100 of them. v0.1 is about 50 rows.

> "Tier-0 coverage 50/100. Strict parity 7% of 711 catalogued capabilities. Weighted
> everyday-workflow coverage 57%. Ceiling is 91% — the remainder is server-side AI,
> patent-encumbered, or proprietary-model features that will never be implemented."

7% strict and ~57% of what a person actually does are both true at once, because layers, blend
modes, masks, selections, the brush and Levels/Curves are where the work happens. Generate the
headline from `data/parity.yaml` and never hand-edit it. The denominator freezes at v1; rows may be
added, never deleted; `wontfix` is the only retirement path. A row with a stubbed implementation
counts as missing.

Phase ladder after v0.1:

| Phase | Contents |
|---|---|
| **0.2 Depth** | Layer styles; 16-bit/channel; full lcms2 ICC pipeline with display transform and soft proof; the remaining 8 adjustment layers; Dodge/Burn/Sponge/Smudge/Blur/Sharpen; angle/reflected/diamond gradients; pattern fill; multithreaded compositor; Blend If and Knockout |
| **0.3 Type & Vector** | FreeType + HarfBuzz + Fontconfig text layers; Pen tool, paths, shape layers, vector masks; PSD `TySh`/`Txt2` best-effort round-trip |
| **0.4 Nondestructive** | Embedded Smart Objects and Smart Filters; OpenImageIO for JXL/AVIF/EXR/WebP/RAW; 32-bit float |
| **0.5 Assist** | Poisson healing (patent-free since 2021); "Smart Fill" via `cv::inpaint` — never called Content-Aware Fill; optional ONNX segmentation only if the artifact stays under 350 MB; Liquify; Lens Correction |
| **0.6 Automation** | Actions and batch; scripting API; a native C ABI plugin interface (never `.8bf`); native-Wayland default once decorations are solved; opt-in GL 4.3 compute with a startup self-test against the CPU path |
| **2027-03+** | Reconsider seam carving when US7477800B2 expires. PatchMatch stays barred to 2031/2033. |
</honest_scope>

---

## Open items that need a human

1. **Trademark clearance for "Rasterloom"** before any public release: USPTO classes 9 and 42,
   EUIPO, UKIPO, plus npm / PyPI / crates.io / Flathub / GitHub / domain checks. It is a coined
   compound, so the risk is low, but the check is 15 minutes and this project has a naming
   trap in its lineage. Fallbacks: Verditer, Scumble, Grisaille.
2. **Confirm Qt-Advanced-Docking-System's LGPL-2.1 → GPL-3.0 compatibility** via LGPLv2.1 §3.
   15 minutes. Fallback is raw `QDockWidget`, which is acceptable because we default to X11 where
   its float-behaviour bugs do not bite.
3. **The friend's environment**, asked once before the first delivery — see `<acceptance_criteria>`.
