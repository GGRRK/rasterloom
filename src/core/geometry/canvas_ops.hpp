// SPDX-License-Identifier: GPL-3.0-or-later
//
// Image-menu canvas geometry (docs/math/30-geometry-selection.md §13 Image Size, §14 Canvas Size and
// Crop, §15 Rotate Canvas, §16 Flip) and the one-layer ops (§12 transform, §16 flip with a layer).
//
// Canvas-geometry ops resample or index-map EVERY raster layer and EVERY layer mask (hidden nodes
// and nodes inside groups included), set the new canvas size and clear the selection and its
// Reselect memory (§2). Callers validate ranges first; these functions assume valid input except
// where they return an error string.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "core/doc/document.hpp"
#include "core/transform/transform.hpp"

namespace rl::geom {

enum class Anchor { TL, T, TR, L, C, R, BL, B, BR };
bool parse_anchor(const std::string& s, Anchor& out);

// §13, w2/h2 in [1, 16384].
void image_size(DocState& d, int w2, int h2, transform::Interp interp);

// §14 crop to (cx, cy, cw, ch), cw/ch in [1, 16384]; the rectangle may extend past the canvas.
void crop(DocState& d, int64_t cx, int64_t cy, int cw, int ch);

// §14 canvas size to w2 x h2 with the anchor (floor division for odd deltas).
void canvas_size(DocState& d, int w2, int h2, Anchor anchor);

// §15. Returns the new canvas size without changing anything; nullopt when a side would exceed
// 16384 (an invalid script). angle in [-3600, 3600].
struct Size {
    int w = 0, h = 0;
};
std::optional<Size> rotate_canvas_size(int W, int H, double angle);
// Applies §15. Throws nothing; call rotate_canvas_size first to validate.
void rotate_canvas(DocState& d, double angle);

// §16 without a layer: every raster layer and mask, selection cleared.
void flip_canvas(DocState& d, bool horizontal);
// §16 with a layer: that raster layer's pixels only.
void flip_layer(Node& raster, bool horizontal);

// §12: transform one raster layer's pixels by forward matrix `m` (mask and selection unchanged).
// Returns false when `m` is singular (|det| < 1e-12), leaving the layer untouched.
bool transform_layer(Node& raster, const transform::Mat3& m, transform::Interp interp);

}  // namespace rl::geom
