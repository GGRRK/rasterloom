// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for docs/math/20-adjustments-filters.md Part A (adjustments).
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "core/adjust/adjust_math.hpp"
#include "core/adjust/adjustment.hpp"
#include "core/base/error.hpp"
#include "core/base/mutation.hpp"
#include "core/base/quant.hpp"

using namespace rl;
using namespace rl::adjust;
using Json = nlohmann::json;

namespace {

std::shared_ptr<const Adjustment> make(const std::string& type, const Json& params) {
    return make_adjustment(type, &params, "test");
}

Rgba8 run(const Adjustment& a, Rgba8 c) {
    a.apply(c.r, c.g, c.b);
    return c;
}

}  // namespace

TEST(AdjustLevels, IdentityLutIsExact) {
    const Lut8 lut = levels_lut(LevelsSetting{}, LevelsSetting{});
    for (int i = 0; i < 256; ++i) EXPECT_EQ(lut[static_cast<size_t>(i)], i);
}

TEST(AdjustLevels, GammaAboveOneBrightensAndMutation24Darkens) {
    LevelsSetting s;
    s.gamma = 2.2;
    const Lut8 good = levels_lut(LevelsSetting{}, s);
    EXPECT_GT(good[128], 128);
    EXPECT_EQ(good[0], 0);
    EXPECT_EQ(good[255], 255);
    mut::ScopedMutations m({24});
    const Lut8 bad = levels_lut(LevelsSetting{}, s);
    EXPECT_LT(bad[128], 128);
}

TEST(AdjustLevels, InputClipAndCrossedOutput) {
    LevelsSetting s;
    s.in_black = 30;
    s.in_white = 220;
    const Lut8 lut = levels_lut(LevelsSetting{}, s);
    for (int i = 0; i <= 30; ++i) EXPECT_EQ(lut[static_cast<size_t>(i)], 0);
    for (int i = 220; i < 256; ++i) EXPECT_EQ(lut[static_cast<size_t>(i)], 255);
    LevelsSetting inv;
    inv.out_black = 255;
    inv.out_white = 0;
    const Lut8 l2 = levels_lut(LevelsSetting{}, inv);
    for (int i = 0; i < 256; ++i) EXPECT_EQ(l2[static_cast<size_t>(i)], 255 - i);
}

TEST(AdjustLevels, ChannelSettingRoutedToItsChannel) {
    auto a = make("levels", Json{{"b", {{"out_white", 200}}}});
    const Rgba8 o = run(*a, Rgba8{255, 255, 255, 255});
    EXPECT_EQ(o.r, 255);
    EXPECT_EQ(o.g, 255);
    EXPECT_EQ(o.b, 200);
}

TEST(AdjustCurves, IdentityIsExact) {
    const CurveSpline id(identity_curve());
    EXPECT_EQ(id.slopes()[0], 1.0);
    for (int i = 0; i < 256; ++i) EXPECT_EQ(id.eval(i), static_cast<double>(i));
    const Lut8 lut = curves_lut(id, id);
    for (int i = 0; i < 256; ++i) EXPECT_EQ(lut[static_cast<size_t>(i)], i);
}

TEST(AdjustCurves, NaturalSplinePassesThroughPointsWithZeroEndCurvature) {
    const CurvePoints pts = {{{0, 0}}, {{17, 40}}, {{34, 20}}, {{51, 70}}, {{68, 50}}, {{85, 110}}, {{102, 90}},
                             {{119, 150}}, {{136, 130}}, {{153, 190}}, {{170, 170}}, {{187, 220}}, {{204, 200}},
                             {{221, 245}}, {{238, 230}}, {{255, 255}}};
    const CurveSpline s(pts);
    EXPECT_EQ(s.second_derivatives().front(), 0.0);
    EXPECT_EQ(s.second_derivatives().back(), 0.0);
    for (const auto& p : pts) EXPECT_NEAR(s.eval(p[0]), p[1], 1e-9);
    // Solve check: the spline's first derivative is continuous at an interior knot.
    const double eps = 1e-6;
    const double left = (s.eval(85.0) - s.eval(85.0 - eps)) / eps;
    const double right = (s.eval(85.0 + eps) - s.eval(85.0)) / eps;
    EXPECT_NEAR(left, right, 1e-3);
}

TEST(AdjustCurves, FlatExtensionAndMutation25) {
    const CurveSpline s({{{48, 20}}, {{100, 160}}, {{208, 230}}});
    EXPECT_EQ(s.eval(0.0), 20.0);
    EXPECT_EQ(s.eval(10.0), 20.0);
    EXPECT_EQ(s.eval(255.0), 230.0);
    mut::ScopedMutations m({25});
    EXPECT_NE(s.eval(0.0), 20.0);
}

