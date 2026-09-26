// SPDX-License-Identifier: GPL-3.0-or-later
//
// B2-B6 filters and the B0 framework (lock step, coverage masks, premultiplied coverage lerp).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "core/base/error.hpp"
#include "core/base/mutation.hpp"
#include "core/base/quant.hpp"
#include "core/base/rng.hpp"
#include "core/filters/filters.hpp"
#include "core/tile/memory.hpp"

namespace rl::filters {

namespace {

constexpr double K_PI = 3.141592653589793;

[[noreturn]] void fail(const std::string& ctx, const std::string& msg) { throw ScriptError(ctx + ": " + msg); }

std::string num(double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.17g", v);
    return buf;
}

void check_double(const std::string& ctx, const char* what, double v, double lo, double hi) {
    if (!(v >= lo && v <= hi))
        fail(ctx, std::string(what) + " = " + num(v) + " out of range [" + num(lo) + ", " + num(hi) + "]");
}

void check_int(const std::string& ctx, const char* what, int64_t v, int64_t lo, int64_t hi) {
    if (v < lo || v > hi)
        fail(ctx, std::string(what) + " = " + std::to_string(v) + " out of range [" + std::to_string(lo) + ", " +
                      std::to_string(hi) + "]");
}

// ---- B2 Motion Blur ---------------------------------------------------------------------------------
Image motion_blur(const Image& src, const MotionBlurParams& p) {
    const int W = src.w, H = src.h;
    const double K_DEG = K_PI / 180.0;
    const double th = p.angle * K_DEG;
    double ox = p.distance * std::cos(th);
    double oy = -(p.distance * std::sin(th));
    if (std::abs(ox) < 1e-9) ox = 0.0;
    if (std::abs(oy) < 1e-9) oy = 0.0;
    const int N = static_cast<int>(std::ceil(p.distance)) + 1;
    std::vector<int> IX(static_cast<size_t>(N)), IY(static_cast<size_t>(N));
    std::vector<double> FX(static_cast<size_t>(N)), FY(static_cast<size_t>(N));
    for (int s = 0; s < N; ++s) {
        const double t = (static_cast<double>(s) / static_cast<double>(N - 1)) - 0.5;
        const double tx = t * ox;
        const double ty = t * oy;
        const size_t k = static_cast<size_t>(s);
        IX[k] = static_cast<int>(std::floor(tx));
        FX[k] = tx - std::floor(tx);
        IY[k] = static_cast<int>(std::floor(ty));
        FY[k] = ty - std::floor(ty);
    }

    // Premultiplied samples, 4 doubles per pixel: pc = (C8 * A8) / 65025.0, pa = A8 / 255.0 - but
    // only for the source rows the taps of the current output row can reach (a ring of
    // max(IY) - min(IY) + 2 rows), not for the whole canvas: 32 bytes x W x (distance + 2) instead
    // of 32 bytes per canvas pixel. Every row is computed from the same formula: identical doubles.
    using P4 = std::array<double, 4>;
    const int iy_min = *std::min_element(IY.begin(), IY.end());
    const int iy_max = *std::max_element(IY.begin(), IY.end()) + 1;  // bilinear reads y0 + 1
    const int K = (iy_max - iy_min) + 1;
    std::vector<std::vector<P4>> ring(static_cast<size_t>(K), std::vector<P4>(static_cast<size_t>(W)));
    std::vector<int> ring_row(static_cast<size_t>(K), -1);
    auto load_row = [&](int yy) -> const std::vector<P4>& {
        const size_t slot = static_cast<size_t>(yy % K);
        if (ring_row[slot] != yy) {
            std::vector<P4>& r = ring[slot];
            const Rgba8* sp = &src.px[static_cast<size_t>(yy) * static_cast<size_t>(W)];
            for (int x = 0; x < W; ++x) {
                const Rgba8 c = sp[x];
                r[static_cast<size_t>(x)] = {static_cast<double>(c.r * c.a) / 65025.0, static_cast<double>(c.g * c.a) / 65025.0,
                                             static_cast<double>(c.b * c.a) / 65025.0, static_cast<double>(c.a) / 255.0};
            }
            ring_row[slot] = yy;
        }
        return ring[slot];
    };
    // Per output row, the source row of every tap (row y0 and y0 + 1 of sample k), resolved once:
    // nullptr = outside the canvas with transparent edges (every tap on it reads zero).
    static const P4 kZero{0.0, 0.0, 0.0, 0.0};
    const bool transparent = p.edge == Edge::Transparent;
    std::vector<const P4*> r0(static_cast<size_t>(N)), r1(static_cast<size_t>(N));
    auto row_of = [&](int yy) -> const P4* {
        if (yy < 0 || yy >= H) {
            if (transparent) return nullptr;
            yy = std::min(std::max(yy, 0), H - 1);
        }
        return load_row(yy).data();
    };
    auto prepare_rows = [&](int y) {
        for (size_t k = 0; k < static_cast<size_t>(N); ++k) {
            r0[k] = row_of(y + IY[k]);
            r1[k] = row_of(y + IY[k] + 1);
        }
    };
    // E(x, y) of the whole-plane form, with y already resolved to its row.
    auto E = [&](int x, const P4* row) -> const P4& {
        if (row == nullptr) return kZero;
        if (x < 0 || x >= W) {
            if (transparent) return kZero;
            x = std::min(std::max(x, 0), W - 1);
        }
        return row[static_cast<size_t>(x)];
    };

    const double Nd = static_cast<double>(N);
    Image out(W, H);
    for (int y = 0; y < H; ++y) {
        prepare_rows(y);
        for (int x = 0; x < W; ++x) {
            double acc[4] = {0.0, 0.0, 0.0, 0.0};
            for (int s = 0; s < N; ++s) {
                const size_t k = static_cast<size_t>(s);
                const int x0 = x + IX[k];
                const P4& p0 = E(x0, r0[k]);
                const P4& p1 = E(x0 + 1, r0[k]);
                const P4& p2 = E(x0, r1[k]);
                const P4& p3 = E(x0 + 1, r1[k]);
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
    }
    return out;
}

// ---- B3 Unsharp Mask --------------------------------------------------------------------------------
Image unsharp_mask(const Image& src, const UnsharpMaskParams& p) {
    const Image bl = gaussian_blur(src, p.radius, p.edge);
    const double k = p.amount / 100.0;
    const bool m28 = mut::active(28);
    Image out(src.w, src.h);
    for (size_t i = 0; i < src.px.size(); ++i) {
        const Rgba8 o = src.px[i];
        const Rgba8 b = bl.px[i];
        const uint8_t oc[3] = {o.r, o.g, o.b};
        const uint8_t bc[3] = {b.r, b.g, b.b};
        Rgba8 f;
        uint8_t* fc[3] = {&f.r, &f.g, &f.b};
        for (int c = 0; c < 3; ++c) {
            const int d = static_cast<int>(oc[c]) - static_cast<int>(bc[c]);
            // Mutation 28 (doc 20 Part D): normalised difference compared against a level count.
            const bool below = m28 ? ((static_cast<double>(std::abs(d)) / 255.0) < static_cast<double>(p.threshold))
                                   : (std::abs(d) < p.threshold);
            if (below)
                *fc[c] = oc[c];
            else
                *fc[c] = q((static_cast<double>(oc[c]) + (k * static_cast<double>(d))) / 255.0);
        }
        f.a = o.a;
        out.px[i] = canonicalize(f);
    }
    return out;
}

// ---- B4 Add Noise -----------------------------------------------------------------------------------
Image add_noise(const Image& src, const AddNoiseParams& p) {
    const double A = (p.amount * 255.0) / 100.0;
    const double sigma = A / 1.7320508075688772;
    const bool gauss = p.distribution == NoiseDistribution::Gaussian;
    Image out(src.w, src.h);
    for (int y = 0; y < src.h; ++y) {
        for (int x = 0; x < src.w; ++x) {
            const Rgba8 o = src.at(x, y);
            const uint8_t oc[3] = {o.r, o.g, o.b};
            Rgba8 f;
            uint8_t* fc[3] = {&f.r, &f.g, &f.b};
            const auto ux = static_cast<uint32_t>(x), uy = static_cast<uint32_t>(y);
            for (int c = 0; c < 3; ++c) {
                const uint64_t ci = p.monochromatic ? 0 : static_cast<uint64_t>(c);
                double n;
                if (!gauss) {
                    const double u = unit(pixel_hash(p.seed, ux, uy, ci));
                    n = A * ((2.0 * u) - 1.0);
                } else {
                    const double u1 = unit(pixel_hash(p.seed, ux, uy, 2 * ci));
                    const double u2 = unit(pixel_hash(p.seed, ux, uy, (2 * ci) + 1));
                    const double rr = std::sqrt(-2.0 * std::log(1.0 - u1));
                    const double z = rr * std::cos(6.283185307179586 * u2);
                    n = sigma * z;
                }
                *fc[c] = q((static_cast<double>(oc[c]) + n) / 255.0);
            }
            f.a = o.a;
            out.at(x, y) = canonicalize(f);
        }
    }
    return out;
}

// ---- B5 High Pass ----------------------------------------------------------------------------------
Image high_pass(const Image& src, const HighPassParams& p) {
    const Image bl = gaussian_blur(src, p.radius, p.edge);
    Image out(src.w, src.h);
    for (size_t i = 0; i < src.px.size(); ++i) {
        const Rgba8 o = src.px[i];
        const Rgba8 b = bl.px[i];
        Rgba8 f;
        f.r = q((static_cast<double>(static_cast<int>(o.r) - static_cast<int>(b.r)) / 255.0) + 0.5);
        f.g = q((static_cast<double>(static_cast<int>(o.g) - static_cast<int>(b.g)) / 255.0) + 0.5);
        f.b = q((static_cast<double>(static_cast<int>(o.b) - static_cast<int>(b.b)) / 255.0) + 0.5);
        f.a = o.a;
        out.px[i] = canonicalize(f);
    }
    return out;
}

// ---- B6 Offset -------------------------------------------------------------------------------------
Image offset(const Image& src, const OffsetParams& p) {
    const int64_t W = src.w, H = src.h;
    Image out(src.w, src.h);
    for (int64_t y = 0; y < H; ++y) {
        for (int64_t x = 0; x < W; ++x) {
            int64_t sx = x - p.dx, sy = y - p.dy;
            switch (p.mode) {
                case OffsetMode::Transparent:
                    if (sx < 0 || sx >= W || sy < 0 || sy >= H) continue;  // stays (0,0,0,0)
                    break;
                case OffsetMode::Repeat:
                    sx = std::min(std::max(sx, int64_t{0}), W - 1);
                    sy = std::min(std::max(sy, int64_t{0}), H - 1);
                    break;
                case OffsetMode::Wrap:
                    sx = ((sx % W) + W) % W;
                    sy = ((sy % H) + H) % H;
                    break;
            }
            out.at(static_cast<int>(x), static_cast<int>(y)) = src.at(static_cast<int>(sx), static_cast<int>(sy));
        }
    }
    return out;
}

struct Runner {
    const Image& src;
    Image operator()(const GaussianBlurParams& p) const { return gaussian_blur(src, p.radius, p.edge); }
    Image operator()(const MotionBlurParams& p) const { return motion_blur(src, p); }
    Image operator()(const UnsharpMaskParams& p) const { return unsharp_mask(src, p); }
    Image operator()(const AddNoiseParams& p) const { return add_noise(src, p); }
    Image operator()(const HighPassParams& p) const { return high_pass(src, p); }
    Image operator()(const OffsetParams& p) const { return offset(src, p); }
};

struct Validator {
    const std::string& ctx;
    void operator()(const GaussianBlurParams& p) const { check_double(ctx, "radius", p.radius, 0.1, 250.0); }
    void operator()(const MotionBlurParams& p) const {
        check_double(ctx, "angle", p.angle, -360.0, 360.0);
        check_double(ctx, "distance", p.distance, 1.0, 2000.0);
    }
    void operator()(const UnsharpMaskParams& p) const {
        check_double(ctx, "amount", p.amount, 1.0, 500.0);
        check_double(ctx, "radius", p.radius, 0.1, 250.0);
        check_int(ctx, "threshold", p.threshold, 0, 255);
    }
    void operator()(const AddNoiseParams& p) const {
        check_double(ctx, "amount", p.amount, 0.0, 400.0);
        if (p.seed > 9007199254740991ULL) fail(ctx, "seed out of range [0, 9007199254740991]");
    }
    void operator()(const HighPassParams& p) const { check_double(ctx, "radius", p.radius, 0.1, 250.0); }
    void operator()(const OffsetParams& p) const {
        check_int(ctx, "dx", p.dx, -65536, 65536);
        check_int(ctx, "dy", p.dy, -65536, 65536);
    }
};

}  // namespace

const char* op_name(const FilterParams& p) {
    static constexpr const char* kNames[] = {"filter_gaussian_blur", "filter_motion_blur", "filter_unsharp_mask",
                                             "filter_add_noise",     "filter_high_pass",   "filter_offset"};
    return kNames[p.index()];
}

void validate(const FilterParams& p, const std::string& context) { std::visit(Validator{context}, p); }

void validate(const Coverage& c, const std::string& context) {
    if (c.src != Coverage::Src::Rect) return;
    if (c.w < 0 || c.h < 0) fail(context, "coverage rect w and h must be >= 0");
    check_int(context, "coverage value", c.value, 0, 255);
}

Image run_filter(const FilterParams& p, const Image& src) { return std::visit(Runner{src}, p); }

std::vector<uint8_t> coverage_mask(const Coverage& c, int w, int h, const Selection& sel) {
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
    std::vector<uint8_t> m(n, 255);
    switch (c.src) {
        case Coverage::Src::All:
            break;
        case Coverage::Src::Selection:
            if (sel.active())
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x) m[(static_cast<size_t>(y) * w) + x] = sel.mask.get(x, y);
            break;
        case Coverage::Src::Rect:
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    const bool in = x >= c.x && x < c.x + c.w && y >= c.y && y < c.y + c.h;
                    m[(static_cast<size_t>(y) * w) + x] = in ? static_cast<uint8_t>(c.value) : uint8_t{0};
                }
            break;
        case Coverage::Src::Ramp:
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    int64_t v;
                    if (!c.vertical)
                        v = (w == 1) ? 255 : ((int64_t{x} * 255) / (w - 1));
                    else
                        v = (h == 1) ? 255 : ((int64_t{y} * 255) / (h - 1));
                    m[(static_cast<size_t>(y) * w) + x] = static_cast<uint8_t>(v);
                }
            break;
    }
    return m;
}

