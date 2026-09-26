// SPDX-License-Identifier: GPL-3.0-or-later
//
// What changed between two document states, so the canvas recomposites only dirty tiles.
//
// Tiles are copy-on-write handles (core/tile): an op that writes a tile replaces its handle, so
// comparing handle identity per tile is an exact, zero-copy "was this tile written" test. A change
// of any node property that affects compositing (visibility, opacity, fill, mode, clip, masks
// enabled, tree shape, adjustment) marks every tile dirty; a pure pixel change marks the tiles whose
// handle changed in any layer or mask.
#pragma once

#include <cstdint>
#include <vector>

#include "core/doc/document.hpp"

namespace rl::gui {

struct StateDiff {
    bool size_changed = false;      // canvas w/h differ: everything must be rebuilt
    bool all_tiles = false;         // every composite tile is dirty
    bool tree_changed = false;      // the Layers panel must refresh (ids, props, shape, lock)
    bool selection_changed = false; // selection mask or Reselect memory changed
    int tiles_x = 0;
    int tiles_y = 0;
    std::vector<uint8_t> dirty;     // tiles_x * tiles_y flags (row-major), when !all_tiles

    bool any_pixels() const;
    bool tile_dirty(int tx, int ty) const {
        return all_tiles || dirty[static_cast<size_t>(ty) * static_cast<size_t>(tiles_x) + static_cast<size_t>(tx)] != 0;
    }
};

StateDiff diff_states(const rl::DocState& before, const rl::DocState& after);

}  // namespace rl::gui
