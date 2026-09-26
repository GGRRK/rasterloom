// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/state_diff.hpp"

#include <algorithm>

namespace rl::gui {

namespace {

template <class Px>
void mark_changed_tiles(const rl::TiledImage<Px>& a, const rl::TiledImage<Px>& b, StateDiff& d) {
    if (d.all_tiles) return;
    if (a.width() != b.width() || a.height() != b.height() || !(a.background() == b.background())) {
        d.all_tiles = true;
        return;
    }
    for (int ty = 0; ty < d.tiles_y; ++ty) {
        for (int tx = 0; tx < d.tiles_x; ++tx) {
            if (a.tile_ptr(tx, ty).get() != b.tile_ptr(tx, ty).get())
                d.dirty[static_cast<size_t>(ty) * static_cast<size_t>(d.tiles_x) + static_cast<size_t>(tx)] = 1;
        }
    }
}

void compare_nodes(const rl::Node& a, const rl::Node& b, StateDiff& d) {
    if (a.id != b.id || a.name != b.name || a.lock_alpha != b.lock_alpha) d.tree_changed = true;  // panel-only changes
    const bool render_props_differ = a.kind != b.kind || a.visible != b.visible || a.opacity != b.opacity ||
                                     a.mode != b.mode || a.seed != b.seed || a.fill != b.fill ||
                                     a.clip != b.clip || a.clbl != b.clbl || a.adjustment != b.adjustment ||
                                     a.mask.has_value() != b.mask.has_value() ||
                                     (a.mask && b.mask && a.mask->enabled != b.mask->enabled) ||
                                     a.children.size() != b.children.size();
    if (render_props_differ) {
        d.tree_changed = true;
        d.all_tiles = true;
        return;
    }
    if (a.is_raster()) mark_changed_tiles(a.pixels, b.pixels, d);
    if (a.mask && b.mask) mark_changed_tiles(a.mask->plane, b.mask->plane, d);
    for (size_t i = 0; i < a.children.size(); ++i) compare_nodes(a.children[i], b.children[i], d);
}

bool gray_images_differ(const rl::GrayImage& a, const rl::GrayImage& b) {
    if (a.width() != b.width() || a.height() != b.height() || a.background() != b.background()) return true;
    for (int ty = 0; ty < a.tiles_y(); ++ty)
        for (int tx = 0; tx < a.tiles_x(); ++tx)
            if (a.tile_ptr(tx, ty).get() != b.tile_ptr(tx, ty).get()) return true;
    return false;
}

}  // namespace

bool StateDiff::any_pixels() const {
    return all_tiles || std::any_of(dirty.begin(), dirty.end(), [](uint8_t v) { return v != 0; });
}

StateDiff diff_states(const rl::DocState& before, const rl::DocState& after) {
    StateDiff d;
    d.tiles_x = (after.w + rl::kTileSize - 1) / rl::kTileSize;
    d.tiles_y = (after.h + rl::kTileSize - 1) / rl::kTileSize;
    if (before.w != after.w || before.h != after.h) {
        d.size_changed = d.all_tiles = d.tree_changed = d.selection_changed = true;
        return d;
    }
    d.dirty.assign(static_cast<size_t>(d.tiles_x) * static_cast<size_t>(d.tiles_y), 0);
    if (!(before.bg == after.bg)) d.all_tiles = true;
    compare_nodes(before.root, after.root, d);

    const rl::Selection& sa = before.selection;
    const rl::Selection& sb = after.selection;
    d.selection_changed = gray_images_differ(sa.mask, sb.mask) || sa.saved.has_value() != sb.saved.has_value() ||
                          (sa.saved && sb.saved && gray_images_differ(*sa.saved, *sb.saved));
    return d;
}

}  // namespace rl::gui
