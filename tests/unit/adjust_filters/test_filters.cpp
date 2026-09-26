// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for docs/math/20-adjustments-filters.md Part B (filters and the B0 framework).
#include <gtest/gtest.h>

#include <chrono>

#include "../test_util.hpp"
#include "core/base/error.hpp"
#include "core/base/mutation.hpp"
#include "core/base/quant.hpp"
#include "core/base/rng.hpp"
#include "core/filters/filters.hpp"
#include "core/script/engine.hpp"

using namespace rl;
using namespace rl::filters;

namespace {

Image noise_image(int w, int h, uint64_t seed) {
    Image img(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const uint64_t v = pixel_hash(seed, static_cast<uint32_t>(x), static_cast<uint32_t>(y), 0);
            Rgba8 p{static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16),
                    static_cast<uint8_t>(v >> 24)};
            if ((v >> 32) % 5 == 0) p.a = 0;
            img.at(x, y) = canonicalize(p);
        }
    return img;
}

// B1 written as the direct sum, straight from the doc (no running sum), for comparison.
Image gaussian_direct(const Image& src, double radius, Edge edge) {
    const int W = src.w, H = src.h;
    const auto widths = box_widths(radius);
    std::array<std::vector<int64_t>, 4> P;
    for (auto& p : P) p.resize(src.px.size());
    for (size_t i = 0; i < src.px.size(); ++i) {
        const Rgba8 c = src.px[i];
        P[0][i] = c.r * c.a;
        P[1][i] = c.g * c.a;
        P[2][i] = c.b * c.a;
        P[3][i] = c.a * 255;
    }
    auto pass = [&](std::vector<int64_t>& p, int w, bool vertical) {
        const int r = (w - 1) / 2;
        std::vector<int64_t> out(p.size());
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                int64_t S = 0;
                for (int j = -r; j <= r; ++j) {
                    int xx = vertical ? x : x + j, yy = vertical ? y + j : y;
                    const int n = vertical ? H : W;
                    int& k = vertical ? yy : xx;
                    if (k < 0 || k >= n) {
                        if (edge == Edge::Transparent) continue;
                        k = std::min(std::max(k, 0), n - 1);
                    }
                    S += p[(static_cast<size_t>(yy) * W) + xx];
                }
                out[(static_cast<size_t>(y) * W) + x] = (S + r) / w;
            }
        p = out;
    };
    for (auto& p : P) {
        for (int w : widths) pass(p, w, false);
        for (int w : widths) pass(p, w, true);
    }
    Image out(W, H);
    for (size_t i = 0; i < src.px.size(); ++i) {
        const int64_t pa = P[3][i];
        Rgba8 o;
        o.r = q(pa > 0 ? static_cast<double>(P[0][i]) / static_cast<double>(pa) : 0.0);
        o.g = q(pa > 0 ? static_cast<double>(P[1][i]) / static_cast<double>(pa) : 0.0);
        o.b = q(pa > 0 ? static_cast<double>(P[2][i]) / static_cast<double>(pa) : 0.0);
        o.a = q(static_cast<double>(pa) / 65025.0);
        out.px[i] = canonicalize(o);
    }
    return out;
}

bool same(const Image& a, const Image& b) { return a.w == b.w && a.h == b.h && a.px == b.px; }

}  // namespace

TEST(FilterGaussian, BoxWidthExamplesFromDoc) {
    using W = std::array<int, 3>;
    EXPECT_EQ(box_widths(0.5), (W{1, 1, 1}));
    EXPECT_EQ(box_widths(1.0), (W{1, 1, 3}));
    EXPECT_EQ(box_widths(1.5), (W{3, 3, 3}));
    EXPECT_EQ(box_widths(2.0), (W{3, 3, 5}));
    EXPECT_EQ(box_widths(3.0), (W{5, 5, 7}));
    EXPECT_EQ(box_widths(4.5), (W{9, 9, 9}));
    EXPECT_EQ(box_widths(8.0), (W{15, 15, 17}));
    EXPECT_EQ(box_widths(25.0), (W{49, 49, 51}));
    EXPECT_EQ(box_widths(250.0), (W{499, 499, 501}));
}

