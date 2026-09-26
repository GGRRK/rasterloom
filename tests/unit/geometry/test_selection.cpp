// SPDX-License-Identifier: GPL-3.0-or-later
//
// Selection math (doc 30 §1.3-§8): the interval rasterisers are checked against a brute-force
// per-sample evaluation of the doc's inside tests (§1.3 allows shortcuts only when n is identical
// for every pixel), plus the combine identities, feather/expand/contract properties, region
// connectivity and the marching-ants helpers.
#include <gtest/gtest.h>

#include <cmath>
#include <functional>
#include <random>

#include "core/base/mutation.hpp"
#include "core/base/quant.hpp"
#include "core/geometry/dense.hpp"
#include "core/select/selection_ops.hpp"

using namespace rl;
using select::Pt;

namespace {

using Inside = std::function<bool(double, double)>;

std::vector<uint8_t> brute(int W, int H, bool aa, const Inside& in) {
    std::vector<uint8_t> out(static_cast<size_t>(W) * H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            uint8_t v = 0;
            if (aa) {
                int n = 0;
                for (int j = 0; j < 16; ++j)
                    for (int i = 0; i < 16; ++i)
                        if (in(x + ((2 * i + 1) / 32.0), y + ((2 * j + 1) / 32.0))) ++n;
                v = q(n / 256.0);
            } else {
                v = in(x + 0.5, y + 0.5) ? 255 : 0;
            }
            out[static_cast<size_t>(y) * W + x] = v;
        }
    return out;
}

Inside rect_in(double x, double y, double w, double h) {
    const double x1 = x + w, y1 = y + h;
    return [=](double px, double py) { return (x <= px) && (px < x1) && (y <= py) && (py < y1); };
}

Inside ellipse_in(double x, double y, double w, double h) {
    const double rx = w / 2.0, ry = h / 2.0, cx = x + rx, cy = y + ry;
    return [=](double px, double py) {
        const double dx = (px - cx) / rx, dy = (py - cy) / ry;
        return ((dx * dx) + (dy * dy)) <= 1.0;
    };
}

Inside poly_in(std::vector<Pt> P, bool winding) {
    return [P, winding](double px, double py) {
        const size_t n = P.size();
        bool c = false;
        int wind = 0;
        for (size_t k = 0; k < n; ++k) {
            const Pt a = P[k], b = P[(k + n - 1) % n];
            if ((a.y > py) != (b.y > py)) {
                const double xc = (((b.x - a.x) * (py - a.y)) / (b.y - a.y)) + a.x;
                if (px < xc) {
                    c = !c;
                    wind += (a.y > b.y) ? 1 : -1;
                }
            }
        }
        return winding ? wind != 0 : c;
    };
}

int first_diff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i] != b[i]) return static_cast<int>(i);
    return -1;
}

Selection sel_from(int W, int H, const std::vector<uint8_t>& m) {
    Selection s(W, H);
    select::set_mask_dense(s, m);
    return s;
}

}  // namespace

TEST(GeometrySelection, RectMatchesBruteForce) {
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> pos(-10.0, 50.0), ext(0.01, 40.0);
    for (int it = 0; it < 200; ++it) {
        const double x = pos(rng), y = pos(rng), w = ext(rng), h = ext(rng);
        for (bool aa : {false, true}) {
            auto a = select::rasterize_rect(37, 29, x, y, w, h, aa);
            auto b = brute(37, 29, aa, rect_in(x, y, w, h));
            ASSERT_EQ(first_diff(a, b), -1) << x << "," << y << "," << w << "," << h << " aa=" << aa;
        }
    }
    // Integer rectangles: AA and non-AA identical, exactly [x, x+w) x [y, y+h).
    auto a = select::rasterize_rect(20, 20, 3, 4, 5, 6, true);
    auto b = select::rasterize_rect(20, 20, 3, 4, 5, 6, false);
    EXPECT_EQ(a, b);
    EXPECT_EQ(a[4 * 20 + 3], 255);
    EXPECT_EQ(a[4 * 20 + 8], 0);
    EXPECT_EQ(a[10 * 20 + 3], 0);
}

