// SPDX-License-Identifier: GPL-3.0-or-later
//
// Brush engine unit tests (docs/math/40-brush.md): the §8 worked examples, incremental
// StrokeSession equivalence (1 / 7 / N-sample chunks == the brush_stroke op), the §6
// brush_history_granularity selftest, mutation hooks 10, 39 and 42, sparseness, the dab limit, and a
// throughput measurement.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "../test_util.hpp"
#include "core/base/error.hpp"
#include "core/base/mutation.hpp"
#include "core/brush/brush.hpp"

using rl::brush::CloneState;
using rl::brush::Sample;
using rl::brush::SessionOptions;
using rl::brush::StrokeParams;
using rl::brush::StrokeSession;
using rl::brush::Tool;

namespace {

std::unique_ptr<rl::Document> setup_doc(int w, int h, const std::string& ops, const std::string& bg = "#00000000") {
    auto res = rl::script::run_script_text(rltest::make_script(w, h, ops, bg));
    return std::move(res.doc);
}

// A deterministic, awkward sample stream: curved path, sub-step and long segments, repeated points,
// a decreasing timestamp, pressure outside [0, 1], stationary airbrush holds.
std::vector<Sample> make_stream(int n, double cx, double cy, double rad) {
    std::vector<Sample> out;
    double t = 0.0;
    for (int i = 0; i < n; ++i) {
        const double a = static_cast<double>(i) * 0.11;
        Sample s;
        const bool hold = (i % 17) >= 14;  // three repeated samples every 17
        const int j = hold ? (i - ((i % 17) - 13)) : i;
        const double aj = static_cast<double>(j) * 0.11;
        s.x = cx + (rad * std::cos(aj)) + ((j % 5) * 0.37);
        s.y = cy + ((rad * 0.6) * std::sin(2.0 * aj));
        s.pressure = 0.55 + (0.6 * std::sin(a * 1.7));  // spans below 0 .. above 1
        s.tilt_x = 10.0;
        s.tilt_y = -5.0;
        t += (i % 23 == 22) ? -3.0 : 1.0 + static_cast<double>(i % 3);
        if (t < 0.0) t = 0.0;
        s.t_ms = t;
        out.push_back(s);
    }
    return out;
}

struct StrokeRun {
    rl::io::RgbaBuffer image;
    nlohmann::json op;
    size_t history = 0;
    size_t dabs = 0;
};

// Runs one stroke through StrokeSession in chunks of `chunk` samples (0 = all at once).
StrokeRun run_session(int w, int h, const std::string& setup, const StrokeParams& p, const std::vector<Sample>& samples,
                      size_t chunk, bool preview, const std::vector<std::array<int, 3>>& sel = {}) {
    auto doc = setup_doc(w, h, setup);
    for (const auto& s : sel) doc->selection().mask.set(s[0], s[1], static_cast<uint8_t>(s[2]));
    CloneState cs;
    StrokeSession ss;
    SessionOptions o;
    o.preview = preview;
    ss.begin(*doc, "L", p, o, &cs);
    const size_t n = samples.size();
    const size_t step = chunk == 0 ? n : chunk;
    for (size_t i = 0; i < n; i += step) {
        const size_t m = std::min(step, n - i);
        const auto delta = ss.add_samples(std::span<const Sample>(samples.data() + i, m));
        if (preview && !delta.rect.empty()) {
            EXPECT_GE(delta.rect.x0, 0);
            EXPECT_LE(delta.rect.x1, w);
            EXPECT_FALSE(delta.tiles.empty());
        }
    }
    // The preview equals what end() commits.
    rl::RgbaImage pv;
    rl::GrayImage pm;
    if (preview) {
        if (p.target == rl::brush::Target::Mask) {
            EXPECT_NE(ss.preview_mask(), nullptr);
            pm = *ss.preview_mask();
        } else {
            EXPECT_NE(ss.preview_pixels(), nullptr);
            pv = *ss.preview_pixels();
        }
    }
    StrokeRun r;
    r.dabs = ss.dab_count();
    r.op = ss.end();
    if (preview) {
        const rl::Node& L = *doc->find("L").node;
        if (p.target == rl::brush::Target::Mask)
            EXPECT_TRUE(L.mask->plane.pixels_equal(pm));
        else
            EXPECT_TRUE(L.pixels.pixels_equal(pv));
    }
    r.history = doc->history_size();
    r.image = rl::io::render_document(doc->state());
    return r;
}

rl::io::RgbaBuffer run_op_script(int w, int h, const std::string& setup, const nlohmann::json& op) {
    return rltest::render_text(rltest::make_script(w, h, setup + "," + op.dump()));
}

void expect_chunk_equivalence(int w, int h, const std::string& setup, const StrokeParams& p,
                              const std::vector<Sample>& samples, const char* label) {
    SCOPED_TRACE(label);
    const StrokeRun whole = run_session(w, h, setup, p, samples, 0, true);
    const StrokeRun one = run_session(w, h, setup, p, samples, 1, true);
    const StrokeRun seven = run_session(w, h, setup, p, samples, 7, true);
    const StrokeRun headless = run_session(w, h, setup, p, samples, 1, false);
    EXPECT_GT(whole.dabs, 20u);
    EXPECT_TRUE(rltest::same_pixels(whole.image, one.image));
    EXPECT_TRUE(rltest::same_pixels(whole.image, seven.image));
    EXPECT_TRUE(rltest::same_pixels(whole.image, headless.image));
    EXPECT_EQ(whole.dabs, one.dabs);
    EXPECT_EQ(whole.dabs, seven.dabs);
    // The recorded op replays through the script engine byte for byte.
    const auto replay = run_op_script(w, h, setup, whole.op);
    EXPECT_TRUE(rltest::same_pixels(whole.image, replay));
    // The session changed something (non-vacuous).
    const auto before = rltest::render_text(rltest::make_script(w, h, setup));
    EXPECT_FALSE(rltest::same_pixels(before, whole.image));
    // One history record per stroke: setup ops + 1.
    EXPECT_EQ(whole.history, one.history);
}

StrokeParams soft60() {
    StrokeParams p;
    p.color = rl::Rgba8{0xC0, 0x30, 0x20, 255};
    p.size = 60.0;
    p.hardness = 0.0;
    p.spacing = 0.1;
    p.opacity = 0.7;
    p.flow = 0.3;
    p.angle = 25.0;
    p.roundness = 0.6;
    p.smoothing = 0.5;
    p.dabs_per_second = 200.0;
    p.size_curve = rl::brush::Curve{{0.0, 0.2}, {0.4, 0.5}, {1.0, 1.0}};
    p.opacity_curve = rl::brush::Curve{{0.0, 0.0}, {1.0, 1.0}};
    return p;
}

const char* kGradient =
    R"({"op":"add_layer","id":"L","fill":"gradient","from":"#2040C0FF","to":"#F0C02080","dir":"h"})";

}  // namespace

