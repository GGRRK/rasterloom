// SPDX-License-Identifier: GPL-3.0-or-later
#include <gtest/gtest.h>

#include "core/base/error.hpp"
#include "core/base/mutation.hpp"
#include "core/doc/document.hpp"
#include "core/tile/tiled_image.hpp"

using namespace rl;

TEST(Tile, EmptyImageAllocatesNothingAndSharesOneEmptyTile) {
    RgbaImage a(1000, 700), b(64, 64);
    EXPECT_EQ(a.allocated_tiles(), 0u);
    EXPECT_EQ(a.total_tiles(), 16u * 11u);
    EXPECT_EQ(a.get(5, 5), Rgba8{});
    // Absent tiles of every zero-background image are the very same object.
    EXPECT_EQ(&a.tile(0, 0), &a.tile(15, 10));
    EXPECT_EQ(&a.tile(3, 3), &b.tile(0, 0));
    EXPECT_TRUE(a.is_absent(2, 2));
}

TEST(Tile, WriteAllocatesOnlyTouchedTile) {
    RgbaImage a(1000, 700);
    a.set(130, 70, Rgba8{1, 2, 3, 4});
    EXPECT_EQ(a.allocated_tiles(), 1u);
    EXPECT_FALSE(a.is_absent(2, 1));
    EXPECT_EQ(a.get(130, 70), (Rgba8{1, 2, 3, 4}));
    EXPECT_EQ(a.get(131, 70), Rgba8{});
}

TEST(Tile, OutOfCanvasReadsTransparentAndWritesAreDiscarded) {
    RgbaImage a(10, 10);
    a.set(-1, 0, Rgba8{9, 9, 9, 9});
    a.set(10, 0, Rgba8{9, 9, 9, 9});
    a.set(0, 10, Rgba8{9, 9, 9, 9});
    EXPECT_EQ(a.allocated_tiles(), 0u);
    EXPECT_EQ(a.get(-1, -1), Rgba8{});
    EXPECT_EQ(a.get(10, 3), Rgba8{});
}

TEST(Tile, SortedKeysAreRowMajor) {
    RgbaImage a(300, 300);
    a.set(130, 0, Rgba8{1, 1, 1, 1});  // (2, 0)
    a.set(0, 70, Rgba8{1, 1, 1, 1});   // (0, 1)
    a.set(70, 0, Rgba8{1, 1, 1, 1});   // (1, 0)
    a.set(0, 0, Rgba8{1, 1, 1, 1});    // (0, 0)
    a.set(200, 200, Rgba8{1, 1, 1, 1});  // (3, 3)
    const auto keys = a.sorted_keys();
    ASSERT_EQ(keys.size(), 5u);
    const int expect[5][2] = {{0, 0}, {1, 0}, {2, 0}, {0, 1}, {3, 3}};
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(keys[static_cast<size_t>(i)].tx, expect[i][0]);
        EXPECT_EQ(keys[static_cast<size_t>(i)].ty, expect[i][1]);
    }
}

TEST(Tile, CopyOnWrite) {
    RgbaImage a(200, 100);
    a.set(1, 1, Rgba8{10, 0, 0, 255});
    a.set(100, 1, Rgba8{20, 0, 0, 255});
    RgbaImage b = a;  // shares both tiles
    EXPECT_TRUE(b.shares_tile(a, 0, 0));
    EXPECT_TRUE(b.shares_tile(a, 1, 0));

    b.set(2, 2, Rgba8{30, 0, 0, 255});  // clones tile (0,0) only
    EXPECT_FALSE(b.shares_tile(a, 0, 0));
    EXPECT_TRUE(b.shares_tile(a, 1, 0));
    EXPECT_EQ(a.get(2, 2), Rgba8{});
    EXPECT_EQ(b.get(2, 2), (Rgba8{30, 0, 0, 255}));
    EXPECT_EQ(b.get(1, 1), (Rgba8{10, 0, 0, 255}));

    // A sole owner is written in place (no clone).
    const RgbaTile* before = &b.tile(0, 0);
    b.set(3, 3, Rgba8{40, 0, 0, 255});
    EXPECT_EQ(&b.tile(0, 0), before);
}

