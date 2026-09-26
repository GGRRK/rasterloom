// SPDX-License-Identifier: GPL-3.0-or-later
//
// Channel normalisation and the single shared quantisation rule (docs/math/00-conventions.md C2/C3).
// Every double that becomes a byte anywhere in the core goes through q().
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "core/base/mutation.hpp"
#include "core/base/types.hpp"

namespace rl {

// C2 decode: a division, never a multiplication by 1/255.
inline double dec(int v) { return static_cast<double>(v) / 255.0; }

// C2: clamp(x, lo, hi) = min(max(x, lo), hi).
inline double clamp(double x, double lo, double hi) { return std::min(std::max(x, lo), hi); }
inline double clamp01(double x) { return clamp(x, 0.0, 1.0); }

// C2 encode: y = clamp(x, 0, 1) * 255.0 (one multiply), rounded half away from zero.
// Mutation 14 (doc 10 §13): truncates instead.
inline uint8_t q(double x) {
    const double y = clamp01(x) * 255.0;
    if (mut::active(14)) return static_cast<uint8_t>(std::floor(y));
    return static_cast<uint8_t>(std::round(y));
}

// Round half away from zero of an arbitrary double (doc 20 notation `rhaz`), without clamp/scale.
inline double rhaz(double y) {
    if (y < 0.0) return -rhaz(-y);
    const double f = std::floor(y);
    return f + (((y - f) >= 0.5) ? 1.0 : 0.0);
}

// C3 canonical transparent pixel: a pixel whose alpha byte is 0 is stored as (0, 0, 0, 0).
inline Rgba8 canonicalize(Rgba8 p) { return p.a == 0 ? Rgba8{} : p; }
inline void canonicalize_inplace(Rgba8& p) {
    if (p.a == 0) p = Rgba8{};
}

}  // namespace rl