// ---- doc 40 §8 worked examples -------------------------------------------------------------------

TEST(BrushWorked, DabWalk) {
    StrokeParams p;
    p.size = 8.0;
    p.spacing = 0.25;
    p.dabs_per_second = 20.0;
    p.smoothing = 0.5;
    p.size_curve = rl::brush::Curve{{0.0, 0.0}, {1.0, 1.0}};
    std::vector<Sample> s(3);
    s[0] = Sample{10, 10, 1.0, 0, 0, 0};
    s[1] = Sample{13, 14, 0.5, 0, 0, 20};
    s[2] = Sample{13, 14, 0.5, 0, 0, 120};
    double frac = -1;
    const auto dabs = rl::brush::detail::walk(p, s, &frac);
    ASSERT_EQ(dabs.size(), 6u);
    const double want[6][4] = {
        {10.0, 10.0, 1.0, 8.0},
        {10.90909090909091, 11.212121212121213, 0.696969696969697, 5.584313725490196},
        {11.535483870967742, 12.047311827956989, 0.5, 4.015686274509804},
        {11.766600331996932, 12.355467109329243, 0.5, 4.015686274509804},
        {11.997716793026123, 12.663622390701498, 0.5, 4.015686274509804},
        {12.228833254055314, 12.97177767207375, 0.5, 4.015686274509804},
    };
    for (size_t i = 0; i < 6; ++i) {
        EXPECT_EQ(dabs[i].x, want[i][0]) << i;
        EXPECT_EQ(dabs[i].y, want[i][1]) << i;
        EXPECT_EQ(dabs[i].p, want[i][2]) << i;
        EXPECT_EQ(dabs[i].d, want[i][3]) << i;
    }
    EXPECT_EQ(frac, 0.0915847614247312);
}

