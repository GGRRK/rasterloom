// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/io/render_rows.hpp"

#include <algorithm>

#include "core/composite/render.hpp"

namespace rl::io {

BandRenderer::BandRenderer(const DocState& s, bool onto_bg)
    : s_(s), onto_bg_(onto_bg), band_(static_cast<size_t>(s.w) * kTileSize) {}

const Rgba8* BandRenderer::row(int y) {
    const int ty = y / kTileSize;
    const size_t w = static_cast<size_t>(s_.w);
    if (ty != band_ty_) {
        RgbaTile t;
        const int tiles_x = (s_.w + kTileSize - 1) / kTileSize;
        for (int tx = 0; tx < tiles_x; ++tx) {
            composite::render_tile(s_, tx, ty, onto_bg_, t);
            const int x0 = tx * kTileSize;
            const int nx = std::min(kTileSize, s_.w - x0);
            for (int ly = 0; ly < kTileSize; ++ly)
                std::copy(t.px.begin() + (ly * kTileSize), t.px.begin() + (ly * kTileSize) + nx,
                          band_.begin() + static_cast<std::ptrdiff_t>((static_cast<size_t>(ly) * w) + static_cast<size_t>(x0)));
        }
        band_ty_ = ty;
    }
    return band_.data() + (static_cast<size_t>(y % kTileSize) * w);
}

void for_each_render_row(const DocState& s, bool onto_bg, const std::function<void(int y, const Rgba8* row)>& fn) {
    BandRenderer r(s, onto_bg);
    for (int y = 0; y < s.h; ++y) fn(y, r.row(y));
}

}  // namespace rl::io
