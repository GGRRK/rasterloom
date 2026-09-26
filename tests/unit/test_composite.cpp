// SPDX-License-Identifier: GPL-3.0-or-later
//
// COMPOSITE / ADJUST primitives (doc 10 §4) with hand-computed expectations.
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "core/adjust/adjustment.hpp"
#include "core/base/error.hpp"
#include "core/base/mutation.hpp"
#include "core/base/quant.hpp"
#include "core/base/rng.hpp"
#include "core/composite/composite.hpp"

using namespace rl;
using namespace rl::composite;

namespace {
std::shared_ptr<const adjust::Adjustment> invert() { return adjust::make_adjustment("invert", nullptr, "test"); }
}  // namespace

// §4.1 "Skipping is exact": c == 0 returns the backdrop bytes, for every mode, alpha and byte.
TEST(Composite, ZeroCoverageReturnsBackdropExactly) {
    const int alphas[] = {0, 1, 7, 128, 254, 255};
    for (int mi = 1; mi < kBlendModeCount; ++mi) {
        const auto mode = static_cast<BlendMode>(mi);
        for (int a : alphas)
            for (int v = 0; v < 256; ++v) {
                const Rgba8 bd = canonicalize(Rgba8{static_cast<uint8_t>(v), static_cast<uint8_t>(255 - v),
                                                    static_cast<uint8_t>((v * 7) & 255), static_cast<uint8_t>(a)});
                const Rgba8 src{static_cast<uint8_t>((v * 13) & 255), 3, 250, 255};
                ASSERT_EQ(composite_px(bd, src, 0.0, mode, 7, static_cast<uint32_t>(v), 3), bd)
                    << blend_mode_name(mode) << " a=" << a << " v=" << v;
            }
    }
}

TEST(Composite, FullCoverageNormalIsSource) {
    for (int v = 0; v < 256; ++v) {
        const Rgba8 bd{9, 9, 9, static_cast<uint8_t>(v)};
        const Rgba8 src{static_cast<uint8_t>(v), 128, 255, 17};  // source alpha byte is ignored
        EXPECT_EQ(composite_px(canonicalize(bd), src, 1.0, BlendMode::Norm, 0, 0, 0),
                  (Rgba8{static_cast<uint8_t>(v), 128, 255, 255}));
    }
}

TEST(Composite, NormalHalfOpacityHandComputed) {
    // ab = 1, as = 0.5: ao = 1, co = 0.5*1 + (1*0)*0.5 = 0.5 -> y = 127.5 -> 128 (half away from 0).
    EXPECT_EQ(composite_px(Rgba8{0, 0, 0, 255}, Rgba8{255, 255, 255, 255}, 0.5, BlendMode::Norm, 0, 0, 0),
              (Rgba8{128, 128, 128, 255}));
    // Transparent backdrop: ao = 0.5 -> alpha 128, colour = source exactly.
    EXPECT_EQ(composite_px(Rgba8{}, Rgba8{200, 100, 50, 255}, 0.5, BlendMode::Norm, 0, 0, 0),
              (Rgba8{200, 100, 50, 128}));
}

TEST(Composite, MultiplyPartialBackdropAlphaHandComputed) {
    const Rgba8 bd{200, 100, 0, 128}, src{100, 255, 50, 255};
    const double ab = dec(128), as = 0.6;
    const double ao = as + (ab * (1.0 - as));
    uint8_t expect[3];
    const int b3[3] = {200, 100, 0}, s3[3] = {100, 255, 50};
    for (int ch = 0; ch < 3; ++ch) {
        const double cb = dec(b3[ch]), cs = dec(s3[ch]);
        const double bl = clamp01(cb * cs);
        const double mixed = ((1.0 - ab) * cs) + (ab * bl);
        const double co = ((as * mixed) + ((ab * cb) * (1.0 - as))) / ao;
        expect[ch] = q(co);
    }
    EXPECT_EQ(composite_px(bd, src, 0.6, BlendMode::Mul, 0, 0, 0), (Rgba8{expect[0], expect[1], expect[2], q(ao)}));
    // Mutations 2 (premultiplied operands) and 3 (linear light) change this pixel.
    {
        mut::ScopedMutations m({2});
        EXPECT_NE(composite_px(bd, src, 0.6, BlendMode::Mul, 0, 0, 0), (Rgba8{expect[0], expect[1], expect[2], q(ao)}));
    }
    {
        mut::ScopedMutations m({3});
        EXPECT_NE(composite_px(bd, src, 0.6, BlendMode::Mul, 0, 0, 0), (Rgba8{expect[0], expect[1], expect[2], q(ao)}));
    }
}