TEST(BrushWorked, StraightLineSplitIntoEvents) {
    StrokeParams p;
    p.size = 16.0;
    p.spacing = 0.25;
    double frac = -1;
    const std::vector<Sample> one = {Sample{32, 64, 1, 0, 0, 0}, Sample{224, 64, 1, 0, 0, 0}};
    auto d1 = rl::brush::detail::walk(p, one, &frac);
    EXPECT_EQ(d1.size(), 48u);
    EXPECT_EQ(frac, 0.9999999999999698);
    std::vector<Sample> many;
    for (int i = 0; i <= 80; ++i) many.push_back(Sample{32.0 + (2.4 * i), 64, 1, 0, 0, 0});
    auto d2 = rl::brush::detail::walk(p, many, &frac);
    EXPECT_EQ(d2.size(), 49u);
    EXPECT_EQ(frac, 0.0);
    for (size_t i = 0; i < 48; ++i) EXPECT_NEAR(d2[i].x, d1[i].x, 1.2e-13);
    {
        rl::mut::ScopedMutations m({8});
        EXPECT_EQ(rl::brush::detail::walk(p, many, nullptr).size(), 1u);
    }
    {
        rl::mut::ScopedMutations m({9});
        StrokeParams z = p;
        z.view_zoom = 4.0;
        EXPECT_EQ(rl::brush::detail::walk(z, one, nullptr).size(), 192u);
    }
    StrokeParams z = p;
    z.view_zoom = 4.0;
    EXPECT_EQ(rl::brush::detail::walk(z, one, nullptr).size(), 48u);  // zoom never enters the math
}

TEST(BrushWorked, DabMask) {
    using rl::brush::detail::dab_mask;
    EXPECT_EQ(dab_mask(10, 10, 8, 0.5, 1.0, 0.0, 12, 11), 0.563242072809033);
    EXPECT_EQ(dab_mask(10, 10, 8, 1.0, 1.0, 0.0, 12, 12), 0.44678887547554913);
    EXPECT_EQ(dab_mask(10, 10, 8, 0.0, 1.0, 0.0, 13, 10), 0.037317932004975574);
    EXPECT_EQ(dab_mask(10, 10, 8, 1.0, 0.5, 30.0, 12, 8), 0.5623208812614978);
}

