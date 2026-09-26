<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Performance and memory

Numbers from `tools/bench.py --cpu 2 --large-repeat 2` (Release build, `-O2 -march=x86-64-v2`,
scalar binary64 math as `docs/math/` requires, one thread). The tool runs every case of
`rasterloom-cli --bench` in a fresh process. Each process builds the document with a setup render
script, resets the kernel's peak-RSS counter through `/proc/self/clear_refs`, then times exactly
one step: an op run through the script registry as the engine runs it (history record included),
or, for the composite case, the full-canvas composite that PNG export performs.

- **RSS before** is the process with the setup document live, including its history. For example,
  the 8000x8000 cases hold the noise layer plus the pre-noise gradient in history (2 x 244 MiB).
- **Peak RSS** is the highest resident set during the timed step.

Machine class: Intel Core Ultra 9 275HX (hybrid: 8 P-cores and 16 E-cores). All runs are pinned
to one P-core, logical CPU 2, so the timings are single-core figures. Unpinned runs on this CPU
vary by up to about 30%, depending on which core type the scheduler picks.

## Timings (v0.1 scalar path, deterministic scheduler)

| case | what | min ms | median ms | runs | RSS before MiB | peak RSS MiB |
|---|---|---:|---:|---:|---:|---:|
| `composite_4000x3000_x10` | full-canvas composite of 10 gradient layers (9 blend modes, opacity 0.8) | 3029 | 3089 | 5 | 503 | 550 |
| `gaussian_r50_4000x3000` | filter_gaussian_blur radius 50 on a 4000x3000 layer | 942 | 943 | 5 | 101 | 238 |
| `brush_1000dabs_size200` | one brush_stroke, 1000 dabs of size 200 (spacing 25%) on 4000x3000 | 292 | 294 | 5 | 6 | 128 |
| `image_size_4000x3000_to_2000x1500` | image_size 4000x3000 -> 2000x1500 | 215 | 216 | 5 | 101 | 114 |
| `gaussian_r50_8000x8000` | filter_gaussian_blur radius 50 on 8000x8000 | 5425 | 5448 | 2 | 504 | 1237 |
| `motion_d30_8000x8000` | filter_motion_blur distance 30, angle 30 on 8000x8000 | 6972 | 8474 | 2 | 504 | 997 |
| `image_size_8000_to_5000` | image_size 8000x8000 -> 5000x5000 | 1593 | 1594 | 2 | 504 | 606 |
| `rotate_15_8000x8000` | rotate_canvas 15 degrees on 8000x8000 | 9795 | 9824 | 2 | 504 | 760 |

Notes:
- The brush case is exactly 1000 dabs. `tests/unit/infra/test_bench.cpp` replays its samples
  through `StrokeSession` and asserts `dab_count() == 1000`: size 200, default spacing 25% (50 px),
  along a 49,950 px path.
- The composite is about 25 ns per pixel per layer, all in `composite_px`'s binary64 path. The
  BUILD-SPEC's allowed Highway path for Normal mode (with a scalar byte-equality test) is the
  obvious next step, followed by threads behind `ICompositeScheduler` in 0.2. Interactive
  painting does not recomposite the full canvas: the GUI re-renders only dirty tiles, through
  the same scheduler.

## Large-image memory (8000x8000, peak RSS of the step)

Before = the lane's tile tiers without the item-4 changes. After = this tree. Same binary flags,
same machine, both pinned to CPU 2. Every result is byte-identical: goldens 334/334,
`tests/unit/infra/test_large_image.cpp` compares against the previous whole-canvas algorithms on
awkward sizes, and the mutation gate is 40/40.

| step | before peak MiB | after peak MiB | before ms | after ms | what changed |
|---|---:|---:|---:|---:|---|
| gaussian r=50 | 1969 | 1237 | 5750 | 5425 | one uint16 plane per channel (values <= 65025) instead of four int32 planes; the dense filter input is released before the result's tiles are allocated |
| motion blur d=30 | 2945 | 997 | 7761 | 6972 | premultiplied doubles only for the ring of source rows the taps reach, not a 4-doubles-per-pixel plane; each tap's row is resolved once per output row |
| image_size 8000->5000 | 3922 | 606 | 1942 | 1593 | streamed separable resample: rows are read from tiles, horizontally resampled once, kept only while the vertical window needs them, and written out in 64-row bands |
| rotate 15 degrees | 1368 | 760 | 9158 | 9795 | the warp reads straight from tiles (TileGrid) and writes 64-row bands into tiles; no dense source or destination copy |

