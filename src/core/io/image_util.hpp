// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tile-aware helpers shared by the codecs: writing a rectangle of decoded pixels into a sparse
// canvas-sized image, finding the bounding box of non-background content, and downscaling a
// composite for thumbnails.
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "core/io/png.hpp"
#include "core/tile/tiled_image.hpp"

namespace rl::io {

struct IRect {
    int x = 0, y = 0, w = 0, h = 0;
    bool empty() const { return w <= 0 || h <= 0; }
};

// Writes f(x, y) for every canvas pixel of the rectangle (x0, y0, w, h) (clipped to the canvas;
// content outside the canvas is discarded, 00-conventions C8a). Tiles that end up uniform
// background are not stored. f is called in row-major order within each tile.
template <class Px, class F>
void write_region(TiledImage<Px>& img, int64_t x0, int64_t y0, int64_t w, int64_t h, F&& f) {
    const int64_t cx0 = std::max<int64_t>(x0, 0), cy0 = std::max<int64_t>(y0, 0);
    const int64_t cx1 = std::min<int64_t>(x0 + w, img.width()), cy1 = std::min<int64_t>(y0 + h, img.height());
    if (cx0 >= cx1 || cy0 >= cy1) return;
    const int tx0 = static_cast<int>(cx0 / kTileSize), tx1 = static_cast<int>((cx1 - 1) / kTileSize);
    const int ty0 = static_cast<int>(cy0 / kTileSize), ty1 = static_cast<int>((cy1 - 1) / kTileSize);
    for (int ty = ty0; ty <= ty1; ++ty) {
        for (int tx = tx0; tx <= tx1; ++tx) {
            Tile<Px> t = img.tile(tx, ty);
            const int64_t bx = static_cast<int64_t>(tx) * kTileSize, by = static_cast<int64_t>(ty) * kTileSize;
            const int lx0 = static_cast<int>(std::max<int64_t>(cx0 - bx, 0));
            const int lx1 = static_cast<int>(std::min<int64_t>(cx1 - bx, kTileSize));
            const int ly0 = static_cast<int>(std::max<int64_t>(cy0 - by, 0));
            const int ly1 = static_cast<int>(std::min<int64_t>(cy1 - by, kTileSize));
            for (int ly = ly0; ly < ly1; ++ly)
                for (int lx = lx0; lx < lx1; ++lx)
                    t.at(lx, ly) = f(static_cast<int>(bx + lx), static_cast<int>(by + ly));
            img.put_tile_sparse(tx, ty, t);
        }
    }
}

// Bounding box of the canvas pixels that differ from the image's background (empty rect when
// none).
template <class Px>
IRect content_bbox(const TiledImage<Px>& img) {
    int x0 = img.width(), y0 = img.height(), x1 = -1, y1 = -1;
    const Px bg = img.background();
    auto same = [](const Px& a, const Px& b) { return detail::px_bits(a) == detail::px_bits(b); };
    for (const TileKey& k : img.sorted_keys()) {
        const Tile<Px>& t = img.tile(k.tx, k.ty);
        const int bx = k.tx * kTileSize, by = k.ty * kTileSize;
        for (int ly = 0; ly < kTileSize; ++ly) {
            const int y = by + ly;
            if (y >= img.height()) break;
            for (int lx = 0; lx < kTileSize; ++lx) {
                const int x = bx + lx;
                if (x >= img.width()) break;
                if (same(t.at(lx, ly), bg)) continue;
                x0 = std::min(x0, x);
                x1 = std::max(x1, x);
                y0 = std::min(y0, y);
                y1 = std::max(y1, y);
            }
        }
    }
    if (x1 < 0) return IRect{};
    return IRect{x0, y0, x1 - x0 + 1, y1 - y0 + 1};
}

// Copies a canvas rectangle out of a tiled image (row-major). Pixels outside the canvas read as
// Px{} (C4).
template <class Px>
std::vector<Px> read_region(const TiledImage<Px>& img, const IRect& r) {
    std::vector<Px> out(static_cast<size_t>(std::max(r.w, 0)) * static_cast<size_t>(std::max(r.h, 0)));
    for (int y = 0; y < r.h; ++y)
        for (int x = 0; x < r.w; ++x) out[(static_cast<size_t>(y) * static_cast<size_t>(r.w)) + static_cast<size_t>(x)] = img.get(r.x + x, r.y + y);
    return out;
}

// Streaming area-average downscaler: feed the source rows in order 0..h-1, then finish(). Output
// size: the longer side at most `max_side` (never upscales; aspect preserved, each side >= 1).
// Averaging is premultiplied by alpha, colour is divided by the summed alpha and both are
// quantised with q (00-conventions C2/C3).
class ThumbAccumulator {
public:
    ThumbAccumulator(int w, int h, int max_side);
    void add_row(int y, const Rgba8* row);
    RgbaBuffer finish() const;
    int out_w() const { return tw_; }
    int out_h() const { return th_; }

private:
    int w_, h_, tw_, th_;
    std::vector<int> ox_of_x_, oy_of_y_;
    std::vector<int> nx_, ny_;
    std::vector<double> acc_;  // tw*th*4: sum(r*a), sum(g*a), sum(b*a), sum(a)
};

// Area-average downscale so that the longer side is at most `max_side` (never upscales; aspect
// preserved, each side at least 1). Averaging is premultiplied by alpha, colour is divided by the
// averaged alpha and both are quantised with q (00-conventions C2/C3).
RgbaBuffer downscale_fit(const RgbaBuffer& src, int max_side);

// Composites straight-alpha pixels over opaque white: c' = q(c/255 * a/255 + (1 - a/255)), alpha
// becomes 255. Used where a format has no alpha (JPEG) or stores a white-matted composite (PSD
// merged image, see core/psd/psd.hpp).
Rgba8 matte_white(Rgba8 p);

}  // namespace rl::io
