// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/composite/fills.hpp"

#include <algorithm>

#include "core/base/quant.hpp"
#include "core/base/rng.hpp"

namespace rl::composite {

namespace {

// Visits every tile that intersects rect ∩ canvas, handing the tile's in-rect pixel range to `f`
// as (tile, lx0, lx1, ly0, ly1) with half-open local bounds. The tile starts as the image's current
// content and is stored sparsely afterwards.
template <class Px, class F>
void fill_rect_tiles(TiledImage<Px>& img, const script::Rect& r, F&& f) {
    const int64_t x0 = std::max<int64_t>(r.x, 0);
    const int64_t y0 = std::max<int64_t>(r.y, 0);
    const int64_t x1 = std::min<int64_t>(r.x + r.w, img.width());  // exclusive
    const int64_t y1 = std::min<int64_t>(r.y + r.h, img.height());
    if (x0 >= x1 || y0 >= y1) return;
    const int tx0 = static_cast<int>(x0 / kTileSize), tx1 = static_cast<int>((x1 - 1) / kTileSize);
    const int ty0 = static_cast<int>(y0 / kTileSize), ty1 = static_cast<int>((y1 - 1) / kTileSize);
    for (int ty = ty0; ty <= ty1; ++ty) {
        for (int tx = tx0; tx <= tx1; ++tx) {
            Tile<Px> t = img.tile(tx, ty);
            const int bx = tx * kTileSize, by = ty * kTileSize;
            const int lx0 = static_cast<int>(std::max<int64_t>(x0 - bx, 0));
            const int lx1 = static_cast<int>(std::min<int64_t>(x1 - bx, kTileSize));
            const int ly0 = static_cast<int>(std::max<int64_t>(y0 - by, 0));
            const int ly1 = static_cast<int>(std::min<int64_t>(y1 - by, kTileSize));
            f(t, bx, by, lx0, lx1, ly0, ly1);
            img.put_tile_sparse(tx, ty, t);
        }
    }
}

// Gradient parameter t (§11.1): 0.0 if the extent is 1, else the double quotient of two integers.
inline double grad_t(int64_t p, int64_t start, int64_t extent) {
    if (extent == 1) return 0.0;
    return static_cast<double>(p - start) / static_cast<double>(extent - 1);
}

inline uint8_t grad_byte(uint8_t from, uint8_t to, double t) {
    const double v = dec(from) + (t * (dec(to) - dec(from)));
    return q(v);
}

}  // namespace

RgbaImage make_layer_pixels(int w, int h, const LayerFill& f) {
    RgbaImage img(w, h);
    if (f.kind == LayerFill::Kind::Empty) return img;
    fill_rect_tiles(img, f.rect, [&](RgbaTile& t, int bx, int by, int lx0, int lx1, int ly0, int ly1) {
        for (int ly = ly0; ly < ly1; ++ly) {
            for (int lx = lx0; lx < lx1; ++lx) {
                const int64_t x = bx + lx, y = by + ly;
                Rgba8 p{};
                switch (f.kind) {
                    case LayerFill::Kind::Solid:
                        p = f.color;
                        break;
                    case LayerFill::Kind::Gradient: {
                        const double tt = f.vertical ? grad_t(y, f.rect.y, f.rect.h) : grad_t(x, f.rect.x, f.rect.w);
                        p = Rgba8{grad_byte(f.from.r, f.to.r, tt), grad_byte(f.from.g, f.to.g, tt),
                                  grad_byte(f.from.b, f.to.b, tt), grad_byte(f.from.a, f.to.a, tt)};
                        break;
                    }
                    case LayerFill::Kind::Checker: {
                        const int64_t cx = (x - f.rect.x) / f.cell;
                        const int64_t cy = (y - f.rect.y) / f.cell;
                        p = ((cx + cy) % 2 == 0) ? f.a : f.b;
                        break;
                    }
                    case LayerFill::Kind::Noise: {
                        const uint32_t ux = static_cast<uint32_t>(x), uy = static_cast<uint32_t>(y);
                        p.r = static_cast<uint8_t>(pixel_hash(f.seed, ux, uy, 0) >> 56);
                        p.g = static_cast<uint8_t>(pixel_hash(f.seed, ux, uy, 1) >> 56);
                        p.b = static_cast<uint8_t>(pixel_hash(f.seed, ux, uy, 2) >> 56);
                        p.a = f.alpha_random ? static_cast<uint8_t>(pixel_hash(f.seed, ux, uy, 3) >> 56) : f.alpha;
                        break;
                    }
                    case LayerFill::Kind::Empty:
                        break;
                }
                t.at(lx, ly) = canonicalize(p);
            }
        }
    });
    return img;
}

GrayImage make_mask_plane(int w, int h, const MaskFill& f) {
    GrayImage img(w, h, f.outside);
    fill_rect_tiles(img, f.rect, [&](GrayTile& t, int bx, int by, int lx0, int lx1, int ly0, int ly1) {
        for (int ly = ly0; ly < ly1; ++ly) {
            for (int lx = lx0; lx < lx1; ++lx) {
                const int64_t x = bx + lx, y = by + ly;
                uint8_t v = 0;
                switch (f.kind) {
                    case MaskFill::Kind::Solid:
                        v = f.value;
                        break;
                    case MaskFill::Kind::Gradient: {
                        const double tt = f.vertical ? grad_t(y, f.rect.y, f.rect.h) : grad_t(x, f.rect.x, f.rect.w);
                        v = grad_byte(f.from, f.to, tt);
                        break;
                    }
                    case MaskFill::Kind::Noise:
                        v = static_cast<uint8_t>(
                            pixel_hash(f.seed, static_cast<uint32_t>(x), static_cast<uint32_t>(y), 4) >> 56);
                        break;
                }
                t.at(lx, ly) = v;
            }
        }
    });
    return img;
}

}  // namespace rl::composite
