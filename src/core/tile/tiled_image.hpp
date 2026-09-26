// SPDX-License-Identifier: GPL-3.0-or-later
//
// The tile engine (BUILD-SPEC <stack> "Tile engine", docs/math/00-conventions.md C4).
//
// - 64x64 tiles, generic over the pixel type (Rgba8 for layers, Gray8 for masks and selection).
// - Sparse: TileKey -> shared storage cell (TileCellPtr). An absent entry reads as the image's uniform
//   background tile; for background 0 (every RGBA layer, the selection) that is ONE process-wide
//   shared all-zero tile.
// - Copy-on-write: copying a TiledImage copies tile handles only; mutable_tile() clones a tile that
//   is shared with any other image (a history snapshot, a copy) before handing out a writable
//   reference. The pre-existing shared_ptr IS the undo record (BUILD-SPEC D5).
// - Deterministic iteration: sorted_keys() returns keys in row-major order (ty, then tx). The
//   unordered_map is never iterated where output depends on the order.
// - Mutation 13 (dense tile grid) allocates a distinct tile for every grid cell instead of relying
//   on the shared empty tile; allocated_tiles() exposes it.
// - Storage tiers (core/tile/memory.hpp): each stored tile is a mem::Cell, the unit of CoW sharing.
//   A cell may be LZ4-compressed in RAM or spilled to the scratch file between ops; tile() faults
//   it back in transparently, so no caller can observe the tiers. A reference returned by tile()
//   or mutable_tile() stays valid until the next mem::safe_point (the next op).
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "core/base/mutation.hpp"
#include "core/base/types.hpp"
#include "core/tile/memory.hpp"

namespace rl {

constexpr int kTileSize = 64;
constexpr int kTilePixels = kTileSize * kTileSize;

struct TileKey {
    int32_t tx = 0;
    int32_t ty = 0;

    friend bool operator==(const TileKey& a, const TileKey& b) { return a.tx == b.tx && a.ty == b.ty; }
    // Row-major order: by ty, then tx (C4).
    friend bool operator<(const TileKey& a, const TileKey& b) {
        return a.ty != b.ty ? a.ty < b.ty : a.tx < b.tx;
    }
};

struct TileKeyHash {
    size_t operator()(const TileKey& k) const noexcept {
        const uint64_t v = (static_cast<uint64_t>(static_cast<uint32_t>(k.ty)) << 32) |
                           static_cast<uint32_t>(k.tx);
        return static_cast<size_t>(v * 0x9E3779B97F4A7C15ULL);
    }
};

template <class Px>
struct Tile {
    std::array<Px, kTilePixels> px;  // row-major, index = (y * 64) + x within the tile

    Px& at(int lx, int ly) { return px[static_cast<size_t>((ly * kTileSize) + lx)]; }
    const Px& at(int lx, int ly) const { return px[static_cast<size_t>((ly * kTileSize) + lx)]; }
};

// The shared storage handle of one stored tile (see memory.hpp). Images share cells copy-on-write.
using TileCellPtr = std::shared_ptr<mem::Cell>;

namespace detail {

template <class Px>
uint32_t px_bits(const Px& v) {
    static_assert(sizeof(Px) <= sizeof(uint32_t), "pixel type too large for the uniform-tile cache");
    uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(Px));
    return bits;
}

static_assert(std::is_trivially_copyable_v<Tile<Rgba8>> && std::is_trivially_copyable_v<Tile<Gray8>>,
              "tile bytes are compressed and restored with memcpy semantics");

template <class Px>
const Tile<Px>& cell_tile(const mem::Cell& c) {
    return *static_cast<const Tile<Px>*>(c.read());
}
template <class Px>
Tile<Px>& cell_tile_mut(mem::Cell& c) {
    return *static_cast<Tile<Px>*>(c.write());
}

// A new hot cell holding a copy of `t`.
template <class Px>
TileCellPtr make_cell(const Tile<Px>& t) {
    auto c = std::make_shared<mem::Cell>(sizeof(Tile<Px>));
    cell_tile_mut<Px>(*c) = t;
    return c;
}
// A new hot cell filled with `v`.
template <class Px>
TileCellPtr make_cell_filled(const Px& v) {
    auto c = std::make_shared<mem::Cell>(sizeof(Tile<Px>));
    cell_tile_mut<Px>(*c).px.fill(v);
    return c;
}

// Shared read-only uniform tiles (pinned cells: never evicted, never written). Value 0 is THE shared
// empty tile. Thread-safe; the map's nodes are stable, so the returned reference stays valid.
template <class Px>
const TileCellPtr& uniform_tile(const Px& v) {
    static std::mutex m;
    static std::map<uint32_t, TileCellPtr> cache;
    const uint32_t key = px_bits(v);
    std::lock_guard<std::mutex> lk(m);
    auto it = cache.find(key);
    if (it == cache.end()) {
        auto c = std::make_shared<mem::Cell>(sizeof(Tile<Px>), /*pinned=*/true);
        static_cast<Tile<Px>*>(const_cast<void*>(c->read()))->px.fill(v);
        it = cache.emplace(key, std::move(c)).first;
    }
    return it->second;
}

}  // namespace detail

