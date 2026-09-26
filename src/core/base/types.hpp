// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace rl {

// One stored pixel: 8-bit straight (unassociated) RGBA, channel order R, G, B, A (00-conventions C3).
struct Rgba8 {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    uint8_t a = 0;

    friend bool operator==(const Rgba8& x, const Rgba8& y) {
        return x.r == y.r && x.g == y.g && x.b == y.b && x.a == y.a;
    }
    friend bool operator!=(const Rgba8& x, const Rgba8& y) { return !(x == y); }
};

static_assert(sizeof(Rgba8) == 4, "Rgba8 must be tightly packed");

// Single-channel 8-bit sample (layer masks, selection coverage).
using Gray8 = uint8_t;

constexpr int kMaxCanvasSide = 16384;  // BUILD-SPEC req 2, 00-conventions C9

}  // namespace rl
