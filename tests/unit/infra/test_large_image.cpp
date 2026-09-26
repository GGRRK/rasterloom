// SPDX-License-Identifier: GPL-3.0-or-later
//
// Byte identity of the memory-reduced (banded / tiled / streamed) filter and geometry paths with
// the whole-canvas algorithms they replaced. The oracles below are the previous implementations,
// copied verbatim from the core before the change (same formulas, same evaluation order), run on
// awkward sizes (not multiples of 64, extreme scale factors, one-axis resizes) that the golden
// corpus does not reach, with and without the mutations whose hooks live in these paths.
#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstring>
#include <vector>

#include "core/base/mutation.hpp"
#include "core/base/quant.hpp"
#include "core/base/rng.hpp"
#include "core/doc/document.hpp"
#include "core/filters/filters.hpp"
#include "core/geometry/canvas_ops.hpp"
#include "core/geometry/dense.hpp"
#include "core/transform/transform.hpp"

using namespace rl;

namespace {

uint64_t g_state = 0x9E3779B97F4A7C15ULL;
uint32_t rnd() {
    g_state ^= g_state << 13;
    g_state ^= g_state >> 7;
    g_state ^= g_state << 17;
    return static_cast<uint32_t>(g_state >> 11);
}

// Random pixels with runs of alpha 0 / 255 / partial, so premultiply edge cases occur.
filters::Image random_image(int w, int h) {
    filters::Image im(w, h);
    for (auto& p : im.px) {
        const uint32_t r = rnd();
        const uint8_t a = (r % 5 == 0) ? 0 : (r % 5 == 1) ? 255 : static_cast<uint8_t>(r >> 8);
        p = canonicalize(Rgba8{static_cast<uint8_t>(rnd()), static_cast<uint8_t>(rnd()), static_cast<uint8_t>(rnd()), a});
    }
    return im;
}

bool same(const std::vector<Rgba8>& a, const std::vector<Rgba8>& b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(Rgba8)) == 0;
}

// ---- oracle: whole-canvas gaussian (four int32 planes) --------------------------------------------
void old_box_line(int32_t* base, size_t stride, int n, int w, filters::Edge edge, std::vector<int32_t>& tmp) {
    if (w == 1) return;
    const int r = (w - 1) / 2;
    for (int i = 0; i < n; ++i) tmp[static_cast<size_t>(i)] = base[static_cast<size_t>(i) * stride];
    auto E = [&](int j) -> int64_t {
        if (j < 0 || j >= n) {
            if (edge == filters::Edge::Transparent) return 0;
            j = std::min(std::max(j, 0), n - 1);
        }
        return tmp[static_cast<size_t>(j)];
    };
    int64_t S = 0;
    for (int j = -r; j <= r; ++j) S += E(j);
    for (int i = 0; i < n; ++i) {
        base[static_cast<size_t>(i) * stride] = static_cast<int32_t>((S + r) / w);
        S += E(i + r + 1) - E(i - r);
    }
}

filters::Image old_gaussian(const filters::Image& src, double radius, filters::Edge edge) {
    const int W = src.w, H = src.h;
    const size_t N = src.px.size();
    const std::array<int, 3> widths = filters::box_widths(radius);
    const bool m29 = mut::active(29);
    std::array<std::vector<int32_t>, 4> P;
    for (auto& p : P) p.resize(N);
    for (size_t i = 0; i < N; ++i) {
        const Rgba8 c = src.px[i];
        const int32_t a = c.a, k = m29 ? 255 : a;
        P[0][i] = c.r * k;
        P[1][i] = c.g * k;
        P[2][i] = c.b * k;
        P[3][i] = a * 255;
    }
    std::vector<int32_t> tmp(static_cast<size_t>(std::max(W, H)));
    const bool v_first = mut::active(12);
    for (auto& p : P)
        for (int stage = 0; stage < 2; ++stage) {
            const bool vertical = (stage == 0) == v_first;
            for (int w : widths) {
                if (!vertical)
                    for (int y = 0; y < H; ++y) old_box_line(p.data() + (static_cast<size_t>(y) * W), 1, W, w, edge, tmp);
                else
                    for (int x = 0; x < W; ++x) old_box_line(p.data() + x, static_cast<size_t>(W), H, w, edge, tmp);
            }
        }
    filters::Image out(W, H);
    for (size_t i = 0; i < N; ++i) {
        const int32_t pa = P[3][i];
        const double a = static_cast<double>(pa) / 65025.0;
        Rgba8 o;
        uint8_t* ch[3] = {&o.r, &o.g, &o.b};
        for (int c = 0; c < 3; ++c) {
            double C;
            if (m29)
                C = static_cast<double>(P[static_cast<size_t>(c)][i]) / 65025.0;
            else
                C = (pa > 0) ? (static_cast<double>(P[static_cast<size_t>(c)][i]) / static_cast<double>(pa)) : 0.0;
            *ch[c] = q(C);
        }
        o.a = q(a);
        out.px[i] = canonicalize(o);
    }
    return out;
}