TEST(GeometrySelection, EllipseMatchesBruteForce) {
    std::mt19937 rng(11);
    std::uniform_real_distribution<double> pos(-15.0, 45.0), ext(0.05, 50.0);
    for (int it = 0; it < 200; ++it) {
        const double x = pos(rng), y = pos(rng), w = ext(rng), h = ext(rng);
        for (bool aa : {false, true}) {
            auto a = select::rasterize_ellipse(33, 31, x, y, w, h, aa);
            auto b = brute(33, 31, aa, ellipse_in(x, y, w, h));
            ASSERT_EQ(first_diff(a, b), -1) << x << "," << y << "," << w << "," << h << " aa=" << aa;
        }
    }
    // Tiny and huge ellipses.
    for (auto [x, y, w, h] : {std::array<double, 4>{10.3, 10.7, 1e-6, 2.0}, {-1e6, -1e6, 2e6 + 20, 2e6 + 15},
                              {20.3, 20.7, 2.5, 3.0}, {5, 5, 33, 21}}) {
        for (bool aa : {false, true})
            ASSERT_EQ(first_diff(select::rasterize_ellipse(32, 32, x, y, w, h, aa), brute(32, 32, aa, ellipse_in(x, y, w, h))),
                      -1);
    }
}

TEST(GeometrySelection, PolygonMatchesBruteForceEvenOddAndWinding) {
    std::mt19937 rng(3);
    std::uniform_real_distribution<double> pos(-12.0, 44.0);
    std::uniform_int_distribution<int> cnt(3, 9);
    for (int it = 0; it < 150; ++it) {
        std::vector<Pt> P(static_cast<size_t>(cnt(rng)));
        for (Pt& p : P) p = Pt{pos(rng), pos(rng)};
        if (it % 5 == 0)  // vertices on sample rows / pixel centres
            for (Pt& p : P) p = Pt{std::floor(p.x) + 0.5, std::floor(p.y) + 0.5};
        for (bool winding : {false, true}) {
            mut::ScopedMutations m(winding ? std::vector<int>{33} : std::vector<int>{});
            for (bool aa : {false, true}) {
                auto a = select::rasterize_polygon(32, 30, P, aa);
                auto b = brute(32, 30, aa, poly_in(P, winding));
                ASSERT_EQ(first_diff(a, b), -1) << "it=" << it << " winding=" << winding << " aa=" << aa;
            }
        }
    }
    // Pentagram: even-odd leaves the centre empty, winding fills it.
    const std::vector<Pt> star{{32, 4}, {48, 58}, {5, 24}, {59, 24}, {16, 58}};
    EXPECT_EQ(select::rasterize_polygon(64, 64, star, false)[32 * 64 + 32], 0);
    mut::ScopedMutations m({33});
    EXPECT_EQ(select::rasterize_polygon(64, 64, star, false)[32 * 64 + 32], 255);
}

TEST(GeometrySelection, CombineIdentities) {
    const int W = 40, H = 30;
    const auto A = select::rasterize_ellipse(W, H, 5.3, 4.1, 22.7, 17.2, true);
    Selection s = sel_from(W, H, A);
    select::combine(s, A, select::Mode::Add);
    EXPECT_EQ(select::mask_dense(s), A);
    select::combine(s, A, select::Mode::Intersect);
    EXPECT_EQ(select::mask_dense(s), A);
    select::combine(s, A, select::Mode::Subtract);
    EXPECT_FALSE(s.active());
    {
        mut::ScopedMutations m({34});
        Selection t = sel_from(W, H, A);
        select::combine(t, A, select::Mode::Subtract);
        EXPECT_TRUE(t.active());  // the fuzzy min leaves a ring
    }
}

