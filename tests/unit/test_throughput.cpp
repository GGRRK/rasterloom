// SPDX-License-Identifier: GPL-3.0-or-later
//
// Throughput: composite 1000 64x64 tiles in Normal mode, single-threaded, scalar binary64 path.
#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>

#include "core/composite/render.hpp"
#include "test_util.hpp"

using namespace rl;

TEST(Throughput, NormalComposite1000Tiles) {
    // 40 x 25 tiles = 1000 tiles. Bottom layer: opaque noise; top layer: Normal, random alpha, 0.8.
    auto res = script::run_script_text(rltest::make_script(64 * 40, 64 * 25, R"(
        {"op":"add_layer","id":"b","fill":"noise","seed":1,"alpha":255},
        {"op":"add_layer","id":"t","fill":"noise","seed":2,"alpha":"random"},
        {"op":"set_opacity","layer":"t","value":0.8})"));
    const DocState& s = res.doc->state();
    ASSERT_EQ(s.root.children[1].pixels.allocated_tiles(), 1000u);

    RgbaTile out;
    uint64_t checksum = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (int ty = 0; ty < 25; ++ty)
        for (int tx = 0; tx < 40; ++tx) {
            composite::render_tile(s, tx, ty, /*onto_bg=*/true, out);
            checksum += out.px[static_cast<size_t>((tx * 7 + ty) & 4095)].r;
        }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    // Two layers per tile: 2000 tile composites, 8.19 M pixel composites.
    std::printf("[throughput] 1000 tiles x 2 Normal layers: %.3f s (%.1f Mpx/s per layer), checksum %llu\n", secs,
                (2.0 * 1000 * 4096) / secs / 1e6, static_cast<unsigned long long>(checksum));
    EXPECT_LT(secs, 2.0);
}