TEST(Composite, TransparentResultIsCanonical) {
    // ao == 0
    EXPECT_EQ(composite_px(Rgba8{}, Rgba8{255, 0, 0, 255}, 0.0, BlendMode::Scrn, 0, 0, 0), Rgba8{});
    // ao > 0 but q(ao) == 0: canonical (0,0,0,0); mutation 22 keeps the colour.
    EXPECT_EQ(composite_px(Rgba8{}, Rgba8{255, 128, 0, 255}, 0.001, BlendMode::Norm, 0, 0, 0), Rgba8{});
    mut::ScopedMutations m({22});
    EXPECT_EQ(composite_px(Rgba8{}, Rgba8{255, 128, 0, 255}, 0.001, BlendMode::Norm, 0, 0, 0),
              (Rgba8{255, 128, 0, 0}));
}

TEST(Composite, DissolveUsesCanvasCoordinatesAndStream5) {
    const Rgba8 bd{0, 0, 0, 255}, src{255, 255, 255, 255};
    int kept = 0;
    for (uint32_t y = 0; y < 64; ++y)
        for (uint32_t x = 0; x < 64; ++x) {
            const bool keep = unit(pixel_hash(7, x, y, 5)) < 0.5;
            const Rgba8 out = composite_px(bd, src, 0.5, BlendMode::Diss, 7, x, y);
            EXPECT_EQ(out, keep ? src : bd);
            kept += keep ? 1 : 0;
        }
    EXPECT_GT(kept, 1800);
    EXPECT_LT(kept, 2300);
    // c = 1 always kept, c = 0 always dropped.
    EXPECT_EQ(composite_px(bd, src, 1.0, BlendMode::Diss, 7, 1, 1), src);
    EXPECT_EQ(composite_px(bd, src, 0.0, BlendMode::Diss, 7, 1, 1), bd);
}

TEST(Adjust, InvertKeepsAlphaAndLerps) {
    const auto inv = invert();
    EXPECT_EQ(adjust_px(Rgba8{10, 20, 30, 77}, *inv, 1.0, BlendMode::Norm, 0, 0, 0), (Rgba8{245, 235, 225, 77}));
    // c = 0.5: co = 0.5*cb + 0.5*(1-cb)... evaluated as ((1-c)*cb) + (c*bl)
    const double cb = dec(10), bl = dec(245);
    EXPECT_EQ(adjust_px(Rgba8{10, 20, 30, 77}, *inv, 0.5, BlendMode::Norm, 0, 0, 0).r, q((0.5 * cb) + (0.5 * bl)));
    // Nothing to recolour on a transparent backdrop.
    EXPECT_EQ(adjust_px(Rgba8{}, *inv, 1.0, BlendMode::Norm, 0, 0, 0), Rgba8{});
    // Non-Normal mode: bl = clamp01(B(cb, adj)).
    const Rgba8 m = adjust_px(Rgba8{100, 100, 100, 255}, *inv, 1.0, BlendMode::Mul, 0, 0, 0);
    EXPECT_EQ(m.r, q(dec(100) * dec(155)));
}

TEST(Adjust, UnknownAndUnimplementedTypes) {
    EXPECT_THROW(adjust::make_adjustment("sepia", nullptr, "t"), ScriptError);
    // All eight doc-20 types are implemented (adjustments lane); defaults build.
    EXPECT_NO_THROW(adjust::make_adjustment("levels", nullptr, "t"));
    const nlohmann::json p = {{"amount", 1}};
    EXPECT_THROW(adjust::make_adjustment("invert", &p, "t"), ScriptError);
}
