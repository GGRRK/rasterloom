// SPDX-License-Identifier: GPL-3.0-or-later
//
// Blend-function guards, ties and mutation hooks (doc 10 §3, §13).
#include <gtest/gtest.h>

#include <cmath>

#include "core/base/mutation.hpp"
#include "core/base/quant.hpp"
#include "core/composite/blend.hpp"

using namespace rl;
using namespace rl::composite;

namespace {
double sep(BlendMode m, int b, int s) { return blend_separable(m, dec(b), dec(s), b, s); }
Vec3 vec(BlendMode m, Rgba8 b, Rgba8 s) {
    const int bb[3] = {b.r, b.g, b.b}, sb[3] = {s.r, s.g, s.b};
    return blend_vector(m, Vec3{dec(b.r), dec(b.g), dec(b.b)}, Vec3{dec(s.r), dec(s.g), dec(s.b)}, bb, sb);
}
}  // namespace

TEST(Blend, ColorDodgeGuardOrder) {
    EXPECT_EQ(ColorDodge(0.0, 1.0), 0.0);  // cb == 0 wins
    EXPECT_EQ(ColorDodge(0.3, 1.0), 1.0);
    EXPECT_EQ(ColorDodge(0.2, 0.5), std::min(1.0, 0.2 / (1.0 - 0.5)));
    mut::ScopedMutations m({16});
    EXPECT_EQ(ColorDodge(0.0, 1.0), 1.0);
    EXPECT_EQ(sep(BlendMode::Div, 0, 255), 1.0);
    EXPECT_EQ(sep(BlendMode::VLit, 0, 255), 1.0);  // vLit inherits the dodge guard
}

TEST(Blend, ColorBurnGuardOrder) {
    EXPECT_EQ(ColorBurn(1.0, 0.0), 1.0);  // cb == 1 wins over s == 0
    EXPECT_EQ(ColorBurn(0.5, 0.0), 0.0);
    EXPECT_EQ(sep(BlendMode::Idiv, 255, 0), 1.0);
    EXPECT_EQ(sep(BlendMode::VLit, 255, 0), 1.0);
    EXPECT_EQ(sep(BlendMode::VLit, 254, 0), 0.0);
    EXPECT_EQ(sep(BlendMode::VLit, 0, 255), 0.0);
    EXPECT_EQ(sep(BlendMode::VLit, 1, 255), 1.0);
}

TEST(Blend, DivideGuardOrder) {
    EXPECT_EQ(sep(BlendMode::Fdiv, 0, 0), 0.0);
    EXPECT_EQ(sep(BlendMode::Fdiv, 1, 0), 1.0);
    EXPECT_EQ(sep(BlendMode::Fdiv, 100, 200), std::min(1.0, dec(100) / dec(200)));
    mut::ScopedMutations m({21});
    EXPECT_EQ(sep(BlendMode::Fdiv, 0, 0), 1.0);
}

TEST(Blend, HardMixThresholdIsSumAtLeast255) {
    EXPECT_EQ(sep(BlendMode::HMix, 100, 155), 1.0);
    EXPECT_EQ(sep(BlendMode::HMix, 100, 154), 0.0);
    EXPECT_EQ(sep(BlendMode::HMix, 0, 255), 1.0);
    mut::ScopedMutations m({17});
    EXPECT_EQ(sep(BlendMode::HMix, 100, 155), 0.0);
    EXPECT_EQ(sep(BlendMode::HMix, 100, 156), 1.0);
}

TEST(Blend, DarkerLighterColorTiesKeepBackdrop) {
    const Rgba8 b{0x3B, 0, 0, 255}, s{0, 0x1E, 0, 255};  // both integer luma 1770
    Vec3 d = vec(BlendMode::DkCl, b, s);
    EXPECT_EQ(d.r, dec(0x3B));
    EXPECT_EQ(d.g, 0.0);
    Vec3 l = vec(BlendMode::LgCl, b, s);
    EXPECT_EQ(l.r, dec(0x3B));
    {
        mut::ScopedMutations m({18});
        EXPECT_EQ(vec(BlendMode::DkCl, b, s).g, dec(0x1E));
        EXPECT_EQ(vec(BlendMode::LgCl, b, s).g, dec(0x1E));
    }
    // Whole-vector selection, not per channel: backdrop luma 12210 > source luma 8790 -> source.
    const Rgba8 bd{10, 200, 10, 255}, src{200, 10, 200, 255};
    Vec3 v = vec(BlendMode::DkCl, bd, src);
    EXPECT_EQ(v.r, dec(200));
    EXPECT_EQ(v.g, dec(10));
    EXPECT_EQ(v.b, dec(200));
    Vec3 w = vec(BlendMode::LgCl, bd, src);
    EXPECT_EQ(w.g, dec(200));
}