// ---- oracle: whole-canvas motion blur (4 doubles per pixel) ----------------------------------------
filters::Image old_motion(const filters::Image& src, const filters::MotionBlurParams& p) {
    const int W = src.w, H = src.h;
    const double K_PI = 3.141592653589793;
    const double th = p.angle * (K_PI / 180.0);
    double ox = p.distance * std::cos(th);
    double oy = -(p.distance * std::sin(th));
    if (std::abs(ox) < 1e-9) ox = 0.0;
    if (std::abs(oy) < 1e-9) oy = 0.0;
    const int N = static_cast<int>(std::ceil(p.distance)) + 1;
    std::vector<int> IX(static_cast<size_t>(N)), IY(static_cast<size_t>(N));
    std::vector<double> FX(static_cast<size_t>(N)), FY(static_cast<size_t>(N));
    for (int s = 0; s < N; ++s) {
        const double t = (static_cast<double>(s) / static_cast<double>(N - 1)) - 0.5;
        const double tx = t * ox, ty = t * oy;
        const size_t k = static_cast<size_t>(s);
        IX[k] = static_cast<int>(std::floor(tx));
        FX[k] = tx - std::floor(tx);
        IY[k] = static_cast<int>(std::floor(ty));
        FY[k] = ty - std::floor(ty);
    }
    std::vector<std::array<double, 4>> P(src.px.size());
    for (size_t i = 0; i < src.px.size(); ++i) {
        const Rgba8 c = src.px[i];
        P[i] = {static_cast<double>(c.r * c.a) / 65025.0, static_cast<double>(c.g * c.a) / 65025.0,
                static_cast<double>(c.b * c.a) / 65025.0, static_cast<double>(c.a) / 255.0};
    }
    static const std::array<double, 4> kZero{0.0, 0.0, 0.0, 0.0};
    const bool transparent = p.edge == filters::Edge::Transparent;
    auto E = [&](int x, int y) -> const std::array<double, 4>& {
        if (x < 0 || y < 0 || x >= W || y >= H) {
            if (transparent) return kZero;
            x = std::min(std::max(x, 0), W - 1);
            y = std::min(std::max(y, 0), H - 1);
        }
        return P[(static_cast<size_t>(y) * static_cast<size_t>(W)) + static_cast<size_t>(x)];
    };
    const double Nd = static_cast<double>(N);
    filters::Image out(W, H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            double acc[4] = {0.0, 0.0, 0.0, 0.0};
            for (int s = 0; s < N; ++s) {
                const size_t k = static_cast<size_t>(s);
                const int x0 = x + IX[k], y0 = y + IY[k];
                const auto& p0 = E(x0, y0);
                const auto& p1 = E(x0 + 1, y0);
                const auto& p2 = E(x0, y0 + 1);
                const auto& p3 = E(x0 + 1, y0 + 1);
                const double fx = FX[k], fy = FY[k];
                for (size_t c = 0; c < 4; ++c) {
                    const double m0 = (fy * (p2[c] - p0[c])) + p0[c];
                    const double m1 = (fy * (p3[c] - p1[c])) + p1[c];
                    acc[c] = acc[c] + ((fx * (m1 - m0)) + m0);
                }
            }
            const double a = acc[3] / Nd;
            Rgba8 o;
            uint8_t* ch[3] = {&o.r, &o.g, &o.b};
            for (int c = 0; c < 3; ++c) *ch[c] = q((a > 0.0) ? ((acc[c] / Nd) / a) : 0.0);
            o.a = q(a);
            out.at(x, y) = canonicalize(o);
        }
    return out;
}

// ---- oracle: whole-plane separable bicubic resample ---------------------------------------------------
struct AxisCoeffs {
    std::vector<int64_t> lo;
    std::vector<std::vector<double>> w;
};