TEST(GeometrySelection, DeselectReselectInverse) {
    const int W = 16, H = 16;
    Selection s(W, H);
    select::reselect(s);  // nothing saved: no-op
    EXPECT_FALSE(s.active());
    select::combine(s, select::rasterize_rect(W, H, 2, 2, 4, 4, false), select::Mode::New);
    const auto first = select::mask_dense(s);
    select::deselect(s);
    EXPECT_FALSE(s.active());
    select::deselect(s);  // empty: Saved untouched
    select::reselect(s);
    EXPECT_EQ(select::mask_dense(s), first);
    select::select_inverse(s);
    const auto inv = select::mask_dense(s);
    for (size_t i = 0; i < inv.size(); ++i) EXPECT_EQ(inv[i], 255 - first[i]);
    Selection e(W, H);
    select::select_inverse(e);
    EXPECT_TRUE(e.mask.all_equal(255));
}

TEST(GeometrySelection, BoxWidthsCheckValues) {
    using V = std::vector<int64_t>;
    EXPECT_EQ(select::box_widths(2 / 2.0), (V{1, 1, 3}));
    EXPECT_EQ(select::box_widths(4 / 2.0), (V{3, 3, 5}));
    EXPECT_EQ(select::box_widths(8 / 2.0), (V{7, 7, 9}));
    EXPECT_EQ(select::box_widths(10 / 2.0), (V{9, 9, 11}));
    const V big = select::box_widths(250 / 2.0);
    for (int64_t w : big) EXPECT_LE(w, 251);
}

TEST(GeometrySelection, FeatherSelectAllIsIdentityAndMaxRadiusIsSafe) {
    Selection s(70, 50);
    select::select_all(s);
    select::feather(s, 250.0);
    EXPECT_TRUE(s.mask.all_equal(255));

    // A single selected pixel at the maximal radius: sums stay exact, result symmetric.
    Selection p(301, 3);
    std::vector<uint8_t> m(301 * 3, 0);
    m[1 * 301 + 150] = 255;
    select::set_mask_dense(p, m);
    select::feather(p, 250.0);
    const auto r = select::mask_dense(p);
    for (int x = 0; x < 301; ++x) EXPECT_EQ(r[1 * 301 + x], r[1 * 301 + (300 - x)]);

    // Against a direct evaluation of §7 on a small mask (independent box sums).
    const int W = 23, H = 17;
    auto S = select::rasterize_ellipse(W, H, 3.2, 2.9, 12.5, 9.1, true);
    Selection f = sel_from(W, H, S);
    select::feather(f, 5.5);
    const auto widths = select::box_widths(5.5 / 2.0);
    std::vector<int64_t> T(S.begin(), S.end());
    for (int64_t w : widths) {
        const int64_t h = (w - 1) / 2;
        std::vector<int64_t> U(T.size());
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                int64_t sum = 0;
                for (int64_t k = -h; k <= h; ++k) sum += T[y * W + std::clamp<int64_t>(x + k, 0, W - 1)];
                U[y * W + x] = sum;
            }
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                int64_t sum = 0;
                for (int64_t k = -h; k <= h; ++k) sum += U[std::clamp<int64_t>(y + k, 0, H - 1) * W + x];
                T[y * W + x] = sum;
            }
    }
    const int64_t P3 = widths[0] * widths[1] * widths[2], D = P3 * P3;
    const auto got = select::mask_dense(f);
    for (size_t i = 0; i < T.size(); ++i) ASSERT_EQ(got[i], (2 * T[i] + D) / (2 * D)) << i;
}

TEST(GeometrySelection, ExpandContractMatchDiscDefinition) {
    const int W = 31, H = 27;
    std::mt19937 rng(5);
    std::vector<uint8_t> S(static_cast<size_t>(W) * H);
    for (auto& v : S) v = (rng() % 7 == 0) ? static_cast<uint8_t>(rng() % 256) : 0;
    for (int N : {1, 3, 6}) {
        for (bool dil : {true, false}) {
            Selection s = sel_from(W, H, S);
            if (dil)
                select::expand(s, N);
            else
                select::contract(s, N);
            const auto got = select::mask_dense(s);
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) {
                    int best = dil ? 0 : 255;
                    for (int dy = -N; dy <= N; ++dy)
                        for (int dx = -N; dx <= N; ++dx) {
                            if (dx * dx + dy * dy > N * N) continue;
                            const int sx = x + dx, sy = y + dy;
                            const bool in = sx >= 0 && sy >= 0 && sx < W && sy < H;
                            const int v = in ? S[sy * W + sx] : (dil ? 0 : 255);
                            best = dil ? std::max(best, v) : std::min(best, v);
                        }
                    ASSERT_EQ(got[y * W + x], best) << "N=" << N << " dil=" << dil << " at " << x << "," << y;
                }
        }
    }
    Selection all(20, 20);
    select::select_all(all);
    select::contract(all, 5);
    EXPECT_TRUE(all.mask.all_equal(255));
}

