// SPDX-License-Identifier: GPL-3.0-or-later
//
// Getting pixels in and out of a document (docs/math/60-editing-ops.md §14): COPY and its crop
// (what the GUI puts on the clipboard), CLEAR, and the pixel half of place_image. The render-script
// ops place_image / layer_via_copy / clear (place_ops.cpp) and the GUI's Cut / Copy / Copy Merged /
// Paste / Paste in Place / drag-and-drop all go through these functions, so the clipboard path
// and the scripted path cannot drift apart.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/doc/document.hpp"
#include "core/io/png.hpp"
#include "core/script/registry.hpp"

namespace rl::edit {

// §14.4: X = COPY(source) over the whole canvas (row-major, W * H). `layer` is a raster node of
// `s` (its own pixels), or nullptr for the merged composite (RENDER over bg, the png8 output).
std::vector<Rgba8> copy_pixels(const DocState& s, const Node* layer);

// §14.4.1: X cropped to its content box and the box's canvas position.
struct Copied {
    int x = 0;
    int y = 0;
    io::RgbaBuffer pixels;
};
// nullopt when every alpha byte of X is 0 ("the selected area is empty").
std::optional<Copied> crop_copied(const std::vector<Rgba8>& X, int W, int H);
// Both steps. `layer_id` empty = merged. Throws rl::ScriptError for an id that is not a raster layer.
std::optional<Copied> copy_region(const DocState& s, const std::string& layer_id);

// §14.6: CLEAR(L) with the document's effective selection; a no-op on a lock_alpha layer.
void clear_pixels(Node& layer, const Selection& sel);

// §14.3: floor_div(n, 2), rounding toward negative infinity.
int64_t floor_half(int64_t n);

// Registers place_image, layer_via_copy and clear (called from registerEditingOps: doc 60 owns them).
void register_place_ops(script::OpRegistry& r);

}  // namespace rl::edit