template <class Px>
class TiledImage {
public:
    using TileT = Tile<Px>;
    using TilePtr = TileCellPtr;  // shared storage cell of one tile

    TiledImage() = default;
    TiledImage(int width, int height, Px background = Px{}) { reset(width, height, background); }

    // Re-initialises to an all-background image of the given size.
    void reset(int width, int height, Px background = Px{}) {
        w_ = width;
        h_ = height;
        bg_ = background;
        tiles_.clear();
        if (mut::active(13)) densify();
    }

    int width() const { return w_; }
    int height() const { return h_; }
    int tiles_x() const { return (w_ + kTileSize - 1) / kTileSize; }
    int tiles_y() const { return (h_ + kTileSize - 1) / kTileSize; }
    size_t total_tiles() const { return static_cast<size_t>(tiles_x()) * static_cast<size_t>(tiles_y()); }
    const Px& background() const { return bg_; }

    bool in_bounds(int x, int y) const { return x >= 0 && y >= 0 && x < w_ && y < h_; }

    // Out-of-canvas reads return Px{} (the transparent pixel / byte 0), per C4.
    Px get(int x, int y) const {
        if (!in_bounds(x, y)) return Px{};
        return tile(x / kTileSize, y / kTileSize).at(x % kTileSize, y % kTileSize);
    }

    // Writes outside the canvas are discarded (C8a).
    void set(int x, int y, const Px& v) {
        if (!in_bounds(x, y)) return;
        mutable_tile(x / kTileSize, y / kTileSize).at(x % kTileSize, y % kTileSize) = v;
    }

    // Read access to a tile; absent tiles return the shared uniform background tile.
    const TileT& tile(int tx, int ty) const { return detail::cell_tile<Px>(*tile_ptr(tx, ty)); }
    const TilePtr& tile_ptr(int tx, int ty) const {
        auto it = tiles_.find(TileKey{tx, ty});
        return it == tiles_.end() ? detail::uniform_tile(bg_) : it->second;
    }

    // True when no tile is stored at (tx, ty): the tile reads as the uniform background.
    bool is_absent(int tx, int ty) const { return tiles_.find(TileKey{tx, ty}) == tiles_.end(); }

    // Copy-on-write mutable access. Allocates (filled with background) when absent; clones when the
    // stored tile is shared with anything else.
    TileT& mutable_tile(int tx, int ty) {
        const TileKey key{tx, ty};
        auto it = tiles_.find(key);
        if (it == tiles_.end()) {
            TilePtr c = detail::make_cell_filled<Px>(bg_);
            TileT& raw = detail::cell_tile_mut<Px>(*c);
            tiles_.emplace(key, std::move(c));
            return raw;
        }
        if (it->second.use_count() != 1 || it->second->pinned()) {
            TilePtr c = detail::make_cell<Px>(detail::cell_tile<Px>(*it->second));
            TileT& raw = detail::cell_tile_mut<Px>(*c);
            it->second = std::move(c);
            return raw;
        }
        // Sole owner: write in place (drops any compressed copy of the cell).
        return detail::cell_tile_mut<Px>(*it->second);
    }

    // Stores `t` at (tx, ty) (shared, no copy). A null pointer removes the tile.
    void put_tile(int tx, int ty, TilePtr t) {
        if (!t) {
            erase_tile(tx, ty);
            return;
        }
        tiles_[TileKey{tx, ty}] = std::move(t);
    }

    // Removes the stored tile so it reads as background (under mutation 13 a fresh background tile
    // is allocated instead, keeping the grid dense).
    void erase_tile(int tx, int ty) {
        tiles_.erase(TileKey{tx, ty});
        if (mut::active(13)) alloc_background_tile(tx, ty);
    }

