// SPDX-License-Identifier: GPL-3.0-or-later
//
// Rendering a layer tree (docs/math/10-compositing.md §5-§8): RENDER, LONE, clip groups (§6, both
// clbl paths), groups (§7, pass-through and isolated as two separate code paths per BUILD-SPEC D4),
// masks (§8).
//
// Every formula in doc 10 is per pixel with no neighbour access, so RENDER is evaluated one 64x64
// tile at a time: the whole tree is run on tile (tx, ty) before moving to the next tile, tiles in
// row-major order (C4). Every intermediate buffer is 8-bit (C5). Pixels outside the canvas in edge
// tiles are never computed and stay (0, 0, 0, 0).
//
// Mutation hooks owned here: 4 (pass-through as isolated), 5 (ci*S inside G), 6 (clip-base fill as
// opacity), 19 (pass-through opacity per child), 20 (clbl ignored), 23 (clip shape without mask).
//
// Scheduling (BUILD-SPEC D8 note): every tile render goes through an ICompositeScheduler. v0.1 ships
// only DeterministicScheduler (single thread, tiles in row-major order); the interface is shaped
// for 0.2's threaded implementation: a scheduler may compute tiles in any order on any threads,
// but it must hand the finished tiles to the sink on the calling thread in row-major order, so
// the output never depends on the thread count. `--deterministic` (both binaries) calls
// force_deterministic(), which pins DeterministicScheduler for the process lifetime.
#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "core/doc/document.hpp"
#include "core/tile/tiled_image.hpp"

namespace rl::composite {

// The part of the canvas a tile covers.
struct TileCtx {
    int tx = 0;
    int ty = 0;
    int x0 = 0;  // canvas x of the tile's left column
    int y0 = 0;
    int nx = 0;  // in-canvas columns (1..64)
    int ny = 0;

    static TileCtx make(int tx, int ty, int canvas_w, int canvas_h);
};

// Fills the in-canvas pixels of `buf` with `v` and the rest with (0, 0, 0, 0).
void fill_tile(RgbaTile& buf, const TileCtx& t, Rgba8 v);

// RENDER(children, D) on one tile: composites `children` (index 0 = bottom) onto `D` in place.
void render_children(const std::vector<Node>& children, const TileCtx& t, RgbaTile& D);

// §6.1 steps 1-2 on one tile: builds the clip-group interior buffer G for `base` and its visible
// clipped layers `vis` (bottom to top). Used by the clbl=true path and by merge_down (§9.1).
void build_clip_interior(const Node& base, const std::vector<const Node*>& vis, const TileCtx& t, RgbaTile& G);

// §6 base shape S = n(A0) * m0 for one tile pixel index `i` (mutation 23 drops m0).
double clip_shape(const Node& base, const RgbaTile& base_px, const GrayTile* base_mask, int i);

// §4.3 layer mask factor source for a node on a tile: the mask tile when the node has an enabled
// mask, else nullptr (factor 1.0).
const GrayTile* enabled_mask_tile(const Node& n, const TileCtx& t);
inline double mask_factor(const GrayTile* mt, int i) {
    return mt ? static_cast<double>(mt->px[static_cast<size_t>(i)]) / 255.0 : 1.0;
}

// The per-tile kernel: renders tile (tx, ty) of RENDER(root.children, start) where start is
// canonicalise(bg) when `onto_bg`, else transparent. Pure function of (s, tx, ty, onto_bg); safe to
// call concurrently for different output buffers. Schedulers call this; everything else calls
// render_tile / render_tiles.
void render_tile_kernel(const DocState& s, int tx, int ty, bool onto_bg, RgbaTile& out);

// Receives finished tiles, on the thread that called render(), in row-major order.
using TileSink = std::function<void(int tx, int ty, const RgbaTile& tile)>;

class ICompositeScheduler {
public:
    virtual ~ICompositeScheduler() = default;
    virtual const char* name() const = 0;
    // Worker threads used (1 = the calling thread only).
    virtual int concurrency() const = 0;
    // Renders every key of `keys` (sorted row-major, no duplicates, all inside the canvas) and
    // passes each finished tile to `sink` in that order, on the calling thread.
    virtual void render(const DocState& s, const std::vector<TileKey>& keys, bool onto_bg, const TileSink& sink) = 0;
};

// Single-threaded, row-major: the v0.1 compositor and the --deterministic path forever.
class DeterministicScheduler final : public ICompositeScheduler {
public:
    const char* name() const override { return "deterministic"; }
    int concurrency() const override { return 1; }
    void render(const DocState& s, const std::vector<TileKey>& keys, bool onto_bg, const TileSink& sink) override;
};

// The process-wide scheduler (DeterministicScheduler unless set_scheduler installed another and
// --deterministic was not given).
ICompositeScheduler& scheduler();
// Installs a scheduler (nullptr = back to the default). Ignored while force_deterministic() is on.
void set_scheduler(std::unique_ptr<ICompositeScheduler> sched);
// Pins DeterministicScheduler for the rest of the process (the --deterministic flag).
void force_deterministic();
bool deterministic_forced();

// Renders `keys` through scheduler() (sorted into row-major order and de-duplicated; keys outside
// the canvas are dropped) - the API for batches of dirty tiles.
void render_tiles(const DocState& s, std::vector<TileKey> keys, bool onto_bg, const TileSink& sink);

// One tile through scheduler() (what the GUI's display cache uses for a dirty tile).
void render_tile(const DocState& s, int tx, int ty, bool onto_bg, RgbaTile& out);

// The whole canvas, tile by tile in row-major order. The result is sparse (all-transparent tiles
// are not stored).
RgbaImage render_image(const DocState& s, bool onto_bg);

}  // namespace rl::composite
