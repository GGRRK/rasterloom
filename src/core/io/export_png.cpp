// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/io/export_png.hpp"

#include <algorithm>
#include <vector>

#include "core/composite/render.hpp"

namespace rl::io {

namespace {

// Renders tile row `ty` into `band` (w * 64 pixels, row-major).
void render_band(const DocState& s, int ty, std::vector<Rgba8>& band) {
    RgbaTile t;
    const int tiles_x = (s.w + kTileSize - 1) / kTileSize;
    for (int tx = 0; tx < tiles_x; ++tx) {
        composite::render_tile(s, tx, ty, /*onto_bg=*/true, t);
        const int x0 = tx * kTileSize;
        const int nx = std::min(kTileSize, s.w - x0);
        for (int ly = 0; ly < kTileSize; ++ly)
            for (int lx = 0; lx < nx; ++lx)
                band[(static_cast<size_t>(ly) * static_cast<size_t>(s.w)) + static_cast<size_t>(x0 + lx)] =
                    t.at(lx, ly);
    }
}

}  // namespace

void write_document_png(const DocState& s, const std::string& path) {
    std::vector<Rgba8> band(static_cast<size_t>(s.w) * kTileSize);
    int band_ty = -1;
    write_png_rows(path, s.w, s.h, [&](int y) {
        const int ty = y / kTileSize;
        if (ty != band_ty) {
            render_band(s, ty, band);
            band_ty = ty;
        }
        return band.data() + (static_cast<size_t>(y % kTileSize) * static_cast<size_t>(s.w));
    });
}

RgbaBuffer render_document(const DocState& s) {
    RgbaBuffer out;
    out.w = s.w;
    out.h = s.h;
    out.px.resize(static_cast<size_t>(s.w) * static_cast<size_t>(s.h));
    std::vector<Rgba8> band(static_cast<size_t>(s.w) * kTileSize);
    const int tiles_y = (s.h + kTileSize - 1) / kTileSize;
    for (int ty = 0; ty < tiles_y; ++ty) {
        render_band(s, ty, band);
        for (int ly = 0; ly < kTileSize; ++ly) {
            const int y = (ty * kTileSize) + ly;
            if (y >= s.h) break;
            std::copy(band.begin() + (static_cast<std::ptrdiff_t>(ly) * s.w),
                      band.begin() + (static_cast<std::ptrdiff_t>(ly + 1) * s.w),
                      out.px.begin() + (static_cast<std::ptrdiff_t>(y) * s.w));
        }
    }
    return out;
}

}  // namespace rl::io