TEST(FilterGaussian, RunningSumEqualsDirectSum) {
    const Image img = noise_image(37, 29, 5);
    for (Edge e : {Edge::Clamp, Edge::Transparent})
        for (double r : {0.5, 1.0, 2.0, 3.7, 8.0, 30.0}) EXPECT_TRUE(same(gaussian_blur(img, r, e), gaussian_direct(img, r, e))) << r;
}

TEST(FilterGaussian, IdentityAtHalfPixelAndOpaqueStaysOpaqueWithClamp) {
    const Image img = noise_image(20, 12, 9);
    EXPECT_TRUE(same(gaussian_blur(img, 0.5, Edge::Clamp), img));
    Image solid(16, 16);
    for (auto& p : solid.px) p = Rgba8{10, 200, 30, 255};
    EXPECT_TRUE(same(gaussian_blur(solid, 250.0, Edge::Clamp), solid));
    const Image faded = gaussian_blur(solid, 4.0, Edge::Transparent);
    EXPECT_LT(faded.at(0, 0).a, 255);
    EXPECT_EQ(faded.at(0, 0).g, 200);  // premultiplied: the colour of transparency does not bleed in
}

TEST(FilterGaussian, Mutations12And29ChangeTheResult) {
    const Image img = noise_image(48, 40, 3);
    const Image good = gaussian_blur(img, 3.0, Edge::Clamp);
    {
        mut::ScopedMutations m({12});
        EXPECT_FALSE(same(gaussian_blur(img, 3.0, Edge::Clamp), good));
    }
    {
        mut::ScopedMutations m({29});
        EXPECT_FALSE(same(gaussian_blur(img, 3.0, Edge::Clamp), good));
    }
}

// O(1) in radius: a radius-250 blur costs about the same as a radius-2 blur.
TEST(FilterGaussian, LargeRadiusIsConstantTime) {
    const Image img = noise_image(512, 512, 1);
    auto time = [&](double r) {
        const auto t0 = std::chrono::steady_clock::now();
        const Image out = gaussian_blur(img, r, Edge::Clamp);
        const auto t1 = std::chrono::steady_clock::now();
        EXPECT_EQ(out.w, 512);
        return std::chrono::duration<double>(t1 - t0).count();
    };
    time(2.0);  // warm-up
    const double small = time(2.0);
    const double large = time(250.0);
    EXPECT_LT(large, (small * 4.0) + 0.05) << "small " << small << " s, large " << large << " s";
}

TEST(FilterMotion, VerticalSnapAndSymmetricHorizontal) {
    // angle 90: taps purely vertical; a single vertical stripe stays one column wide.
    Image img(9, 9);
    for (int y = 0; y < 9; ++y) img.at(4, y) = Rgba8{255, 0, 0, 255};
    const Image out = run_filter(MotionBlurParams{90.0, 4.0, Edge::Clamp}, img);
    for (int y = 0; y < 9; ++y) {
        EXPECT_EQ(out.at(3, y), Rgba8{});
        EXPECT_EQ(out.at(4, y), (Rgba8{255, 0, 0, 255}));
        EXPECT_EQ(out.at(5, y), Rgba8{});
    }
}

TEST(FilterUnsharpHighPass, FlatAreasAndThreshold) {
    Image flat(10, 10);
    for (auto& p : flat.px) p = Rgba8{90, 90, 90, 200};
    const Image hp = run_filter(HighPassParams{3.0, Edge::Clamp}, flat);
    for (const auto& p : hp.px) EXPECT_EQ(p, (Rgba8{128, 128, 128, 200}));
    const Image usm = run_filter(UnsharpMaskParams{150.0, 2.0, 0, Edge::Clamp}, flat);
    EXPECT_TRUE(same(usm, flat));
    const Image img = noise_image(24, 24, 11);
    const Image all_gated = run_filter(UnsharpMaskParams{300.0, 1.5, 255, Edge::Clamp}, img);
    for (size_t i = 0; i < img.px.size(); ++i) EXPECT_EQ(all_gated.px[i].a, img.px[i].a);
    mut::ScopedMutations m({28});
    const Image t8 = run_filter(UnsharpMaskParams{300.0, 1.5, 8, Edge::Clamp}, img);
    EXPECT_TRUE(same(t8, img)) << "mutation 28 gates every channel (|d|/255 < 8)";
}