    // Stores a tile only when it differs from the uniform background (keeps images sparse).
    void put_tile_sparse(int tx, int ty, const TileT& content) {
        if (is_uniform_tile(content, bg_) && !mut::active(13)) {
            tiles_.erase(TileKey{tx, ty});
            return;
        }
        tiles_[TileKey{tx, ty}] = detail::make_cell<Px>(content);
    }

    // Keys of stored tiles, row-major (ty, then tx).
    std::vector<TileKey> sorted_keys() const {
        std::vector<TileKey> keys;
        keys.reserve(tiles_.size());
        for (const auto& kv : tiles_) keys.push_back(kv.first);
        std::sort(keys.begin(), keys.end());
        return keys;
    }

    // Number of stored (allocated) tiles; absent tiles share the uniform background tile.
    size_t allocated_tiles() const { return tiles_.size(); }

    // True when every pixel equals `v` (absent tiles read as background).
    bool all_equal(const Px& v) const {
        if (tiles_.size() < total_tiles() && px_bits_eq(bg_, v) == false) return false;
        for (const auto& kv : tiles_) {
            if (!is_uniform_tile(detail::cell_tile<Px>(*kv.second), v, kv.first)) return false;
        }
        return true;
    }

    // Drops stored tiles that are entirely background (no-op under mutation 13).
    void compact() {
        if (mut::active(13)) return;
        for (const TileKey& k : sorted_keys()) {
            auto it = tiles_.find(k);
            if (is_uniform_tile(detail::cell_tile<Px>(*it->second), bg_)) tiles_.erase(it);
        }
    }

    // True when both images hold the very same tile object at (tx, ty) (CoW sharing check).
    bool shares_tile(const TiledImage& other, int tx, int ty) const {
        auto a = tiles_.find(TileKey{tx, ty});
        auto b = other.tiles_.find(TileKey{tx, ty});
        if (a == tiles_.end() || b == other.tiles_.end()) return false;
        return a->second.get() == b->second.get();
    }

    // Pixel-exact equality over the canvas (sizes must match).
    bool pixels_equal(const TiledImage& other) const {
        if (w_ != other.w_ || h_ != other.h_) return false;
        for (int ty = 0; ty < tiles_y(); ++ty)
            for (int tx = 0; tx < tiles_x(); ++tx) {
                const TileT& a = tile(tx, ty);
                const TileT& b = other.tile(tx, ty);
                if (&a == &b) continue;
                for (int ly = 0; ly < kTileSize; ++ly)
                    for (int lx = 0; lx < kTileSize; ++lx) {
                        const int x = (tx * kTileSize) + lx, y = (ty * kTileSize) + ly;
                        if (x >= w_ || y >= h_) continue;
                        if (!px_bits_eq(a.at(lx, ly), b.at(lx, ly))) return false;
                    }
            }
        return true;
    }

private:
    static bool px_bits_eq(const Px& a, const Px& b) { return detail::px_bits(a) == detail::px_bits(b); }

    static bool is_uniform_tile(const TileT& t, const Px& v) {
        const uint32_t bits = detail::px_bits(v);
        for (const Px& p : t.px)
            if (detail::px_bits(p) != bits) return false;
        return true;
    }

    // Only the in-canvas part of an edge tile matters.
    bool is_uniform_tile(const TileT& t, const Px& v, const TileKey& k) const {
        const uint32_t bits = detail::px_bits(v);
        for (int ly = 0; ly < kTileSize; ++ly) {
            const int y = (k.ty * kTileSize) + ly;
            if (y >= h_) break;
            for (int lx = 0; lx < kTileSize; ++lx) {
                const int x = (k.tx * kTileSize) + lx;
                if (x >= w_) break;
                if (detail::px_bits(t.at(lx, ly)) != bits) return false;
            }
        }
        return true;
    }

    void alloc_background_tile(int tx, int ty) { tiles_[TileKey{tx, ty}] = detail::make_cell_filled<Px>(bg_); }

    // Mutation 13: one distinct allocated tile per grid cell.
    void densify() {
        for (int ty = 0; ty < tiles_y(); ++ty)
            for (int tx = 0; tx < tiles_x(); ++tx)
                if (tiles_.find(TileKey{tx, ty}) == tiles_.end()) alloc_background_tile(tx, ty);
    }

    int w_ = 0;
    int h_ = 0;
    Px bg_{};
    std::unordered_map<TileKey, TilePtr, TileKeyHash> tiles_;
};

using RgbaImage = TiledImage<Rgba8>;
using GrayImage = TiledImage<Gray8>;
using RgbaTile = Tile<Rgba8>;
using GrayTile = Tile<Gray8>;

}  // namespace rl