TEST(AdjustBrightnessContrast, EdgeCases) {
    BrightnessContrastParams p;
    p.contrast = 1.0;
    const Lut8 cut = brightness_contrast_lut(p);
    for (int i = 0; i < 256; ++i) EXPECT_EQ(cut[static_cast<size_t>(i)], i < 128 ? 0 : 255) << i;
    p.contrast = -1.0;
    const Lut8 flat = brightness_contrast_lut(p);
    for (int i = 0; i < 256; ++i) EXPECT_EQ(flat[static_cast<size_t>(i)], 128);
    const Lut8 id = brightness_contrast_lut(BrightnessContrastParams{});
    for (int i = 0; i < 256; ++i) EXPECT_EQ(id[static_cast<size_t>(i)], i);
    p = BrightnessContrastParams{};
    p.brightness = 1.0;  // bh = 0.5: black -> q(0.5), white stays white
    const Lut8 br = brightness_contrast_lut(p);
    EXPECT_EQ(br[0], 128);
    EXPECT_EQ(br[255], 255);
}

// Doc 20 A4 normative property: with hue 0, saturation 0, lightness 0 the HSL round trip is the
// identity over all 2^24 colours.
TEST(AdjustHueSaturation, ZeroParamsRoundTripIsIdentityExhaustive) {
    const HueSatScalars k = hue_sat_scalars(HueSaturationParams{});
    int bad = 0;
    for (int r = 0; r < 256; ++r)
        for (int g = 0; g < 256; ++g)
            for (int b = 0; b < 256; ++b) {
                uint8_t R = static_cast<uint8_t>(r), G = static_cast<uint8_t>(g), B = static_cast<uint8_t>(b);
                hue_sat_pixel(k, R, G, B);
                if (R != r || G != g || B != b) ++bad;
            }
    EXPECT_EQ(bad, 0);
}

TEST(AdjustHueSaturation, HueShiftDirectionAndMutation26) {
    auto a = make("hue_saturation", Json{{"hue", 120.0}});
    EXPECT_EQ(run(*a, Rgba8{255, 0, 0, 255}), (Rgba8{0, 255, 0, 255}));  // red + 120 = green
    auto b = make("hue_saturation", Json{{"hue", -120.0}});
    EXPECT_EQ(run(*b, Rgba8{255, 0, 0, 255}), (Rgba8{0, 0, 255, 255}));  // red - 120 = blue
    mut::ScopedMutations m({26});
    auto c = make("hue_saturation", Json{{"hue", 120.0}});
    EXPECT_EQ(run(*c, Rgba8{255, 0, 0, 255}), (Rgba8{0, 0, 255, 255}));
}

TEST(AdjustHueSaturation, ColorizeDefaultsAndRanges) {
    const Json colorize = {{"colorize", true}};
    const auto p = parse_adjustment_params("hue_saturation", &colorize, "t");
    EXPECT_EQ(std::get<HueSaturationParams>(p).saturation, 25.0);
    const Json bad_hue = {{"hue", 200.0}};
    EXPECT_THROW(make("hue_saturation", bad_hue), ScriptError);
    EXPECT_NO_THROW(make("hue_saturation", Json{{"colorize", true}, {"hue", 360.0}}));
    EXPECT_THROW(make("hue_saturation", Json({{"colorize", true}, {"saturation", -1.0}})), ScriptError);
}

TEST(AdjustBlackWhite, PrimariesSecondariesAndGreys) {
    auto a = make("black_white", Json::object());
    EXPECT_EQ(run(*a, Rgba8{255, 0, 0, 255}).r, q(0.4 * 255.0 / 255.0));
    EXPECT_EQ(run(*a, Rgba8{255, 255, 0, 255}).r, q(153.0 / 255.0));  // yellows 60 %
    EXPECT_EQ(run(*a, Rgba8{255, 255, 255, 255}), (Rgba8{255, 255, 255, 255}));
    EXPECT_EQ(run(*a, Rgba8{77, 77, 77, 255}), (Rgba8{77, 77, 77, 255}));
    // Tinted grey keeps the grey as HSL lightness: white stays white, black stays black.
    auto t = make("black_white", Json{{"tint", "#E1D3B3"}});
    EXPECT_EQ(run(*t, Rgba8{255, 255, 255, 255}), (Rgba8{255, 255, 255, 255}));
    EXPECT_EQ(run(*t, Rgba8{0, 0, 0, 255}), (Rgba8{0, 0, 0, 255}));
    EXPECT_THROW(make("black_white", Json{{"tint", "#e1d3b3ff"}}), ScriptError);
    EXPECT_NO_THROW(make("black_white", Json{{"tint", nullptr}}));
}

