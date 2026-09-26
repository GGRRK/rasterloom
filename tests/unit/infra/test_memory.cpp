// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tile memory tiers (src/core/tile/memory.hpp): compression, spill, fault-in, CoW across tiers,
// the hard limit and the shared empty tile, all with forced tiny limits.
#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "core/base/mutation.hpp"
#include "core/doc/document.hpp"
#include "core/tile/memory.hpp"
#include "core/tile/tiled_image.hpp"

using namespace rl;

namespace {

// Installs limits for one test and restores the previous ones.
class LimitsScope {
public:
    explicit LimitsScope(const mem::Limits& l) : saved_(mem::limits()) { mem::set_limits(l); }
    ~LimitsScope() { mem::set_limits(saved_); }

private:
    mem::Limits saved_;
};

mem::Limits tiny(uint64_t hard = 1ull << 34) {
    mem::Limits l = mem::default_limits();
    l.hard = hard;
    l.soft = 0;
    l.warm = 0;
    l.scratch_cap = 4096ull << 20;
    const char* tmp = std::getenv("TMPDIR");
    l.scratch_dir = std::string(tmp && *tmp ? tmp : "/tmp") + "/rasterloom-unit-scratch";
    return l;
}

Rgba8 pattern(int x, int y) {
    return Rgba8{static_cast<uint8_t>(x * 7 + y), static_cast<uint8_t>(x ^ y), static_cast<uint8_t>(y * 3),
                 static_cast<uint8_t>(255 - ((x + y) & 63))};
}

RgbaImage patterned(int w, int h) {
    RgbaImage img(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) img.set(x, y, pattern(x, y));
    return img;
}

void expect_pattern(const RgbaImage& img) {
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x) ASSERT_EQ(img.get(x, y), pattern(x, y)) << x << "," << y;
}

}  // namespace

TEST(MemoryTiers, DefaultLimitsFollowTheSpec) {
    const uint64_t ram = mem::system_ram();
    ASSERT_GT(ram, 0u);
    // Only meaningful without overrides in the environment.
    if (std::getenv("RASTERLOOM_MEM_HARD_MB") || std::getenv("RASTERLOOM_MEM_SOFT_MB") ||
        std::getenv("RASTERLOOM_MEM_TEST"))
        GTEST_SKIP() << "memory overrides in the environment";
    const mem::Limits l = mem::default_limits();
    EXPECT_EQ(l.hard, ram / 2);
    EXPECT_EQ(l.soft, ram / 50);
    EXPECT_EQ(l.scratch_cap, 4096ull << 20);
}

TEST(MemoryTiers, CompressedAndSpilledTilesReadBackIdentically) {
    LimitsScope ls(tiny());
    RgbaImage img = patterned(200, 130);  // 4 x 3 tiles, all distinct content
    const auto before = mem::stats();
    mem::evict_all(/*spill=*/false);
    auto s = mem::stats();
    EXPECT_GT(s.compressions, before.compressions);
    EXPECT_TRUE(!img.tile_ptr(0, 0)->is_hot());
    EXPECT_TRUE(img.tile_ptr(0, 0)->is_warm());
    expect_pattern(img);  // faults every tile back in from RAM

    mem::evict_all(/*spill=*/true);
    s = mem::stats();
    EXPECT_TRUE(img.tile_ptr(1, 1)->is_cold());
    EXPECT_GT(s.cold_bytes, 0u);
    EXPECT_GT(s.scratch_file_bytes, 0u);
    const auto faults = s.faults;
    expect_pattern(img);  // ... and now from the scratch file
    EXPECT_GE(mem::stats().faults, faults + 12);
}

TEST(MemoryTiers, SafePointTrimsToTheSoftLimit) {
    LimitsScope ls(tiny());
    RgbaImage img = patterned(256, 256);
    EXPECT_GT(mem::stats().hot_bytes, 0u);
    mem::safe_point();
    EXPECT_EQ(mem::stats().hot_bytes, 0u);   // soft = 0: nothing stays hot
    EXPECT_EQ(mem::stats().warm_bytes, 0u);  // warm = 0: everything spilled
    expect_pattern(img);
}

TEST(MemoryTiers, CopyOnWriteSurvivesEviction) {
    LimitsScope ls(tiny());
    RgbaImage a = patterned(130, 70);
    RgbaImage b = a;  // shares every cell
    mem::evict_all(true);
    EXPECT_TRUE(b.shares_tile(a, 0, 0));
    b.set(1, 1, Rgba8{9, 9, 9, 9});  // clones (0,0) from the cold cell
    EXPECT_FALSE(b.shares_tile(a, 0, 0));
    EXPECT_TRUE(b.shares_tile(a, 1, 0));
    EXPECT_EQ(a.get(1, 1), pattern(1, 1));
    EXPECT_EQ(b.get(1, 1), (Rgba8{9, 9, 9, 9}));
    // Sole owner writes in place and drops the stale compressed copy: evict again, read again.
    mem::evict_all(true);
    b.set(2, 2, Rgba8{7, 7, 7, 7});
    mem::evict_all(true);
    EXPECT_EQ(b.get(2, 2), (Rgba8{7, 7, 7, 7}));
    EXPECT_EQ(b.get(1, 1), (Rgba8{9, 9, 9, 9}));
    EXPECT_EQ(a.get(2, 2), pattern(2, 2));
}

