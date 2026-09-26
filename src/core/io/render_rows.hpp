// SPDX-License-Identifier: GPL-3.0-or-later
//
// Streams the document composite RENDER(root.children, start) row by row (start = the canvas
// background when `onto_bg`, else transparent), rendering one 64-row band of tiles at a time so a
// wide canvas never needs a full-canvas buffer.
#pragma once

#include <functional>
#include <vector>

#include "core/doc/document.hpp"

namespace rl::io {

// Pull interface: row(y) renders the band containing y on demand (cheapest when y increases).
class BandRenderer {
public:
    BandRenderer(const DocState& s, bool onto_bg);
    const Rgba8* row(int y);

private:
    const DocState& s_;
    bool onto_bg_;
    int band_ty_ = -1;
    std::vector<Rgba8> band_;
};

// Push interface: fn(y, row) for y = 0..h-1.
void for_each_render_row(const DocState& s, bool onto_bg, const std::function<void(int y, const Rgba8* row)>& fn);

}  // namespace rl::io