TEST(AdjustPosterizeThreshold, Luts) {
    const Lut8 p2 = posterize_lut(2);
    for (int i = 0; i < 256; ++i) EXPECT_EQ(p2[static_cast<size_t>(i)], i < 128 ? 0 : 255);
    const Lut8 p3 = posterize_lut(3);  // levels 0, 128 (q(0.5)), 255
    EXPECT_EQ(p3[63], 0);
    EXPECT_EQ(p3[64], 128);
    EXPECT_EQ(p3[191], 128);
    EXPECT_EQ(p3[192], 255);
    {
        mut::ScopedMutations m({27});
        EXPECT_NE(posterize_lut(4), [] {
            mut::ScopedMutations none({});
            return posterize_lut(4);
        }());
    }
    EXPECT_EQ(threshold_value(255, 255, 255, 255), 255);
    EXPECT_EQ(threshold_value(1, 0, 0, 0), 0);
    // Y8 = (299*128 + 500) // 1000 = 38 for pure half red.
    EXPECT_EQ(threshold_value(38, 128, 0, 0), 255);
    EXPECT_EQ(threshold_value(39, 128, 0, 0), 0);
}

TEST(AdjustParams, StrictValidation) {
    EXPECT_THROW(make("levels", Json{{"rgb", {{"in_black", 200}, {"in_white", 200}}}}), ScriptError);
    EXPECT_NO_THROW(make("levels", Json{{"rgb", {{"in_black", 254}}}}));
    EXPECT_THROW(make("levels", Json{{"rgb", {{"gamma", 10.0}}}}), ScriptError);
    EXPECT_THROW(make("levels", Json{{"rgb", {{"in_black", 2.0}}}}), ScriptError);  // not a JSON integer
    EXPECT_THROW(make("levels", Json{{"rgbx", Json::object()}}), ScriptError);
    EXPECT_THROW(make("curves", Json{{"rgb", {{0, 0}, {10, 5}, {10, 9}}}}), ScriptError);
    EXPECT_THROW(make("curves", Json{{"rgb", Json::array({Json::array({0, 0})})}}), ScriptError);
    EXPECT_THROW(make("curves", Json{{"rgb", Json::object()}}), ScriptError);
    EXPECT_THROW(make("posterize", Json{{"levels", 1}}), ScriptError);
    EXPECT_THROW(make("threshold", Json{{"level", 0}}), ScriptError);
    EXPECT_THROW(make("brightness_contrast", Json{{"brightness", 1.5}}), ScriptError);
    EXPECT_THROW(make("sepia", Json::object()), ScriptError);
}

TEST(AdjustParams, JsonRoundTripAllTypes) {
    const std::vector<std::pair<std::string, Json>> cases = {
        {"levels", Json{{"r", {{"in_black", 40}, {"gamma", 1.8}}}, {"rgb", {{"out_black", 255}, {"out_white", 3}}}}},
        {"curves", Json{{"g", {{0, 30}, {128, 100}, {255, 255}}}}},
        {"brightness_contrast", Json{{"brightness", -0.6}, {"contrast", 0.4}}},
        {"hue_saturation", Json{{"colorize", true}, {"hue", 200.0}, {"saturation", 60.0}, {"lightness", 10.0}}},
        {"black_white", Json{{"reds", -50.0}, {"tint", "#e1d3b3"}}},
        {"invert", Json::object()},
        {"posterize", Json{{"levels", 7}}},
        {"threshold", Json{{"level", 99}}},
    };
    for (const auto& [type, params] : cases) {
        const auto a = make(type, params);
        EXPECT_STREQ(a->type(), type.c_str());
        EXPECT_STREQ(type_name(a->params()), type.c_str());
        const Json j = params_to_json(a->params());
        const auto b = make(type, j);
        for (int v = 0; v < 256; v += 5)
            for (int w = 0; w < 256; w += 51) {
                const Rgba8 c{static_cast<uint8_t>(v), static_cast<uint8_t>(w), static_cast<uint8_t>(255 - v), 255};
                EXPECT_EQ(run(*a, c), run(*b, c)) << type;
            }
        EXPECT_EQ(params_to_json(b->params()), j) << type;
    }
}

TEST(AdjustParams, TypedBuilderValidates) {
    LevelsParams p;
    p.rgb.in_black = 100;
    p.rgb.in_white = 50;
    EXPECT_THROW(make_adjustment(AdjustmentParams(p)), ScriptError);
    ThresholdParams t;
    t.level = 10;
    EXPECT_STREQ(make_adjustment(AdjustmentParams(t))->type(), "threshold");
}
