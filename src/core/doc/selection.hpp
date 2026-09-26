// SPDX-License-Identifier: GPL-3.0-or-later
//
// Selection storage (00-conventions C8a; the operations are doc 30's, the storage lives here).
//
// - `mask`: a canvas-sized 8-bit coverage mask S (0 = unselected, 255 = fully selected).
// - `saved`: the Reselect memory (doc 30 §2 "Saved"), absent or a canvas-sized mask.
// - Active flag: doc 30 §2 defines "empty means none" (no separate stored flag), and C8a says an
//   all-zero selection is the same as no selection. So active() is DERIVED: true iff any byte of
//   `mask` is non-zero. It cannot disagree with the mask.
//
// Effective coverage consumed by painting/filtering ops (doc 30 §2): E = 255 everywhere when no
// selection is active, else S; e = E / 255.0.
#pragma once

#include <optional>

#include "core/base/quant.hpp"
#include "core/tile/tiled_image.hpp"

namespace rl {

struct Selection {
    GrayImage mask;
    std::optional<GrayImage> saved;

    Selection() = default;
    Selection(int w, int h) : mask(w, h, 0) {}

    bool active() const { return !mask.all_equal(0); }

    // E(x, y) for one pixel. For bulk use, test active() once and read `mask` tiles directly.
    uint8_t effective(int x, int y) const { return active() ? mask.get(x, y) : uint8_t{255}; }
    double effective_coverage(int x, int y) const { return dec(effective(x, y)); }

    // Resets to "no selection" at a new canvas size and forgets Reselect memory (doc 30 §2
    // canvas-geometry rule).
    void reset(int w, int h) {
        mask.reset(w, h, 0);
        saved.reset();
    }
};

}  // namespace rl