// doc 40 §3.7 worked accumulation (revised 2026-09-26), opacity 0.6, flow 0.25, three dabs.
TEST(BrushWorked, Accumulation) {
    using rl::brush::Mode;
    using rl::brush::detail::accumulate;
    auto three = [](double m, Mode mode) {
        std::vector<double> v;
        double b = 0.0;
        for (int i = 0; i < 3; ++i) v.push_back(b = accumulate(b, m, 0.25, 0.6, mode));
        return v;
    };
    EXPECT_EQ(three(1.0, Mode::Wash), (std::vector<double>{0.15, 0.26249999999999996, 0.34687499999999993}));
    EXPECT_EQ(three(1.0, Mode::Buildup), (std::vector<double>{0.15, 0.27749999999999997, 0.38587499999999997}));
    EXPECT_EQ(three(0.5, Mode::Wash), (std::vector<double>{0.075, 0.140625, 0.198046875}));
    EXPECT_EQ(three(0.5, Mode::Buildup), (std::vector<double>{0.075, 0.144375, 0.208546875}));
    // Wash never passes its ceiling O; m = 0 leaves B bit-identical.
    double b = 0.0;
    for (int i = 0; i < 200; ++i) b = accumulate(b, 0.9, 1.0, 0.6, Mode::Wash);
    EXPECT_EQ(b, 0.6);
    EXPECT_EQ(accumulate(0.3, 0.0, 1.0, 0.6, Mode::Wash), 0.3);
    EXPECT_EQ(accumulate(0.3, 0.0, 1.0, 0.6, Mode::Buildup), 0.3);
    // Mutation 42 restores the pre-2026-09-26 mask-shaped ceiling: m = 1 agrees, soft edges do not.
    rl::mut::ScopedMutations m42({42});
    EXPECT_EQ(three(1.0, Mode::Wash), (std::vector<double>{0.15, 0.26249999999999996, 0.34687499999999993}));
    EXPECT_EQ(three(0.5, Mode::Wash), (std::vector<double>{0.075, 0.13124999999999998, 0.17343749999999997}));
    EXPECT_EQ(three(0.5, Mode::Buildup), (std::vector<double>{0.075, 0.13874999999999998, 0.19293749999999998}));
    EXPECT_EQ(accumulate(0.5, 0.5, 1.0, 1.0, Mode::Buildup), 0.5);  // capped at m
}

// The defect §3.7 was revised for: a soft flow-1 stroke's edge beads between dab centres when the
// stroke is the maximum of the dab falloffs. Along a straight stroke, an edge row must stay nearly
// flat.
TEST(BrushProfile, SoftEdgeRippleBounded) {
    StrokeParams p;
    p.size = 48.0;
    p.hardness = 0.0;
    p.opacity = 1.0;
    p.flow = 1.0;
    p.color = rl::Rgba8{0, 0, 0, 255};
    const std::vector<Sample> line = {Sample{16, 48, 1, 0, 0, 0}, Sample{176, 48, 1, 0, 0, 8}};
    auto ripple = [&](int y) {
        const StrokeRun r = run_session(192, 96, R"({"op":"add_layer","id":"L"})", p, line, 0, false);
        int lo = 255, hi = 0;
        for (int x = 64; x < 128; ++x) {  // two full dab periods (step 12) far from the stroke ends
            lo = std::min(lo, int{r.image.at(x, y).a});
            hi = std::max(hi, int{r.image.at(x, y).a});
        }
        return std::array<int, 3>{lo, hi, hi - lo};
    };
    for (int y : {62, 66, 69}) {  // 14, 18 and 21 px from the centre line (R = 24)
        SCOPED_TRACE(y);
        const auto now = ripple(y);
        std::array<int, 3> old{};
        {
            rl::mut::ScopedMutations m42({42});
            old = ripple(y);
        }
        std::printf("[soft-edge] y=%d alpha %d..%d (ripple %d); mutation 42: %d..%d (ripple %d)\n", y, now[0],
                    now[1], now[2], old[0], old[1], old[2]);
        EXPECT_GT(now[0], 0);
        // Discrete dabs (step 12 px) leave a few alpha levels of ripple in 1 - prod(1 - m_i); the old
        // maximum-of-falloffs model ripples at least twice as much on the same row (measured
        // 2026-09-26: 3/4/1 levels now vs 15/8/4 with mutation 42).
        EXPECT_LE(now[2], 4);
        EXPECT_GE(old[2], 2 * now[2]);
    }
}

TEST(BrushWorked, IdentityLut) {
    const auto lut = rl::brush::detail::bake_lut({{0.0, 0.0}, {1.0, 1.0}});
    for (int i = 0; i < 256; ++i) EXPECT_EQ(lut[static_cast<size_t>(i)], 257 * i);
}

// ---- incremental StrokeSession == brush_stroke op -------------------------------------------------

TEST(BrushSession, ChunkedEqualsOpWashSoft60) {
    expect_chunk_equivalence(192, 160, kGradient, soft60(), make_stream(160, 96, 80, 55), "wash");
}