AxisCoeffs axis_coeffs(int nin, int nout) {
    AxisCoeffs c;
    const double scale = static_cast<double>(nin) / static_cast<double>(nout);
    const double fs = std::max(scale, 1.0), supp = 2.0 * fs, inv_fs = 1.0 / fs;
    c.lo.resize(static_cast<size_t>(nout));
    c.w.resize(static_cast<size_t>(nout));
    for (int o = 0; o < nout; ++o) {
        const double center = (static_cast<double>(o) + 0.5) * scale;
        const int64_t lo = std::max<int64_t>(static_cast<int64_t>(std::floor((center - supp) + 0.5)), 0);
        const int64_t hi = std::min<int64_t>(static_cast<int64_t>(std::floor((center + supp) + 0.5)), nin);
        std::vector<double> k;
        double ww = 0.0;
        for (int64_t i = lo; i < hi; ++i) {
            const double ki = transform::keys(((static_cast<double>(i) - center) + 0.5) * inv_fs);
            k.push_back(ki);
            ww = ww + ki;
        }
        if (ww != 0.0)
            for (double& v : k) v = v / ww;
        c.lo[static_cast<size_t>(o)] = lo;
        c.w[static_cast<size_t>(o)] = std::move(k);
    }
    return c;
}

std::vector<double> resample_planes(std::vector<double> p, int ch, int W, int H, int w2, int h2) {
    using geom::idx;
    int cw = W;
    if (w2 != W) {
        const AxisCoeffs cx = axis_coeffs(W, w2);
        std::vector<double> o(static_cast<size_t>(w2) * static_cast<size_t>(H) * static_cast<size_t>(ch));
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < w2; ++x) {
                const auto& wt = cx.w[static_cast<size_t>(x)];
                const int64_t lo = cx.lo[static_cast<size_t>(x)];
                for (int c = 0; c < ch; ++c) {
                    double acc = 0.0;
                    for (size_t i = 0; i < wt.size(); ++i)
                        acc = acc + (wt[i] * p[(idx(static_cast<int>(lo + static_cast<int64_t>(i)), y, W) * ch) + c]);
                    o[(idx(x, y, w2) * ch) + c] = acc;
                }
            }
        p = std::move(o);
        cw = w2;
    }
    if (h2 != H) {
        const AxisCoeffs cy = axis_coeffs(H, h2);
        std::vector<double> o(static_cast<size_t>(cw) * static_cast<size_t>(h2) * static_cast<size_t>(ch));
        for (int y = 0; y < h2; ++y) {
            const auto& wt = cy.w[static_cast<size_t>(y)];
            const int64_t lo = cy.lo[static_cast<size_t>(y)];
            for (int x = 0; x < cw; ++x)
                for (int c = 0; c < ch; ++c) {
                    double acc = 0.0;
                    for (size_t i = 0; i < wt.size(); ++i)
                        acc = acc + (wt[i] * p[(idx(x, static_cast<int>(lo + static_cast<int64_t>(i)), cw) * ch) + c]);
                    o[(idx(x, y, cw) * ch) + c] = acc;
                }
        }
        p = std::move(o);
    }
    return p;
}

std::vector<Rgba8> old_image_size_rgba(const std::vector<Rgba8>& src, int W, int H, int w2, int h2) {
    const bool m11 = mut::active(11);
    std::vector<double> p(src.size() * 4);
    for (size_t i = 0; i < src.size(); ++i) {
        const Rgba8 s = src[i];
        const double pa = dec(s.a);
        p[(i * 4) + 0] = m11 ? dec(s.r) : dec(s.r) * pa;
        p[(i * 4) + 1] = m11 ? dec(s.g) : dec(s.g) * pa;
        p[(i * 4) + 2] = m11 ? dec(s.b) : dec(s.b) * pa;
        p[(i * 4) + 3] = pa;
    }
    p = resample_planes(std::move(p), 4, W, H, w2, h2);
    std::vector<Rgba8> out(static_cast<size_t>(w2) * static_cast<size_t>(h2));
    for (size_t i = 0; i < out.size(); ++i) {
        const double* a = &p[i * 4];
        const double A = clamp(a[3], 0.0, 1.0);
        if (m11) {
            out[i] = canonicalize(Rgba8{q(clamp(a[0], 0.0, 1.0)), q(clamp(a[1], 0.0, 1.0)), q(clamp(a[2], 0.0, 1.0)), q(A)});
            continue;
        }
        if (A == 0.0) continue;
        out[i] = canonicalize(Rgba8{q(clamp(a[0], 0.0, A) / A), q(clamp(a[1], 0.0, A) / A), q(clamp(a[2], 0.0, A) / A), q(A)});
    }
    return out;
}

std::vector<uint8_t> old_image_size_gray(const std::vector<uint8_t>& src, int W, int H, int w2, int h2) {
    std::vector<double> p(src.size());
    for (size_t i = 0; i < src.size(); ++i) p[i] = dec(src[i]);
    p = resample_planes(std::move(p), 1, W, H, w2, h2);
    std::vector<uint8_t> out(p.size());
    for (size_t i = 0; i < p.size(); ++i) out[i] = q(clamp(p[i], 0.0, 1.0));
    return out;
}