TEST(FilterNoise, MonochromaticSharesOneValueAndAlphaIsKept) {
    Image grey(16, 16);
    for (auto& p : grey.px) p = Rgba8{100, 100, 100, 77};
    const Image out = run_filter(AddNoiseParams{60.0, NoiseDistribution::Gaussian, true, 99}, grey);
    int changed = 0;
    for (const auto& p : out.px) {
        EXPECT_EQ(p.r, p.g);
        EXPECT_EQ(p.g, p.b);
        EXPECT_EQ(p.a, 77);
        changed += p.r != 100;
    }
    EXPECT_GT(changed, 200);
    const Image colour = run_filter(AddNoiseParams{60.0, NoiseDistribution::Uniform, false, 99}, grey);
    int differ = 0;
    for (const auto& p : colour.px) differ += (p.r != p.g);
    EXPECT_GT(differ, 200);
    EXPECT_TRUE(same(run_filter(AddNoiseParams{0.0, NoiseDistribution::Uniform, false, 1}, grey), grey));
}

TEST(FilterOffset, ModesAndNegativeWrap) {
    const Image img = noise_image(7, 5, 2);
    const Image w = run_filter(OffsetParams{-70, 5, OffsetMode::Wrap}, img);
    for (int y = 0; y < 5; ++y)
        for (int x = 0; x < 7; ++x) EXPECT_EQ(w.at(x, y), img.at((((x + 70) % 7) + 7) % 7, y));
    const Image t = run_filter(OffsetParams{2, -1, OffsetMode::Transparent}, img);
    EXPECT_EQ(t.at(0, 0), Rgba8{});
    EXPECT_EQ(t.at(2, 0), img.at(0, 1));
    EXPECT_EQ(t.at(3, 4), Rgba8{});
    const Image r = run_filter(OffsetParams{100, 100, OffsetMode::Repeat}, img);
    for (const auto& p : r.px) EXPECT_EQ(p, img.at(0, 0));
}

TEST(FilterFramework, CoverageMasks) {
    Selection sel(10, 4);
    Coverage ramp;
    ramp.src = Coverage::Src::Ramp;
    auto m = coverage_mask(ramp, 10, 4, sel);
    EXPECT_EQ(m[0], 0);
    EXPECT_EQ(m[9], 255);
    EXPECT_EQ(m[4], (4 * 255) / 9);
    auto one = coverage_mask(ramp, 1, 4, sel);
    EXPECT_EQ(one[0], 255);
    Coverage rect;
    rect.src = Coverage::Src::Rect;
    rect.x = -3;
    rect.y = 2;
    rect.w = 5;
    rect.h = 100;
    rect.value = 128;
    m = coverage_mask(rect, 10, 4, sel);
    EXPECT_EQ(m[(2 * 10) + 1], 128);
    EXPECT_EQ(m[(2 * 10) + 2], 0);
    EXPECT_EQ(m[(1 * 10) + 1], 0);
    // No active selection: 255 everywhere; an active one is used as is.
    m = coverage_mask(Coverage::selection(), 10, 4, sel);
    for (uint8_t v : m) EXPECT_EQ(v, 255);
    sel.mask.set(3, 1, 77);
    m = coverage_mask(Coverage::selection(), 10, 4, sel);
    EXPECT_EQ(m[(1 * 10) + 3], 77);
    EXPECT_EQ(m[0], 0);
}

TEST(FilterFramework, LerpShortcutsLockAndPremultipliedMix) {
    Image O(3, 1), F(3, 1);
    O.px = {Rgba8{10, 20, 30, 255}, Rgba8{}, Rgba8{200, 0, 0, 100}};
    F.px = {Rgba8{250, 240, 230, 128}, Rgba8{255, 255, 255, 255}, Rgba8{0, 0, 200, 200}};
    const Image r0 = finish_filter(O, F, false, {0, 255, 128});
    EXPECT_EQ(r0.px[0], O.px[0]);
    EXPECT_EQ(r0.px[1], F.px[1]);
    // Pixel 2: m = 128/255 in premultiplied space.
    const double m = 128.0 / 255.0, aO = 100.0 / 255.0, aF = 200.0 / 255.0;
    const double a = (aO * (1.0 - m)) + (aF * m);
    const double pr = (((200.0 / 255.0) * aO) * (1.0 - m)) + (((0.0 / 255.0) * aF) * m);
    const double pb = (((0.0 / 255.0) * aO) * (1.0 - m)) + (((200.0 / 255.0) * aF) * m);
    EXPECT_EQ(r0.px[2], (Rgba8{q(pr / a), 0, q(pb / a), q(a)}));
    // Lock: F.A = O.A, alpha-0 originals stay transparent.
    const Image rl = finish_filter(O, F, true, {});
    EXPECT_EQ(rl.px[0], (Rgba8{250, 240, 230, 255}));
    EXPECT_EQ(rl.px[1], Rgba8{});
    EXPECT_EQ(rl.px[2], (Rgba8{0, 0, 200, 100}));
}

