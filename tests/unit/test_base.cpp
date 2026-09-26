// SPDX-License-Identifier: GPL-3.0-or-later
#include <gtest/gtest.h>

#include "core/base/color.hpp"
#include "core/base/mutation.hpp"
#include "core/base/quant.hpp"
#include "core/base/rng.hpp"

using namespace rl;

TEST(Quant, DecodeIsADivision) {
    for (int v = 0; v < 256; ++v) EXPECT_EQ(dec(v), static_cast<double>(v) / 255.0);
    EXPECT_EQ(dec(0), 0.0);
    EXPECT_EQ(dec(255), 1.0);
}

TEST(Quant, RoundTripsEveryByte) {
    for (int v = 0; v < 256; ++v) EXPECT_EQ(q(dec(v)), v);
}

TEST(Quant, RoundsHalfAwayFromZero) {
    // y = x * 255 exactly .5 -> rounds up (never half-to-even, never truncation).
    EXPECT_EQ(q(0.5), 128);            // 127.5 -> 128
    EXPECT_EQ(q(0.5 / 255.0 * 1.0), 1);  // y ~ 0.5
    EXPECT_EQ(q(2.5 / 255.0), 3);        // half-to-even would give 2
    EXPECT_EQ(q(0.4 / 255.0), 0);
}

TEST(Quant, Clamps) {
    EXPECT_EQ(q(-3.0), 0);
    EXPECT_EQ(q(7.0), 255);
    EXPECT_EQ(clamp01(-0.1), 0.0);
    EXPECT_EQ(clamp01(1.1), 1.0);
}

TEST(Quant, Mutation14Truncates) {
    EXPECT_EQ(q(0.5), 128);
    mut::ScopedMutations m({14});
    EXPECT_EQ(q(0.5), 127);
}

TEST(Quant, Canonicalize) {
    EXPECT_EQ(canonicalize(Rgba8{10, 20, 30, 0}), (Rgba8{0, 0, 0, 0}));
    EXPECT_EQ(canonicalize(Rgba8{10, 20, 30, 1}), (Rgba8{10, 20, 30, 1}));
}

TEST(Rng, SplitMix64KnownValues) {
    // Reference outputs of the standard splitmix64 generator seeded with 0 (state increments by
    // the golden gamma before mixing): 0xE220A8397B1DCDAF, then 0x6E789E6AA1B965F4.
    EXPECT_EQ(splitmix64(0), 0xE220A8397B1DCDAFULL);
    EXPECT_EQ(splitmix64(0x9E3779B97F4A7C15ULL), 0x6E789E6AA1B965F4ULL);
}

TEST(Rng, UnitIsExactAndInRange) {
    EXPECT_EQ(unit(0), 0.0);
    EXPECT_LT(unit(~0ULL), 1.0);
    EXPECT_EQ(unit(~0ULL), 1.0 - 0x1.0p-53);
    EXPECT_EQ(unit(1ULL << 11), 0x1.0p-53);
}

TEST(Rng, PixelHashMatchesDefinition) {
    const uint64_t seed = 7, k = 5;
    const uint32_t x = 3, y = 9;
    const uint64_t expect =
        splitmix64(seed ^ splitmix64((uint64_t{y} << 32) | x) ^ (k * 0xD1B54A32D192ED03ULL));
    EXPECT_EQ(pixel_hash(seed, x, y, k), expect);
    EXPECT_NE(pixel_hash(seed, x, y, 5), pixel_hash(seed, y, x, 5));  // coordinates not symmetric
}

TEST(Color, ParsesHex) {
    EXPECT_EQ(*parse_hex_color("#ff8000"), (Rgba8{255, 128, 0, 255}));
    EXPECT_EQ(*parse_hex_color("#FF800080"), (Rgba8{255, 128, 0, 128}));
    EXPECT_FALSE(parse_hex_color("ff8000"));
    EXPECT_FALSE(parse_hex_color("#ff80"));
    EXPECT_FALSE(parse_hex_color("#gg8000"));
    EXPECT_FALSE(parse_hex_color("#ff8000 "));
}

TEST(Mutation, TableHasFortyDescribedIds) {
    for (int id = 0; id < mut::kCount; ++id) {
        EXPECT_GT(std::string(mut::description(id)).size(), 10u) << id;
        EXPECT_GT(std::string(mut::owner(id)).size(), 3u) << id;
    }
    EXPECT_NE(std::string(mut::description(13)).find("dense tile grid"), std::string::npos);
}

TEST(Mutation, ParseList) {
    EXPECT_EQ(mut::parse_list("3"), std::vector<int>({3}));
    EXPECT_EQ(mut::parse_list("0, 13,45"), std::vector<int>({0, 13, 45}));
    EXPECT_THROW(mut::parse_list("46"), std::invalid_argument);
    EXPECT_THROW(mut::parse_list("-1"), std::invalid_argument);
    EXPECT_THROW(mut::parse_list("1,,2"), std::invalid_argument);
    EXPECT_THROW(mut::parse_list(""), std::invalid_argument);
}

TEST(Mutation, ScopedRestores) {
    ASSERT_TRUE(mut::active_list().empty());
    {
        mut::ScopedMutations m({2, 5});
        EXPECT_TRUE(mut::active(2));
        EXPECT_TRUE(mut::active(5));
        EXPECT_FALSE(mut::active(3));
    }
    EXPECT_TRUE(mut::active_list().empty());
}