TEST(GeometrySelection, RegionConnectivityAndFringe) {
    // Two black pixels touching at a corner on white.
    const int W = 4, H = 4;
    std::vector<Rgba8> px(16, Rgba8{255, 255, 255, 255});
    px[1 * 4 + 1] = Rgba8{0, 0, 0, 255};
    px[2 * 4 + 2] = Rgba8{0, 0, 0, 255};
    auto r8 = select::region(px, W, H, 1, 1, 0, true, false);
    EXPECT_EQ(r8[2 * 4 + 2], 255);
    {
        mut::ScopedMutations m({15});
        auto r4 = select::region(px, W, H, 1, 1, 0, true, false);
        EXPECT_EQ(r4[2 * 4 + 2], 0);
    }
    // Seed outside the canvas: all 0.
    auto out = select::region(px, W, H, -1, 0, 255, true, true);
    for (uint8_t v : out) EXPECT_EQ(v, 0);
    // Fringe: grey ring pixel at d = 40 with t = 32: a = 1.5 - 40/32 = 0.25 -> q(0.5) = 128.
    std::vector<Rgba8> g(9, Rgba8{40, 40, 40, 255});
    g[4] = Rgba8{0, 0, 0, 255};
    auto rf = select::region(g, 3, 3, 1, 1, 32, true, true);
    EXPECT_EQ(rf[4], 255);
    EXPECT_EQ(rf[0], q(0.5));
}

TEST(GeometrySelection, OutlineAndEdgeMask) {
    const int W = 10, H = 8;
    GrayImage m = geom::from_dense<Gray8>(select::rasterize_rect(W, H, 2, 1, 5, 4, false), W, H);
    auto loops = select::outline_polylines(m);
    ASSERT_EQ(loops.size(), 1u);
    ASSERT_EQ(loops[0].size(), 4u);
    EXPECT_EQ(loops[0][0].x, 2.0);
    EXPECT_EQ(loops[0][0].y, 1.0);
    EXPECT_EQ(loops[0][1].x, 7.0);  // clockwise on screen: top edge first, left to right
    EXPECT_EQ(loops[0][1].y, 1.0);
    EXPECT_EQ(loops[0][2].x, 7.0);
    EXPECT_EQ(loops[0][2].y, 5.0);

    // Diagonal pixels: two loops. A ring: outer + hole.
    std::vector<uint8_t> d(static_cast<size_t>(W) * H, 0);
    d[1 * W + 1] = 255;
    d[2 * W + 2] = 255;
    EXPECT_EQ(select::outline_polylines(geom::from_dense<Gray8>(d, W, H)).size(), 2u);
    std::vector<uint8_t> ring(static_cast<size_t>(W) * H, 0);
    for (int y = 1; y <= 5; ++y)
        for (int x = 1; x <= 5; ++x)
            if (!(x == 3 && y == 3)) ring[y * W + x] = 255;
    auto rl = select::outline_polylines(geom::from_dense<Gray8>(ring, W, H));
    ASSERT_EQ(rl.size(), 2u);
    EXPECT_EQ(rl[0].size(), 4u);
    EXPECT_EQ(rl[1].size(), 4u);

    GrayImage e = select::edge_mask(m);
    EXPECT_EQ(e.get(2, 1), 255);
    EXPECT_EQ(e.get(4, 2), 0);  // interior
    EXPECT_EQ(e.get(6, 4), 255);
    EXPECT_EQ(e.get(0, 0), 0);
}
