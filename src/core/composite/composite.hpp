// SPDX-License-Identifier: GPL-3.0-or-later
//
// The per-pixel composite primitives of docs/math/10-compositing.md §4 (scalar binary64 path).
//
// Mutation hooks owned here: 2 (B on premultiplied operands), 3 (linear light), 22 (skipped
// canonicalisation). Hooks inside B are in blend.cpp; q's mutation 14 is in base/quant.hpp.
#pragma once

#include <cstdint>

#include "core/adjust/adjustment.hpp"
#include "core/base/types.hpp"
#include "core/doc/blend_mode.hpp"

namespace rl::composite {

// §3.5 / §4.1 step 1: Dissolve coverage replacement, c' = 1 if unit(pixel_hash(seed, x, y, 5)) < c.
double dissolve_coverage(double c, uint64_t seed, uint32_t x, uint32_t y);

// §4.1 COMPOSITE(backdrop, Cs, c, mode, seed) at canvas pixel (x, y). `mode` must not be Pass.
// `src` supplies the source colour bytes (its alpha is ignored; coverage `c` is computed by the
// caller per §4.3).
Rgba8 composite_px(Rgba8 backdrop, Rgba8 src, double c, BlendMode mode, uint64_t seed, uint32_t x, uint32_t y);

// §4.2 ADJUST(backdrop, node, c, mode, seed) at canvas pixel (x, y).
Rgba8 adjust_px(Rgba8 backdrop, const adjust::Adjustment& adj, double c, BlendMode mode, uint64_t seed,
                uint32_t x, uint32_t y);

}  // namespace rl::composite