TEST(BrushSession, ChunkedEqualsOpBuildupTransparent) {
    StrokeParams p = soft60();
    p.mode = rl::brush::Mode::Buildup;
    p.size_curve.reset();
    p.hardness = 0.8;
    p.angle = 0.0;
    p.roundness = 1.0;
    expect_chunk_equivalence(160, 130, R"({"op":"add_layer","id":"L"})", p, make_stream(120, 80, 64, 70), "buildup");
}

TEST(BrushSession, ChunkedEqualsOpEraser) {
    StrokeParams p = soft60();
    p.tool = Tool::Eraser;
    expect_chunk_equivalence(192, 160, kGradient, p, make_stream(140, 96, 80, 50), "eraser");
}

TEST(BrushSession, ChunkedEqualsOpClone) {
    StrokeParams p = soft60();
    p.tool = Tool::Clone;
    p.source = std::array<double, 2>{40.5, 30.25};
    p.size = 30.0;
    expect_chunk_equivalence(192, 160, kGradient, p, make_stream(140, 110, 90, 40), "clone");
}

TEST(BrushSession, ChunkedEqualsOpMask) {
    StrokeParams p = soft60();
    p.target = rl::brush::Target::Mask;
    p.color = rl::Rgba8{0x40, 0x40, 0x40, 255};
    const std::string setup =
        std::string(kGradient) + R"(,{"op":"add_mask","layer":"L","fill":"solid","value":255})";
    expect_chunk_equivalence(192, 160, setup, p, make_stream(120, 96, 80, 50), "mask");
}

TEST(BrushSession, ChunkedEqualsOpLocked) {
    StrokeParams p = soft60();
    const std::string setup =
        R"({"op":"add_layer","id":"L","fill":"solid","color":"#2060C0B0","rect":[40,30,90,80]},)"
        R"({"op":"lock_transparency","layer":"L"})";
    expect_chunk_equivalence(192, 160, setup, p, make_stream(120, 96, 80, 55), "locked");
}

TEST(BrushSession, ChunkedEqualWithSelection) {
    // No selection op is needed: the selection mask is set directly. Every chunking and the
    // headless path must agree, and the selection must clip (s = 0 pixels are byte copies).
    std::vector<std::array<int, 3>> sel;
    for (int y = 0; y < 160; ++y)
        for (int x = 0; x < 96; ++x) sel.push_back({x, y, (x * 255) / 95});
    const StrokeParams p = soft60();
    const auto samples = make_stream(160, 96, 80, 55);
    const auto a = run_session(192, 160, kGradient, p, samples, 0, true, sel);
    const auto b = run_session(192, 160, kGradient, p, samples, 1, true, sel);
    const auto c = run_session(192, 160, kGradient, p, samples, 7, false, sel);
    EXPECT_TRUE(rltest::same_pixels(a.image, b.image));
    EXPECT_TRUE(rltest::same_pixels(a.image, c.image));
    EXPECT_FALSE(rltest::same_pixels(a.image, run_session(192, 160, kGradient, p, samples, 0, false).image));
    const auto base = rltest::render_text(rltest::make_script(192, 160, kGradient));
    for (int y = 0; y < 160; ++y)
        for (int x = 96; x < 192; ++x) {
            const size_t i = (static_cast<size_t>(y) * 192) + static_cast<size_t>(x);
            ASSERT_TRUE(a.image.px[i] == base.px[i]) << x << "," << y;
        }
}

TEST(BrushSession, ScriptOpEqualsOneShotSession) {
    // The op the golden scripts use == the session fed all samples in one call.
    const StrokeParams p = soft60();
    const auto samples = make_stream(90, 96, 80, 40);
    const auto whole = run_session(192, 160, kGradient, p, samples, 0, false);
    const auto op = rl::brush::to_op("L", p, samples);
    EXPECT_TRUE(rltest::same_pixels(whole.image, run_op_script(192, 160, kGradient, op)));
}