TEST(Tile, GrayBackgroundIsSharedUniformTile) {
    GrayImage m(200, 200, 64);
    EXPECT_EQ(m.allocated_tiles(), 0u);
    EXPECT_EQ(m.get(150, 150), 64);
    m.set(5, 5, 200);
    EXPECT_EQ(m.allocated_tiles(), 1u);
    EXPECT_EQ(m.get(5, 5), 200);
    EXPECT_EQ(m.get(6, 5), 64);
    m.set(5, 5, 64);
    m.compact();
    EXPECT_EQ(m.allocated_tiles(), 0u);
    EXPECT_TRUE(m.all_equal(64));
}

TEST(Tile, AllEqualAndPixelsEqual) {
    GrayImage s(100, 100, 0);
    EXPECT_TRUE(s.all_equal(0));
    s.set(99, 99, 1);
    EXPECT_FALSE(s.all_equal(0));
    GrayImage t(100, 100, 0);
    EXPECT_FALSE(s.pixels_equal(t));
    t.set(99, 99, 1);
    EXPECT_TRUE(s.pixels_equal(t));
}

TEST(Tile, PutTileSparseDropsBackgroundTiles) {
    RgbaImage a(128, 64);
    RgbaTile t;
    t.px.fill(Rgba8{});
    a.put_tile_sparse(0, 0, t);
    EXPECT_EQ(a.allocated_tiles(), 0u);
    t.at(3, 3) = Rgba8{1, 2, 3, 4};
    a.put_tile_sparse(1, 0, t);
    EXPECT_EQ(a.allocated_tiles(), 1u);
    EXPECT_EQ(a.get(67, 3), (Rgba8{1, 2, 3, 4}));
}

// Mutation 13 must be detectable: a dense grid allocates every tile.
TEST(Tile, Mutation13DenseGridIsDetectable) {
    {
        RgbaImage sparse(640, 320);
        EXPECT_EQ(sparse.allocated_tiles(), 0u);
    }
    mut::ScopedMutations m({13});
    RgbaImage dense(640, 320);
    EXPECT_EQ(dense.allocated_tiles(), dense.total_tiles());
    EXPECT_EQ(dense.total_tiles(), 50u);
    // Every tile is a distinct allocation, not the shared empty tile.
    EXPECT_NE(&dense.tile(0, 0), &dense.tile(1, 0));
    EXPECT_EQ(dense.get(10, 10), Rgba8{});
    Document doc(640, 320, Rgba8{});
    size_t sel_alloc = 0;
    for (const auto& s : doc.tile_stats())
        if (s.what == "selection") sel_alloc = s.allocated;
    EXPECT_EQ(sel_alloc, 50u);
}

TEST(History, SnapshotsShareTilesAndUndoRestores) {
    Document doc(128, 128, Rgba8{}, 1000);
    Node n = Node::make_raster("a", 128, 128);
    n.pixels.set(1, 1, Rgba8{5, 5, 5, 255});
    doc.push_history();
    doc.add_node_top("root", std::move(n), "test");
    doc.push_history();
    Node& a = *doc.find("a").node;
    a.pixels.set(2, 2, Rgba8{6, 6, 6, 255});  // clones the tile; the snapshot keeps the old one
    EXPECT_EQ(doc.history_size(), 2u);
    doc.undo(1);
    EXPECT_EQ(doc.find("a").node->pixels.get(2, 2), Rgba8{});
    EXPECT_EQ(doc.find("a").node->pixels.get(1, 1), (Rgba8{5, 5, 5, 255}));
    doc.undo(1);
    EXPECT_EQ(doc.find("a").node, nullptr);
    EXPECT_THROW(doc.undo(1), ScriptError);
}

TEST(History, DepthDropsOldestRecords) {
    Document doc(8, 8, Rgba8{}, 3);
    for (int i = 0; i < 5; ++i) doc.push_history();
    EXPECT_EQ(doc.history_size(), 3u);
    EXPECT_THROW(doc.undo(4), ScriptError);
    doc.undo(3);
    EXPECT_EQ(doc.history_size(), 0u);
}

TEST(Selection, ActiveIsDerivedFromMask) {
    Selection s(100, 100);
    EXPECT_FALSE(s.active());
    EXPECT_EQ(s.effective(5, 5), 255);
    s.mask.set(5, 5, 128);
    EXPECT_TRUE(s.active());
    EXPECT_EQ(s.effective(5, 5), 128);
    EXPECT_EQ(s.effective(6, 5), 0);
    s.mask.set(5, 5, 0);  // all-zero selection == no selection (C8a)
    EXPECT_FALSE(s.active());
}