// A one-layer document (with a random layer mask) of the given pixels.
DocState doc_of(const filters::Image& im, std::vector<uint8_t>* mask_out) {
    DocState d;
    d.w = im.w;
    d.h = im.h;
    d.selection.reset(im.w, im.h);
    Node n = Node::make_raster("L", im.w, im.h);
    n.pixels = geom::from_dense<Rgba8>(im.px, im.w, im.h);
    std::vector<uint8_t> m(im.px.size());
    for (auto& v : m) v = static_cast<uint8_t>(rnd());
    n.mask = LayerMask{};
    n.mask->plane = geom::from_dense<Gray8>(m, im.w, im.h, 255);
    if (mask_out) *mask_out = m;
    d.root.children.push_back(std::move(n));
    return d;
}

}  // namespace

TEST(LargeImagePaths, GaussianMatchesWholeCanvasPlanes) {
    for (const std::vector<int>& muts : {std::vector<int>{}, {12}, {29}}) {
        mut::ScopedMutations m(muts);
        for (auto [w, h, r] : {std::array<double, 3>{131, 67, 3.5}, {70, 150, 12.0}, {9, 5, 40.0}}) {
            const filters::Image src = random_image(static_cast<int>(w), static_cast<int>(h));
            for (filters::Edge e : {filters::Edge::Clamp, filters::Edge::Transparent})
                EXPECT_TRUE(same(filters::gaussian_blur(src, r, e).px, old_gaussian(src, r, e).px))
                    << w << "x" << h << " r=" << r << " muts=" << muts.size();
        }
    }
}

TEST(LargeImagePaths, MotionBlurMatchesWholeCanvasPlane) {
    const filters::Image src = random_image(97, 61);
    for (double angle : {0.0, 30.0, 90.0, -135.0, 271.0})
        for (double dist : {1.0, 7.5, 40.0})
            for (filters::Edge e : {filters::Edge::Clamp, filters::Edge::Transparent}) {
                filters::MotionBlurParams p;
                p.angle = angle;
                p.distance = dist;
                p.edge = e;
                EXPECT_TRUE(same(filters::run_filter(p, src).px, old_motion(src, p).px))
                    << "angle " << angle << " distance " << dist;
            }
}

TEST(LargeImagePaths, StreamedImageSizeMatchesWholePlaneResample) {
    const int sizes[][4] = {{131, 67, 50, 90}, {131, 67, 131, 20}, {131, 67, 300, 67},
                            {200, 150, 7, 3},  {5, 3, 170, 130},   {64, 64, 128, 1}};
    for (const std::vector<int>& muts : {std::vector<int>{}, {11}}) {
        mut::ScopedMutations m(muts);
        for (const auto& s : sizes) {
            const filters::Image src = random_image(s[0], s[1]);
            std::vector<uint8_t> mask;
            DocState d = doc_of(src, &mask);
            geom::image_size(d, s[2], s[3], transform::Interp::Bicubic);
            const Node& n = d.root.children[0];
            EXPECT_TRUE(same(geom::to_dense(n.pixels), old_image_size_rgba(src.px, s[0], s[1], s[2], s[3])))
                << s[0] << "x" << s[1] << " -> " << s[2] << "x" << s[3];
            EXPECT_EQ(geom::to_dense(n.mask->plane), old_image_size_gray(mask, s[0], s[1], s[2], s[3]));
        }
    }
}

TEST(LargeImagePaths, TiledWarpMatchesDenseWarp) {
    const filters::Image src = random_image(150, 97);
    const RgbaImage tiled = geom::from_dense<Rgba8>(src.px, 150, 97);
    std::vector<uint8_t> g(src.px.size());
    for (auto& v : g) v = static_cast<uint8_t>(rnd());
    const GrayImage gt = geom::from_dense<Gray8>(g, 150, 97, 255);
    for (const std::vector<int>& muts : {std::vector<int>{}, {11}, {30}}) {
        mut::ScopedMutations m(muts);
        for (double deg : {15.0, 33.3, -100.0}) {
            const transform::Mat3 M = transform::mul(transform::Tr(20.0, -7.0), transform::Ro(deg));
            const auto inv = transform::inverse(M);
            ASSERT_TRUE(inv.has_value());
            for (transform::Interp ip : {transform::Interp::Bicubic, transform::Interp::Nearest}) {
                const RgbaImage t = transform::warp_rgba_tiled(tiled, 170, 130, *inv, ip);
                EXPECT_TRUE(same(geom::to_dense(t), transform::warp_rgba(src.px, 150, 97, 170, 130, *inv, ip))) << deg;
            }
            const GrayImage tg = transform::warp_gray_tiled(gt, 170, 130, *inv, 255);
            EXPECT_EQ(geom::to_dense(tg), transform::warp_gray(g, 150, 97, 170, 130, *inv)) << deg;
        }
    }
}
