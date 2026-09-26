// SPDX-License-Identifier: GPL-3.0-or-later
//
// Content fills shared by add_layer and add_mask (docs/math/10-compositing.md §11.1).
#pragma once

#include <cstdint>

#include "core/script/fields.hpp"
#include "core/tile/tiled_image.hpp"

namespace rl::composite {

struct LayerFill {
    enum class Kind { Empty, Solid, Gradient, Checker, Noise };
    Kind kind = Kind::Empty;
    Rgba8 color{0, 0, 0, 255};       // solid
    Rgba8 from{0, 0, 0, 255};        // gradient
    Rgba8 to{255, 255, 255, 255};    // gradient
    bool vertical = false;           // gradient dir "v"
    Rgba8 a{255, 255, 255, 255};     // checker
    Rgba8 b{204, 204, 204, 255};     // checker
    int64_t cell = 8;                // checker
    uint64_t seed = 0;               // noise (not the Dissolve seed)
    bool alpha_random = true;        // noise
    uint8_t alpha = 255;             // noise, when !alpha_random
    script::Rect rect;               // defaults to the whole canvas
};

struct MaskFill {
    enum class Kind { Solid, Gradient, Noise };
    Kind kind = Kind::Solid;
    uint8_t value = 255;  // solid
    uint8_t from = 0;     // gradient
    uint8_t to = 255;     // gradient
    bool vertical = false;
    uint64_t seed = 0;    // noise
    script::Rect rect;    // defaults to the whole canvas
    uint8_t outside = 0;  // value outside the rect (the plane's background)
};

// A canvas-sized raster layer image per §11.1 (every pixel canonical; outside the rect (0,0,0,0)).
RgbaImage make_layer_pixels(int w, int h, const LayerFill& f);

// A canvas-sized mask plane per §11.1 (outside the rect: `outside`).
GrayImage make_mask_plane(int w, int h, const MaskFill& f);

}  // namespace rl::composite