Image finish_filter(const Image& O, Image F, bool lock_alpha, const std::vector<uint8_t>& M) {
    const size_t n = O.px.size();
    if (lock_alpha) {
        for (size_t i = 0; i < n; ++i) {
            F.px[i].a = O.px[i].a;
            if (O.px[i].a == 0) F.px[i] = Rgba8{};
        }
    }
    if (M.empty()) {
        for (auto& p : F.px) canonicalize_inplace(p);
        return F;
    }
    for (size_t i = 0; i < n; ++i) {
        const uint8_t Mi = M[i];
        const Rgba8 o = O.px[i];
        const Rgba8 f = F.px[i];
        Rgba8 r;
        if (Mi == 0) {
            r = o;
        } else if (Mi == 255) {
            r = f;
        } else {
            const double m = static_cast<double>(Mi) / 255.0;
            const double aO = static_cast<double>(o.a) / 255.0;
            const double aF = static_cast<double>(f.a) / 255.0;
            const double a = (aO * (1.0 - m)) + (aF * m);
            const uint8_t oc[3] = {o.r, o.g, o.b};
            const uint8_t fc[3] = {f.r, f.g, f.b};
            uint8_t* rc[3] = {&r.r, &r.g, &r.b};
            for (int c = 0; c < 3; ++c) {
                const double pc = (((static_cast<double>(oc[c]) / 255.0) * aO) * (1.0 - m)) +
                                  (((static_cast<double>(fc[c]) / 255.0) * aF) * m);
                *rc[c] = q((a > 0.0) ? (pc / a) : 0.0);
            }
            r.a = q(a);
        }
        F.px[i] = canonicalize(r);
    }
    return F;
}