// ---- history (doc 40 §6, mutation 10) -------------------------------------------------------------

TEST(BrushHistory, brush_history_granularity) {
    auto doc = setup_doc(64, 64, R"({"op":"add_layer","id":"L","fill":"solid","color":"#808080"})");
    const rl::RgbaImage before = doc->find("L").node->pixels;
    const size_t h0 = doc->history_size();
    StrokeParams p;
    p.size = 10.0;
    p.hardness = 0.5;
    std::vector<Sample> s;
    for (int i = 0; i < 100; ++i) s.push_back(Sample{2.0 + (0.6 * i), 32.0 + std::sin(i * 0.2) * 20.0, 1, 0, 0, 0});
    StrokeSession ss;
    ss.begin(*doc, "L", p);
    for (const auto& x : s) ss.add_samples(std::span<const Sample>(&x, 1));
    ss.end();
    EXPECT_GE(ss.dab_count(), 40u);
    EXPECT_EQ(doc->history_size(), h0 + 1);
    EXPECT_FALSE(doc->find("L").node->pixels.pixels_equal(before));
    doc->undo(1);
    EXPECT_TRUE(doc->find("L").node->pixels.pixels_equal(before));
}

TEST(BrushHistory, Mutation10PushesPerDab) {
    rl::mut::ScopedMutations m({10});
    auto doc = setup_doc(64, 64, R"({"op":"add_layer","id":"L"})");
    const size_t h0 = doc->history_size();
    StrokeParams p;
    p.size = 10.0;
    StrokeSession ss;
    ss.begin(*doc, "L", p);
    const std::vector<Sample> s = {Sample{5, 32, 1, 0, 0, 0}, Sample{60, 32, 1, 0, 0, 0}};
    ss.add_samples(s);
    ss.end();
    EXPECT_EQ(doc->history_size(), h0 + ss.dab_count());
}

// ---- lock rules (mutation 39) ---------------------------------------------------------------------

TEST(BrushLock, EraserIsNoOpOnLockedLayerAndMutation39Breaks) {
    const std::string setup =
        R"({"op":"add_layer","id":"L","fill":"solid","color":"#2060C0","rect":[16,16,32,32]},)"
        R"({"op":"lock_transparency","layer":"L"})";
    const std::string stroke =
        R"({"op":"eraser_stroke","layer":"L","size":20,"samples":[{"x":4,"y":32},{"x":60,"y":32}]})";
    const auto base = rltest::render_text(rltest::make_script(64, 64, setup));
    const auto erased = rltest::render_text(rltest::make_script(64, 64, setup + "," + stroke));
    EXPECT_TRUE(rltest::same_pixels(base, erased));
    rl::mut::ScopedMutations m({39});
    const auto holed = rltest::render_text(rltest::make_script(64, 64, setup + "," + stroke));
    EXPECT_FALSE(rltest::same_pixels(base, holed));
}

TEST(BrushLock, BrushKeepsAlphaAndSkipsTransparent) {
    const std::string setup =
        R"({"op":"add_layer","id":"L","fill":"solid","color":"#2060C080","rect":[16,16,32,32]},)"
        R"({"op":"lock_transparency","layer":"L"})";
    const std::string stroke =
        R"({"op":"brush_stroke","layer":"L","color":"#FFCC00","size":20,"samples":[{"x":4,"y":32},{"x":60,"y":32}]})";
    auto res = rl::script::run_script_text(rltest::make_script(64, 64, setup + "," + stroke));
    const rl::Node& L = *res.doc->find("L").node;
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            const bool inside = x >= 16 && x < 48 && y >= 16 && y < 48;
            EXPECT_EQ(L.pixels.get(x, y).a, inside ? 0x80 : 0) << x << "," << y;
        }
    EXPECT_NE(L.pixels.get(32, 32).r, 0x20);  // recoloured
}

// ---- sparseness, limits, errors ------------------------------------------------------------------

