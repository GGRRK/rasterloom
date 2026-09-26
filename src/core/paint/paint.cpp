// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/paint/paint.hpp"

#include <cmath>
#include <vector>

#include "core/base/quant.hpp"
#include "core/geometry/dense.hpp"
#include "core/select/selection_ops.hpp"

namespace rl::paint {

void paint_over(Rgba8& px, double sr, double sg, double sb, double as, bool lock_alpha) {
    if (as == 0.0) return;
    const double ab = dec(px.a);
    if (lock_alpha) {
        if (px.a == 0) return;
        const double inv = 1.0 - as;
        px.r = q((dec(px.r) * inv) + (sr * as));
        px.g = q((dec(px.g) * inv) + (sg * as));
        px.b = q((dec(px.b) * inv) + (sb * as));
    } else {
        const double inv = 1.0 - as;
        const double ao = as + (ab * inv);
        px.r = q(((sr * as) + ((dec(px.r) * ab) * inv)) / ao);
        px.g = q(((sg * as) + ((dec(px.g) * ab) * inv)) / ao);
        px.b = q(((sb * as) + ((dec(px.b) * ab) * inv)) / ao);
        px.a = q(ao);
    }
    canonicalize_inplace(px);
}

Colour decode(Rgba8 c) { return Colour{dec(c.r), dec(c.g), dec(c.b), dec(c.a)}; }

namespace {

// Visits every canvas pixel row-major by tile, computing its source (r, g, b, as) with `src`;
// writes only pixels whose `as` is non-zero (untouched tiles stay shared / absent).
template <class Src>
void deposit(Node& layer, const Coverage& cov, Src&& src) {
    RgbaImage& img = layer.pixels;
    const int W = img.width(), H = img.height();
    for (int ty = 0; ty < img.tiles_y(); ++ty) {
        for (int tx = 0; tx < img.tiles_x(); ++tx) {
            const GrayTile* ct = cov.tile(tx, ty);
            RgbaTile* wt = nullptr;
            const int x0 = tx * kTileSize, y0 = ty * kTileSize;
            const int xn = std::min(kTileSize, W - x0), yn = std::min(kTileSize, H - y0);
            for (int ly = 0; ly < yn; ++ly) {
                for (int lx = 0; lx < xn; ++lx) {
                    const double e = ct ? dec(ct->at(lx, ly)) : 1.0;
                    double r = 0.0, g = 0.0, b = 0.0;
                    const double as = src(x0 + lx, y0 + ly, e, r, g, b);
                    if (as == 0.0) continue;
                    if (layer.lock_alpha && img.tile(tx, ty).at(lx, ly).a == 0) continue;
                    if (!wt) wt = &img.mutable_tile(tx, ty);
                    paint_over(wt->at(lx, ly), r, g, b, as, layer.lock_alpha);
                }
            }
        }
    }
}

}  // namespace

void fill_selection(Node& layer, const Selection& sel, Rgba8 color, double opacity) {
    const Coverage cov(sel);
    const Colour c = decode(color);
    deposit(layer, cov, [&](int, int, double e, double& r, double& g, double& b) {
        r = c.r;
        g = c.g;
        b = c.b;
        return (c.a * opacity) * e;
    });
}

void gradient(Node& layer, const Selection& sel, const GradientParams& p) {
    const double dx = p.x1 - p.x0;
    const double dy = p.y1 - p.y0;
    const double L2 = (dx * dx) + (dy * dy);
    if (L2 == 0.0) return;
    const double radius = std::sqrt(L2);
    const Colour c0 = decode(p.c0), c1 = decode(p.c1);
    const Coverage cov(sel);
    deposit(layer, cov, [&](int x, int y, double e, double& r, double& g, double& b) {
        const double ex = (static_cast<double>(x) + 0.5) - p.x0;
        const double ey = (static_cast<double>(y) + 0.5) - p.y0;
        double t = 0.0;
        if (p.type == GradientType::Linear)
            t = ((ex * dx) + (ey * dy)) / L2;
        else
            t = std::sqrt((ex * ex) + (ey * ey)) / radius;
        t = clamp(t, 0.0, 1.0);
        if (p.reverse) t = 1.0 - t;
        r = c0.r + ((c1.r - c0.r) * t);
        g = c0.g + ((c1.g - c0.g) * t);
        b = c0.b + ((c1.b - c0.b) * t);
        const double ga = c0.a + ((c1.a - c0.a) * t);
        return (ga * p.opacity) * e;
    });
}

void bucket_fill(Node& layer, const Selection& sel, const BucketParams& p) {
    const int W = layer.pixels.width(), H = layer.pixels.height();
    if (p.x < 0 || p.y < 0 || p.x >= W || p.y >= H) return;  // §18: seed outside the canvas
    // The dense layer copy; region() charges its own planes. (fill_selection and gradient write
    // tiles in place through deposit(), which the tile tiers already charge.)
    const mem::Reservation hold = geom::reserve_dense(geom::dense_bytes(W, H, sizeof(Rgba8)), "bucket_fill layer copy");
    const std::vector<Rgba8> px = geom::to_dense(layer.pixels);
    const std::vector<uint8_t> Rg = select::region(px, W, H, p.x, p.y, p.tolerance, p.contiguous, p.antialias);
    const mem::Reservation hold_rg = geom::reserve_dense(Rg.size(), "bucket_fill region plane");  // was region()'s
    const Colour c = decode(p.color);
    const Coverage cov(sel);
    deposit(layer, cov, [&](int x, int y, double e, double& r, double& g, double& b) {
        r = c.r;
        g = c.g;
        b = c.b;
        return ((c.a * p.opacity) * dec(Rg[geom::idx(x, y, W)])) * e;
    });
}

}  // namespace rl::paint
