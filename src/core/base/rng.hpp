// SPDX-License-Identifier: GPL-3.0-or-later
//
// Deterministic randomness (docs/math/00-conventions.md C6). No global RNG state exists anywhere.
#pragma once

#include <cstdint>

namespace rl {

constexpr uint64_t splitmix64(uint64_t z) {
    z = z + 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

// x, y are canvas pixel coordinates (non-negative, < 2^32); k is the calling doc's stream index.
constexpr uint64_t pixel_hash(uint64_t seed, uint32_t x, uint32_t y, uint64_t k) {
    return splitmix64(seed ^ splitmix64((static_cast<uint64_t>(y) << 32) | static_cast<uint64_t>(x)) ^
                      (k * 0xD1B54A32D192ED03ULL));
}

// A double in [0, 1), exact: (h >> 11) * 2^-53.
constexpr double unit(uint64_t h) { return static_cast<double>(h >> 11) * 0x1.0p-53; }

}  // namespace rl
