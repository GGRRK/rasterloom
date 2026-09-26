// SPDX-License-Identifier: GPL-3.0-or-later
//
// Dense <-> tiled conversion and whole-document visitors for the geometry lane
// (docs/math/30-geometry-selection.md). Canvas-geometry ops, transforms and selection ops work on
// dense row-major buffers (index = y * W + x, C4) and write the result back sparsely.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "core/doc/node.hpp"
#include "core/tile/memory.hpp"
#include "core/tile/tiled_image.hpp"

namespace rl::geom {

inline size_t idx(int x, int y, int w) {
    return (static_cast<size_t>(y) * static_cast<size_t>(w)) + static_cast<size_t>(x);
}

// Bytes of a dense w x h buffer at `bytes_per_px` (64-bit: 16384 x 16384 x 8 does not overflow).
inline uint64_t dense_bytes(int64_t w, int64_t h, uint64_t bytes_per_px) {
    return static_cast<uint64_t>(std::max<int64_t>(w, 0)) * static_cast<uint64_t>(std::max<int64_t>(h, 0)) *
           bytes_per_px;
}

// Dense work buffers are not tiles, so the tile tiers do not see them. Every op that allocates one
// first charges it to the hard limit with a mem::Reservation held for the buffer's lifetime
// (BUILD-SPEC requirement 2): an op too large for the limit fails with mem::MemoryError before it
// allocates or changes anything, instead of running into the kernel's OOM killer.
inline mem::Reservation reserve_dense(uint64_t bytes, const char* what) { return mem::Reservation(bytes, what); }

// Row-major copy of every pixel of `img` (absent tiles read as the image background).
template <class Px>
std::vector<Px> to_dense(const TiledImage<Px>& img) {
    const int W = img.width(), H = img.height();
    std::vector<Px> out(static_cast<size_t>(W) * static_cast<size_t>(H));
    for (int ty = 0; ty < img.tiles_y(); ++ty) {
        for (int tx = 0; tx < img.tiles_x(); ++tx) {
            const Tile<Px>& t = img.tile(tx, ty);
            const int x0 = tx * kTileSize, y0 = ty * kTileSize;
            const int xn = std::min(kTileSize, W - x0), yn = std::min(kTileSize, H - y0);
            for (int ly = 0; ly < yn; ++ly) {
                const Px* src = &t.at(0, ly);
                std::copy(src, src + xn, out.begin() + static_cast<std::ptrdiff_t>(idx(x0, y0 + ly, W)));
            }
        }
    }
    return out;
}

// Builds a W x H tiled image from a row-major buffer. Tiles equal to `bg` are left absent.
template <class Px>
TiledImage<Px> from_dense(const std::vector<Px>& d, int W, int H, Px bg = Px{}) {
    TiledImage<Px> img(W, H, bg);
    Tile<Px> t;
    for (int ty = 0; ty < img.tiles_y(); ++ty) {
        for (int tx = 0; tx < img.tiles_x(); ++tx) {
            t.px.fill(bg);
            const int x0 = tx * kTileSize, y0 = ty * kTileSize;
            const int xn = std::min(kTileSize, W - x0), yn = std::min(kTileSize, H - y0);
            for (int ly = 0; ly < yn; ++ly) {
                const Px* src = &d[idx(x0, y0 + ly, W)];
                std::copy(src, src + xn, &t.at(0, ly));
            }
            img.put_tile_sparse(tx, ty, t);
        }
    }
    return img;
}

// Read-only random access to a tiled image without a dense copy: one tile pointer per grid cell
// (absent cells point at the shared uniform background tile). Valid until the next mem::safe_point,
// i.e. for the duration of one op.
template <class Px>
class TileGrid {
public:
    explicit TileGrid(const TiledImage<Px>& img)
        : w_(img.width()), h_(img.height()), nx_(img.tiles_x()), tiles_(static_cast<size_t>(img.total_tiles())) {
        for (int ty = 0; ty < img.tiles_y(); ++ty)
            for (int tx = 0; tx < nx_; ++tx) tiles_[static_cast<size_t>((ty * nx_) + tx)] = &img.tile(tx, ty);
    }
    int width() const { return w_; }
    int height() const { return h_; }
    // (x, y) must be inside the image.
    const Px& at(int x, int y) const {
        static_assert(kTileSize == 64, "shift/mask indexing assumes 64-pixel tiles");
        const auto ux = static_cast<unsigned>(x), uy = static_cast<unsigned>(y);
        return tiles_[(static_cast<size_t>(uy >> 6) * static_cast<size_t>(nx_)) + (ux >> 6)]
            ->px[((uy & 63u) << 6) | (ux & 63u)];
    }
    // Copies row y (w() pixels) to `dst`.
    void row(int y, Px* dst) const {
        const int ly = y % kTileSize;
        const size_t base = static_cast<size_t>((y / kTileSize) * nx_);
        for (int tx = 0; tx < nx_; ++tx) {
            const int x0 = tx * kTileSize, xn = std::min(kTileSize, w_ - x0);
            const Px* src = &tiles_[base + static_cast<size_t>(tx)]->at(0, ly);
            std::copy(src, src + xn, dst + x0);
        }
    }

private:
    int w_, h_, nx_;
    std::vector<const Tile<Px>*> tiles_;
};

// Builds a W x H tiled image from rows produced in order: fill(y0, y1, band) writes rows [y0, y1)
// into `band` (row-major, W per row). Only one 64-row band is ever held, never a dense canvas;
// tiles equal to `bg` stay absent, exactly as from_dense.
template <class Px, class BandFn>
TiledImage<Px> from_bands(int W, int H, Px bg, BandFn&& fill) {
    TiledImage<Px> img(W, H, bg);
    std::vector<Px> band(static_cast<size_t>(W) * kTileSize);
    Tile<Px> t;
    for (int ty = 0; ty < img.tiles_y(); ++ty) {
        const int y0 = ty * kTileSize, yn = std::min(kTileSize, H - y0);
        fill(y0, y0 + yn, band.data());
        for (int tx = 0; tx < img.tiles_x(); ++tx) {
            t.px.fill(bg);
            const int x0 = tx * kTileSize, xn = std::min(kTileSize, W - x0);
            for (int ly = 0; ly < yn; ++ly) {
                const Px* src = &band[idx(x0, ly, W)];
                std::copy(src, src + xn, &t.at(0, ly));
            }
            img.put_tile_sparse(tx, ty, t);
        }
    }
    return img;
}

// Calls `raster(node)` for every raster layer and `mask(node)` for every node that has a layer
// mask, depth first through every group (hidden nodes included). Doc 30 §13-§16: canvas-geometry
// ops apply to "every raster layer and every layer mask".
inline void for_each_layer_and_mask(Node& n, const std::function<void(Node&)>& raster,
                                    const std::function<void(Node&)>& mask) {
    for (Node& c : n.children) {
        if (c.is_raster()) raster(c);
        if (c.mask) mask(c);
        if (c.is_group()) for_each_layer_and_mask(c, raster, mask);
    }
}

}  // namespace rl::geom