The remaining peak is the document itself (about 504 MiB before the step), plus the result's tiles
(rotate: 9798x9798 = 366 MiB), plus, for the filters, the dense input and output images of the
B0 pipeline (2 x 244 MiB), which lock-alpha and coverage need. Rotate is about 7% slower
because each bicubic tap costs one extra pointer load through the tile grid. That was traded for
600 MiB.

## Memory tiers (BUILD-SPEC requirement 2)

`src/core/tile/memory.hpp` implements the tiers. This is how the spec's numbers are read:

- **Hard limit, 50% of RAM** (or of the cgroup v2 `memory.max`, if that is lower). The limit counts
  hot tile bytes, compressed-in-RAM tile bytes and explicit reservations: the large dense work
  buffers of filters. An allocation that would exceed it throws `mem::MemoryError`, which is a
  clean error: `rasterloom-cli` exits with code 5 and writes no PNG, and the GUI's op runner rolls
  back. The process is never left to the OOM killer. Untracked small allocations (JSON, stroke
  state) are not counted.
- **Soft limit, 2% of RAM**: the budget for **hot** (uncompressed) tiles. At every op boundary
  (`Document::push_history` calls `mem::safe_point`), the least-recently-used hot tiles are
  LZ4-compressed until hot bytes are at or below the soft limit. During an op the working set is
  never evicted.
- **Warm budget, another 2% of RAM**, for compressed tiles kept in RAM. Beyond it, the LRU
  compressed tiles spill to an `O_TMPFILE` in `$XDG_CACHE_HOME/rasterloom` (fallback
  `~/.cache/rasterloom`; `mkostemp` plus `unlink` where `O_TMPFILE` is unsupported), up to the
  **4096 MiB scratch cap**. Freed extents are reused.
- Reads fault tiles back in transparently, and the compressed copy is kept until the tile is
  written, so re-evicting a clean tile is free. Copy-on-write sharing is per cell, so a history
  snapshot and the live document share one compressed copy. The shared empty tile is a pinned cell
  that is never evicted, so mutation 13 is still caught.
- `RASTERLOOM_MEM_TEST=1` sets soft = warm = 0: every op boundary compresses and spills every tile.
  CTest runs the embedded self-test (`selftest_memtest`) and the full CLI-vs-reference golden suite
  (`goldens_memtest`) this way, and both are byte-identical.
- Overrides: `RASTERLOOM_MEM_HARD_MB`, `RASTERLOOM_MEM_SOFT_MB`, `RASTERLOOM_MEM_WARM_MB`,
  `RASTERLOOM_SCRATCH_CAP_MB`, `RASTERLOOM_SCRATCH_DIR`.

## Embedded self-test size

`rasterloom --selftest` embeds the frozen corpus in both binaries (`tests/goldens`, from
`tools/freeze_goldens.py`): 334 stripped scripts plus 287 reference PNGs, 12.9 MB. Unstripped
Release sizes grow as follows:

| binary | before | after | growth |
|---|---:|---:|---:|
| `rasterloom` | 2,144,624 | 15,344,728 | +13.2 MB |
| `rasterloom-cli` | 1,501,352 | 14,753,256 | +13.3 MB |

Of the PNG bytes, 7 MB are 27 incompressible 256x256 noise goldens (`compositing/blend_*_rnd`).
If the AppImage size ever matters more than keeping the expected images inside the binary, those
cases could embed a SHA-256 of the decoded RGBA instead.

## Reproducing

```sh
cmake --build build
python3 tools/bench.py --cpu 2 --large-repeat 2      # full table
./build/src/rasterloom-cli --bench --bench-case rotate_15_8000x8000   # one case
./build/src/rasterloom-cli --bench-list
```