Image to_image(const RgbaImage& img) {
    Image out(img.width(), img.height());
    for (int ty = 0; ty < img.tiles_y(); ++ty)
        for (int tx = 0; tx < img.tiles_x(); ++tx) {
            const RgbaTile& t = img.tile(tx, ty);
            for (int ly = 0; ly < kTileSize; ++ly) {
                const int y = (ty * kTileSize) + ly;
                if (y >= out.h) break;
                for (int lx = 0; lx < kTileSize; ++lx) {
                    const int x = (tx * kTileSize) + lx;
                    if (x >= out.w) break;
                    out.at(x, y) = t.at(lx, ly);
                }
            }
        }
    return out;
}

void store_image(RgbaImage& dst, const Image& src) {
    for (int ty = 0; ty < dst.tiles_y(); ++ty)
        for (int tx = 0; tx < dst.tiles_x(); ++tx) {
            const RgbaTile& cur = dst.tile(tx, ty);
            RgbaTile t = cur;  // keeps the out-of-canvas part of an edge tile as it was
            bool changed = false;
            for (int ly = 0; ly < kTileSize; ++ly) {
                const int y = (ty * kTileSize) + ly;
                if (y >= src.h) break;
                for (int lx = 0; lx < kTileSize; ++lx) {
                    const int x = (tx * kTileSize) + lx;
                    if (x >= src.w) break;
                    const Rgba8 v = canonicalize(src.at(x, y));
                    if (v != t.at(lx, ly)) {
                        t.at(lx, ly) = v;
                        changed = true;
                    }
                }
            }
            if (changed) dst.put_tile_sparse(tx, ty, t);
        }
}

