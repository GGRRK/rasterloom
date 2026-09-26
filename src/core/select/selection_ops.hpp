// SPDX-License-Identifier: GPL-3.0-or-later
//
// Selection operations (docs/math/30-geometry-selection.md §1.3-§8). The storage is rl::Selection
// (core/doc/selection.hpp): a canvas-sized byte mask S plus the Reselect memory; an all-zero mask
// is "no selection" (§2).
//
// Mutation hooks owned here: 15 (region 4-connectivity), 32 (feather sigma = r), 33 (nonzero
// winding polygon fill), 34 (subtract as min(S, 255 - B)), 35 (colour distance normalised by 255).
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "core/doc/selection.hpp"
#include "core/tile/tiled_image.hpp"

namespace rl::select {

// §4 boolean combine modes.
enum class Mode { New, Add, Subtract, Intersect };

// "new" / "add" / "subtract" / "intersect"; returns false on any other spelling.
bool parse_mode(const std::string& s, Mode& out);

struct Pt {
    double x = 0.0;
    double y = 0.0;
};

// ---- candidate masks B (§1.3, §3): row-major W*H bytes, 0 outside the shape ----------------------
// `w`, `h` must be > 0 (the caller validates). Anti-aliased: 16x16 supersampling, S = q(n / 256);
// otherwise the single pixel-centre sample.
std::vector<uint8_t> rasterize_rect(int W, int H, double x, double y, double w, double h, bool aa);
std::vector<uint8_t> rasterize_ellipse(int W, int H, double x, double y, double w, double h, bool aa);
// Even-odd PNPOLY (§3.3); mutation 33 switches to nonzero winding. Fewer than 3 points: all 0.
std::vector<uint8_t> rasterize_polygon(int W, int H, const std::vector<Pt>& pts, bool aa);

// §6.3 region (shared by select_wand and bucket_fill). `px` is the layer's row-major pixels.
// Seed outside the canvas: all 0. Mutation 15: 4-connected component; mutation 35: match test
// (d / 255.0) <= t.
std::vector<uint8_t> region(const std::vector<Rgba8>& px, int W, int H, int64_t sx, int64_t sy, int t,
                            bool contiguous, bool aa);

// ---- state changes on a Selection whose mask is W x H ----------------------------------------------
// §4: S' = combine(S, B) by mode (mutation 34 on subtract).
void combine(Selection& sel, const std::vector<uint8_t>& B, Mode mode);
// §5.
void select_all(Selection& sel);
void deselect(Selection& sel);
void reselect(Selection& sel);
void select_inverse(Selection& sel);
// §7, r in [0, 250]. Mutation 32: sigma = r.
void feather(Selection& sel, double r);
// §8, n in [1, 100].
void expand(Selection& sel, int n);
void contract(Selection& sel, int n);

// §7 box_widths(sigma): three odd box widths (exposed for tests).
std::vector<int64_t> box_widths(double sigma);

// Row-major copy of the mask and its write-back (keeps the tiled mask sparse).
std::vector<uint8_t> mask_dense(const Selection& sel);
void set_mask_dense(Selection& sel, const std::vector<uint8_t>& m);

// ---- GUI helpers: marching ants -------------------------------------------------------------------
// A pixel counts as selected when its coverage byte is >= `threshold` (the display convention; the
// soft edge itself is not drawn). Out-of-canvas pixels are unselected.

// Closed boundary loops of the selected region along pixel edges, in canvas pixel-corner
// coordinates (integers stored as doubles). Each loop is a closed polyline without its closing
// vertex repeated; collinear runs are merged, so a w x h rectangle is exactly 4 vertices. Loops
// are oriented with the selected side on the right in y-down screen space (clockwise outer
// boundaries, counter-clockwise holes), in the order a row-major pixel scan discovers them.
// Regions that touch only at a pixel corner get separate loops.
std::vector<std::vector<Pt>> outline_polylines(const GrayImage& mask, uint8_t threshold = 128);

// 1-bit edge mask (0 / 255): a selected pixel with at least one 4-neighbour that is unselected or
// outside the canvas.
GrayImage edge_mask(const GrayImage& mask, uint8_t threshold = 128);

}  // namespace rl::select