TEST(Blend, SoftLightIsW3CWithDOnBackdrop) {
    // cs <= 0.5 branch
    const double cb = dec(51), cs = dec(100);
    EXPECT_EQ(SoftLight(cb, cs), cb - (((1.0 - (2.0 * cs)) * cb) * (1.0 - cb)));
    // cs > 0.5, cb <= 0.25 -> polynomial D
    const double cb2 = dec(40), cs2 = dec(200);
    const double D2 = ((((16.0 * cb2) - 12.0) * cb2) + 4.0) * cb2;
    EXPECT_EQ(SoftLight(cb2, cs2), cb2 + (((2.0 * cs2) - 1.0) * (D2 - cb2)));
    // cs > 0.5, cb > 0.25 -> sqrt
    const double cb3 = dec(180), cs3 = dec(220);
    EXPECT_EQ(SoftLight(cb3, cs3), cb3 + (((2.0 * cs3) - 1.0) * (std::sqrt(cb3) - cb3)));
    const double w3c = SoftLight(cb3, cs3);
    mut::ScopedMutations m({0});
    EXPECT_NE(SoftLight(cb3, cs3), w3c);
    EXPECT_EQ(SoftLight(cb3, cs3), ((1.0 - cb3) * (cb3 * cs3)) + (cb3 * Screen(cb3, cs3)));
}

TEST(Blend, OverlayIsHardLightWithArgumentsSwapped) {
    for (int b = 0; b < 256; b += 17)
        for (int s = 0; s < 256; s += 13)
            EXPECT_EQ(sep(BlendMode::Over, b, s), HardLight(dec(s), dec(b)));
    EXPECT_EQ(sep(BlendMode::HLit, 100, 200), 1.0 - ((1.0 - dec(100)) * (1.0 - ((2.0 * dec(200)) - 1.0))));
}

TEST(Blend, SeparableFormulasAsWritten) {
    const double cb = dec(77), cs = dec(201);
    EXPECT_EQ(sep(BlendMode::Scrn, 77, 201), 1.0 - ((1.0 - cb) * (1.0 - cs)));
    EXPECT_EQ(sep(BlendMode::Smud, 77, 201), (cb + cs) - ((2.0 * cb) * cs));
    EXPECT_EQ(sep(BlendMode::Lbrn, 77, 201), std::max(0.0, (cb + cs) - 1.0));
    EXPECT_EQ(sep(BlendMode::Lbrn, 10, 20), 0.0);
    EXPECT_EQ(sep(BlendMode::Lddg, 200, 200), 1.0);
    EXPECT_EQ(sep(BlendMode::Fsub, 77, 201), 0.0);   // backdrop minus source, floored
    EXPECT_EQ(sep(BlendMode::Fsub, 201, 77), dec(201) - dec(77));
    EXPECT_EQ(sep(BlendMode::Diff, 77, 201), std::abs(cb - cs));
    EXPECT_EQ(sep(BlendMode::LLit, 77, 201), clamp01((cb + (2.0 * cs)) - 1.0));
    EXPECT_EQ(sep(BlendMode::PLit, 77, 20), std::min(cb, 2.0 * dec(20)));
    EXPECT_EQ(sep(BlendMode::PLit, 77, 201), std::max(cb, (2.0 * cs) - 1.0));
}

TEST(Blend, SetSatFirstIndexTieRule) {
    const Vec3 c{0.5, 0.5, 0.1};
    const Vec3 out = SetSat(c, 0.3);
    EXPECT_EQ(out.r, 0.3);  // imax = r (first of the tied maxima) gets exactly s
    EXPECT_EQ(out.g, ((0.5 - 0.1) * 0.3) / (0.5 - 0.1));
    EXPECT_EQ(out.b, 0.0);
    const Vec3 grey = SetSat(Vec3{0.4, 0.4, 0.4}, 0.7);
    EXPECT_EQ(grey.r, 0.0);
    EXPECT_EQ(grey.g, 0.0);
    EXPECT_EQ(grey.b, 0.0);
}

TEST(Blend, LumWeightsAndMutation1) {
    const Vec3 c{0.2, 0.5, 0.9};
    EXPECT_EQ(Lum(c), ((0.3 * 0.2) + (0.59 * 0.5)) + (0.11 * 0.9));
    mut::ScopedMutations m({1});
    EXPECT_EQ(Lum(c), ((0.2126 * 0.2) + (0.7152 * 0.5)) + (0.0722 * 0.9));
}

TEST(Blend, NonSeparableStayInRangeAfterClamp) {
    const int bb[3] = {255, 0, 0}, sb[3] = {0, 0, 255};
    for (BlendMode m : {BlendMode::Hue, BlendMode::Sat, BlendMode::Colr, BlendMode::Lum}) {
        const Vec3 v = blend_clamped(m, Vec3{1.0, 0.0, 0.0}, Vec3{0.0, 0.0, 1.0}, bb, sb);
        for (int ch = 0; ch < 3; ++ch) {
            EXPECT_GE(v[ch], 0.0);
            EXPECT_LE(v[ch], 1.0);
            EXPECT_FALSE(std::isnan(v[ch]));
        }
    }
}

TEST(Blend, ModeNamesRoundTrip) {
    for (int i = 0; i < kBlendModeCount; ++i) {
        const auto m = static_cast<BlendMode>(i);
        EXPECT_EQ(parse_blend_mode(blend_mode_name(m)), m);
    }
    EXPECT_FALSE(parse_blend_mode("mul "));
    EXPECT_FALSE(parse_blend_mode("MUL"));
    EXPECT_FALSE(parse_blend_mode("isolated"));
}