TEST(FilterApi, PreviewLeavesDocumentAndHistoryUntouched) {
    auto res = script::run_script_text(rltest::make_script(
        70, 70, R"({"op":"add_layer","id":"p","fill":"gradient","from":"#ff000080","to":"#0000ffff","dir":"h"})"));
    Document& doc = *res.doc;
    const size_t hist = doc.history_size();
    Node& layer = doc.require_raster("p", "t");
    const RgbaImage before = layer.pixels;
    const RgbaImage preview = filter_layer(layer, GaussianBlurParams{4.0, Edge::Clamp}, Coverage::all(), doc.selection());
    EXPECT_TRUE(layer.pixels.pixels_equal(before));
    EXPECT_FALSE(preview.pixels_equal(before));
    EXPECT_EQ(doc.history_size(), hist);
    // Commit path the GUI uses: one record, then apply; undo restores the original pixels.
    doc.push_history();
    apply_filter(doc, "p", GaussianBlurParams{4.0, Edge::Clamp}, Coverage::all(), "t");
    EXPECT_TRUE(doc.require_raster("p", "t").pixels.pixels_equal(preview));
    doc.undo(1);
    EXPECT_TRUE(doc.require_raster("p", "t").pixels.pixels_equal(before));
}

TEST(FilterApi, UnchangedTilesStayShared) {
    auto res = script::run_script_text(rltest::make_script(
        192, 64, R"({"op":"add_layer","id":"p","fill":"solid","color":"#20c060ff","rect":[0,0,64,64]})"));
    Document& doc = *res.doc;
    Node& layer = doc.require_raster("p", "t");
    const RgbaImage before = layer.pixels;
    Coverage rect;
    rect.src = Coverage::Src::Rect;
    rect.x = 130;
    rect.y = 0;
    rect.w = 10;
    rect.h = 10;
    const RgbaImage out = filter_layer(layer, OffsetParams{3, 0, OffsetMode::Wrap}, rect, doc.selection());
    EXPECT_TRUE(out.shares_tile(before, 0, 0));
}

TEST(FilterOps, ScriptErrors) {
    auto bad = [](const std::string& op) {
        return rltest::make_script(32, 32, R"({"op":"add_layer","id":"p","fill":"solid","color":"#ffffffff"},)" + op);
    };
    EXPECT_THROW(script::run_script_text(bad(R"({"op":"filter_gaussian_blur","layer":"p","radius":0.05})")), ScriptError);
    EXPECT_THROW(script::run_script_text(bad(R"({"op":"filter_gaussian_blur","layer":"p","edge":"wrap"})")), ScriptError);
    EXPECT_THROW(script::run_script_text(bad(R"({"op":"filter_offset","layer":"p","dx":1.0})")), ScriptError);
    EXPECT_THROW(script::run_script_text(bad(R"({"op":"filter_offset","layer":"p","coverage":{"src":"ramp"}})")), ScriptError);
    EXPECT_THROW(script::run_script_text(bad(R"({"op":"filter_offset","layer":"p","coverage":{"src":"rect","x":0,"y":0,"w":-1,"h":2}})")), ScriptError);
    EXPECT_THROW(script::run_script_text(bad(R"({"op":"filter_offset","layer":"p","coverage":{"src":"selection","value":3}})")), ScriptError);
    EXPECT_THROW(script::run_script_text(bad(R"({"op":"filter_add_noise","layer":"p","seed":9007199254740992})")), ScriptError);
    EXPECT_THROW(script::run_script_text(bad(R"({"op":"filter_high_pass","layer":"nope"})")), ScriptError);
    EXPECT_THROW(script::run_script_text(bad(R"({"op":"filter_high_pass","layer":"p","amount":1})")), ScriptError);
    EXPECT_NO_THROW(script::run_script_text(bad(R"({"op":"filter_add_noise","layer":"p","seed":9007199254740991})")));
}