RgbaImage filter_layer(const Node& layer, const FilterParams& p, const Coverage& c, const Selection& sel) {
    const uint64_t n = static_cast<uint64_t>(layer.pixels.width()) * static_cast<uint64_t>(layer.pixels.height());
    mem::Reservation hold(n * 2 * sizeof(Rgba8), "filter input and output buffers");
    Image O = to_image(layer.pixels);
    Image F = run_filter(p, O);
    std::vector<uint8_t> M;
    if (c.src != Coverage::Src::All) M = coverage_mask(c, O.w, O.h, sel);
    const Image R = finish_filter(O, std::move(F), layer.lock_alpha, M);
    std::vector<Rgba8>().swap(O.px);  // released before the result's tiles are allocated
    std::vector<uint8_t>().swap(M);
    RgbaImage out = layer.pixels;  // shares tiles; only changed tiles are replaced
    store_image(out, R);
    return out;
}

void apply_filter(Document& doc, const std::string& layer_id, const FilterParams& p, const Coverage& c,
                  const std::string& context) {
    validate(p, context);
    validate(c, context);
    Node& layer = doc.require_raster(layer_id, context);
    RgbaImage out = filter_layer(layer, p, c, doc.selection());
    layer.pixels = std::move(out);
}

}  // namespace rl::filters