TEST(BrushSparse, BufferIsSparseOnMaxCanvas) {
    rl::Document doc(16384, 16384, rl::Rgba8{});
    doc.add_node_top("root", rl::Node::make_raster("L", 16384, 16384), "test");
    StrokeParams p;
    p.size = 60.0;
    p.hardness = 0.0;
    StrokeSession ss;
    ss.begin(doc, "L", p);
    const std::vector<Sample> s = {Sample{8000, 8000, 1, 0, 0, 0}, Sample{8600, 8100, 1, 0, 0, 40}};
    ss.add_samples(s);
    EXPECT_LE(ss.buffer_tiles(), 40u);  // a 600 px stroke of a 60 px brush: a strip of tiles
    ss.end();
    EXPECT_LE(doc.find("L").node->pixels.allocated_tiles(), 40u);
}

TEST(BrushLimits, MoreThanAMillionDabsIsAnError) {
    const std::string s =
        R"({"op":"add_layer","id":"L"},{"op":"brush_stroke","layer":"L","size":1,"spacing":0.01,)"
        R"("samples":[{"x":-1000000,"y":-5},{"x":1000000,"y":-5}]})";
    EXPECT_THROW(rltest::render_text(rltest::make_script(8, 8, s)), rl::ScriptError);
}

TEST(BrushErrors, CloneWithoutSourceAndPressureClamp) {
    EXPECT_THROW(rltest::render_text(rltest::make_script(
                     8, 8, R"({"op":"add_layer","id":"L"},{"op":"clone_stroke","layer":"L","samples":[{"x":1,"y":1}]})")),
                 rl::ScriptError);
    // pressure is clamped, never an error
    const auto a = rltest::render_text(rltest::make_script(
        16, 16,
        R"({"op":"add_layer","id":"L"},{"op":"brush_stroke","layer":"L","size":6,"opacity_curve":[[0,0],[1,1]],"samples":[{"x":8,"y":8,"pressure":7.5}]})"));
    const auto b = rltest::render_text(rltest::make_script(
        16, 16,
        R"({"op":"add_layer","id":"L"},{"op":"brush_stroke","layer":"L","size":6,"opacity_curve":[[0,0],[1,1]],"samples":[{"x":8,"y":8,"pressure":1.0}]})"));
    EXPECT_TRUE(rltest::same_pixels(a, b));
}

// ---- throughput (BUILD-SPEC latency target: 60 px soft dabs at 1000 samples/s on one core) --------

TEST(BrushThroughput, Soft60DabStream) {
    rl::Document doc(4096, 4096, rl::Rgba8{});
    doc.add_node_top("root", rl::Node::make_raster("L", 4096, 4096), "test");
    StrokeParams p;
    p.size = 60.0;
    p.hardness = 0.0;
    p.opacity = 0.8;
    p.flow = 0.3;
    p.spacing = 0.05;  // 3 px step: ~1 dab per sample at 3 px/ms
    StrokeSession ss;
    ss.begin(doc, "L", p);  // preview on: the GUI configuration
    const int n = 4000;     // 4 s of input at 1000 samples/s
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < n; ++i) {
        const double a = i * 0.0015;
        Sample s{2048.0 + (1500.0 * std::cos(a)), 2048.0 + (1500.0 * std::sin(a * 1.3)), 1.0, 0, 0,
                 static_cast<double>(i)};
        ss.add_samples(std::span<const Sample>(&s, 1));
    }
    const auto t1 = std::chrono::steady_clock::now();
    ss.end();
    const auto t2 = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(t1 - t0).count();
    const double dabs = static_cast<double>(ss.dab_count());
    std::printf("[brush-throughput] %d samples, %.0f dabs of 60 px soft in %.3f s: %.0f dabs/s, %.0f samples/s "
                "(preview on); end() %.4f s\n",
                n, dabs, secs, dabs / secs, n / secs, std::chrono::duration<double>(t2 - t1).count());
    EXPECT_GT(dabs, 3000.0);
    // Real time: 4000 samples arrive over 4.0 s; processing must be faster than arrival.
    EXPECT_LT(secs, 4.0);
}
