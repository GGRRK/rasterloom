// SPDX-License-Identifier: GPL-3.0-or-later
//
// Colour deposits into one raster layer (docs/math/30-geometry-selection.md §1.2 paint_over, §10
// fill_selection, §17 gradient, §18 bucket_fill) and the selection-coverage reader every painting
// op uses (§2 E / e, §9 coverage form).
#pragma once

#include <cstdint>

#include "core/doc/document.hpp"

namespace rl::paint {

// Effective coverage E (doc 30 §2): 255 everywhere when the selection is empty, else S. Resolve it
// once per op (Selection::active() scans the mask), then read per tile or per pixel.
class Coverage {
public:
    explicit Coverage(const Selection& sel) : m_(sel.active() ? &sel.mask : nullptr) {}
    bool all() const { return m_ == nullptr; }               // no selection: E = 255 everywhere
    const GrayImage* mask() const { return m_; }
    uint8_t at(int x, int y) const { return m_ ? m_->get(x, y) : uint8_t{255}; }
    // The coverage tile at (tx, ty), or nullptr when E = 255 everywhere.
    const GrayTile* tile(int tx, int ty) const { return m_ ? &m_->tile(tx, ty) : nullptr; }

private:
    const GrayImage* m_;
};

// §1.2 on one stored pixel. `as` already carries every coverage/opacity factor.
void paint_over(Rgba8& px, double sr, double sg, double sb, double as, bool lock_alpha);

struct Colour {
    double r = 0.0, g = 0.0, b = 0.0, a = 0.0;  // dec() of the script bytes (not canonicalised)
};
Colour decode(Rgba8 c);

// §10: as = (ca * opacity) * e.
void fill_selection(Node& layer, const Selection& sel, Rgba8 color, double opacity);

// §17.
enum class GradientType { Linear, Radial };
struct GradientParams {
    GradientType type = GradientType::Linear;
    double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
    Rgba8 c0{}, c1{};
    double opacity = 1.0;
    bool reverse = false;
};
void gradient(Node& layer, const Selection& sel, const GradientParams& p);

// §18: region from the layer's own bytes (before any write), then
// as = ((ca * opacity) * dec(Rg)) * e.
struct BucketParams {
    int64_t x = 0, y = 0;
    Rgba8 color{};
    double opacity = 1.0;
    int tolerance = 32;
    bool contiguous = true;
    bool antialias = true;
};
void bucket_fill(Node& layer, const Selection& sel, const BucketParams& p);

}  // namespace rl::paint