TEST(MemoryTiers, HistoryUndoAcrossSpilledTiles) {
    LimitsScope ls(tiny());
    Document doc(128, 128, Rgba8{});
    doc.state().root.children.push_back(Node::make_raster("L", 128, 128));
    RgbaImage& px = doc.state().root.children[0].pixels;
    for (int i = 0; i < 128; ++i) px.set(i, i, Rgba8{static_cast<uint8_t>(i), 1, 2, 255});
    doc.push_history();  // safe point: everything compressed + spilled
    RgbaImage& px2 = doc.state().root.children[0].pixels;
    for (int i = 0; i < 128; ++i) px2.set(i, 127 - i, Rgba8{3, 4, 5, 255});
    doc.push_history();
    doc.undo(1);
    const RgbaImage& r = doc.state().root.children[0].pixels;
    for (int i = 0; i < 128; ++i) {
        EXPECT_EQ(r.get(i, 127 - i), (Rgba8{3, 4, 5, 255}));
        if (i != 127 - i) {
            EXPECT_EQ(r.get(i, i), (Rgba8{static_cast<uint8_t>(i), 1, 2, 255}));
        }
    }
    doc.undo(1);
    const RgbaImage& r2 = doc.state().root.children[0].pixels;
    for (int i = 0; i < 128; ++i) {
        EXPECT_EQ(r2.get(i, i), (Rgba8{static_cast<uint8_t>(i), 1, 2, 255}));
        if (i != 127 - i) {
            EXPECT_EQ(r2.get(i, 127 - i), Rgba8{});
        }
    }
}

TEST(MemoryTiers, SharedEmptyTileIsPinnedAndMutation13StillDense) {
    LimitsScope ls(tiny());
    RgbaImage a(1000, 700);
    mem::evict_all(true);
    EXPECT_TRUE(a.tile_ptr(3, 3)->pinned());
    EXPECT_TRUE(a.tile_ptr(3, 3)->is_hot());  // never evicted
    EXPECT_EQ(&a.tile(0, 0), &a.tile(15, 10));
    EXPECT_EQ(a.allocated_tiles(), 0u);
    mut::ScopedMutations m13({13});
    RgbaImage d(1000, 700);
    EXPECT_EQ(d.allocated_tiles(), d.total_tiles());
    mem::evict_all(true);
    EXPECT_EQ(d.allocated_tiles(), d.total_tiles());  // tiering never changes the tile count
}

TEST(MemoryTiers, HardLimitIsACleanError) {
    LimitsScope ls(tiny(/*hard=*/1ull << 20));  // 1 MiB = 64 RGBA tiles, minus what is live
    RgbaImage img(4096, 4096);
    bool threw = false;
    try {
        for (int ty = 0; ty < 64; ++ty)
            for (int tx = 0; tx < 64; ++tx) img.mutable_tile(tx, ty).px.fill(Rgba8{1, 2, 3, 4});
    } catch (const mem::MemoryError& e) {
        threw = true;
        EXPECT_NE(std::string(e.what()).find("hard limit 1 MiB"), std::string::npos) << e.what();
    }
    EXPECT_TRUE(threw);
    EXPECT_LE(mem::stats().hot_bytes + mem::stats().warm_bytes + mem::stats().reserved_bytes, 1ull << 20);
    EXPECT_THROW(mem::Reservation(2ull << 20, "a test plane"), mem::MemoryError);
}

TEST(MemoryTiers, ReservationChargesAndReleases) {
    LimitsScope ls(tiny(/*hard=*/64ull << 20));
    const uint64_t r0 = mem::stats().reserved_bytes;
    {
        mem::Reservation r(10ull << 20, "plane");
        EXPECT_EQ(mem::stats().reserved_bytes, r0 + (10ull << 20));
        mem::Reservation moved(std::move(r));
        EXPECT_EQ(mem::stats().reserved_bytes, r0 + (10ull << 20));
    }
    EXPECT_EQ(mem::stats().reserved_bytes, r0);
}

TEST(MemoryTiers, ScratchSpaceIsReusedAfterFree) {
    LimitsScope ls(tiny());
    const uint64_t base = mem::stats().scratch_file_bytes;
    {
        RgbaImage img = patterned(512, 256);
        mem::evict_all(true);
        EXPECT_GT(mem::stats().scratch_file_bytes, base);
    }
    // Every extent freed again: the high-water mark falls back.
    EXPECT_EQ(mem::stats().scratch_file_bytes, base);
}

TEST(MemoryTiers, ConcurrentFaultInIsSafe) {
    LimitsScope ls(tiny());
    const RgbaImage img = patterned(512, 512);
    mem::evict_all(true);
    std::atomic<int> bad{0};
    std::vector<std::thread> th;
    for (int t = 0; t < 8; ++t)
        th.emplace_back([&img, &bad, t] {
            mem::ReadScope rs;
            for (int k = 0; k < 4; ++k)
                for (int y = (t * 7) % 512; y < 512; y += 5)
                    for (int x = 0; x < 512; x += 3)
                        if (!(img.get(x, y) == pattern(x, y))) bad.fetch_add(1);
        });
    for (auto& t : th) t.join();
    EXPECT_EQ(bad.load(), 0);
}
