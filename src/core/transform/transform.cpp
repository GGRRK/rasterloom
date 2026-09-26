// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/transform/transform.hpp"

#include <cmath>

#include "core/base/mutation.hpp"
#include "core/base/quant.hpp"
#include "core/geometry/dense.hpp"

namespace rl::transform {

using geom::idx;

Mat3 mul(const Mat3& a, const Mat3& b) {
    Mat3 c{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            c[static_cast<size_t>((3 * i) + j)] =
                ((a[static_cast<size_t>(3 * i)] * b[static_cast<size_t>(j)]) +
                 (a[static_cast<size_t>((3 * i) + 1)] * b[static_cast<size_t>(3 + j)])) +
                (a[static_cast<size_t>((3 * i) + 2)] * b[static_cast<size_t>(6 + j)]);
    return c;
}

double deg_to_rad(double deg) { return deg * (3.141592653589793 / 180.0); }

Mat3 Tr(double tx, double ty) { return Mat3{1.0, 0.0, tx, 0.0, 1.0, ty, 0.0, 0.0, 1.0}; }
Mat3 Sc(double sx, double sy) { return Mat3{sx, 0.0, 0.0, 0.0, sy, 0.0, 0.0, 0.0, 1.0}; }

Mat3 Ro(double deg) {
    const double rad = deg_to_rad(deg);
    const double c = std::cos(rad), s = std::sin(rad);
    return Mat3{c, -s, 0.0, s, c, 0.0, 0.0, 0.0, 1.0};
}

Mat3 Sk(double kx, double ky) {
    const double hx = std::tan(deg_to_rad(kx)), hy = std::tan(deg_to_rad(ky));
    return Mat3{1.0, hx, 0.0, hy, 1.0, 0.0, 0.0, 0.0, 1.0};
}

Mat3 from_params(const Params& p) {
    Mat3 M = Tr(-p.px, -p.py);
    M = mul(Sc(p.sx, p.sy), M);
    M = mul(Sk(p.kx, p.ky), M);
    M = mul(Ro(p.rotate), M);
    M = mul(Tr(p.px + p.tx, p.py + p.ty), M);
    return M;
}

std::optional<Mat3> from_quad(const Quad& qd) {
    const double x0 = qd.q[0][0], y0 = qd.q[0][1];
    const double x1 = qd.q[1][0], y1 = qd.q[1][1];
    const double x2 = qd.q[2][0], y2 = qd.q[2][1];
    const double x3 = qd.q[3][0], y3 = qd.q[3][1];
    const double sx = ((x0 - x1) + x2) - x3;
    const double sy = ((y0 - y1) + y2) - y3;
    double a, b, c, d, e, f, g, h;
    if (sx == 0.0 && sy == 0.0) {
        g = 0.0;
        h = 0.0;
        a = x1 - x0;
        b = x3 - x0;
        c = x0;
        d = y1 - y0;
        e = y3 - y0;
        f = y0;
    } else {
        const double dx1 = x1 - x2, dx2 = x3 - x2, dy1 = y1 - y2, dy2 = y3 - y2;
        const double den = (dx1 * dy2) - (dx2 * dy1);
        if (den == 0.0) return std::nullopt;
        g = ((sx * dy2) - (dx2 * sy)) / den;
        h = ((dx1 * sy) - (sx * dy1)) / den;
        a = (x1 - x0) + (g * x1);
        b = (x3 - x0) + (h * x3);
        c = x0;
        d = (y1 - y0) + (g * y1);
        e = (y3 - y0) + (h * y3);
        f = y0;
    }
    const Mat3 Q{a, b, c, d, e, f, g, h, 1.0};
    const Mat3 N{1.0 / qd.rw, 0.0, (-qd.rx) / qd.rw, 0.0, 1.0 / qd.rh, (-qd.ry) / qd.rh, 0.0, 0.0, 1.0};
    return mul(Q, N);
}

std::optional<Mat3> inverse(const Mat3& in) {
    Mat3 m = in;
    if (mut::active(31)) {
        std::swap(m[1], m[3]);
        std::swap(m[2], m[6]);
        std::swap(m[5], m[7]);
    }
    const double i0 = (m[4] * m[8]) - (m[5] * m[7]);
    const double i1 = (m[2] * m[7]) - (m[1] * m[8]);
    const double i2 = (m[1] * m[5]) - (m[2] * m[4]);
    const double i3 = (m[5] * m[6]) - (m[3] * m[8]);
    const double i4 = (m[0] * m[8]) - (m[2] * m[6]);
    const double i5 = (m[2] * m[3]) - (m[0] * m[5]);
    const double i6 = (m[3] * m[7]) - (m[4] * m[6]);
    const double i7 = (m[1] * m[6]) - (m[0] * m[7]);
    const double i8 = (m[0] * m[4]) - (m[1] * m[3]);
    const double det = ((m[0] * i0) + (m[1] * i3)) + (m[2] * i6);
    if (!(std::abs(det) >= 1e-12)) return std::nullopt;
    return Mat3{i0 / det, i1 / det, i2 / det, i3 / det, i4 / det, i5 / det, i6 / det, i7 / det, i8 / det};
}

double keys(double d) {
    const double x = std::abs(d);
    if (x < 1.0) return ((((1.5 * x) - 2.5) * x) * x) + 1.0;
    if (x < 2.0) return (((((x - 5.0) * x) + 8.0) * x) - 4.0) * (-0.5);
    return 0.0;
}

namespace {

// dec(v) and dec(k) * dec(a) as exact lookup tables (identical doubles to computing them inline).
struct Luts {
    double dec[256];
    std::vector<double> pm;  // pm[a * 256 + k] = dec(k) * dec(a)
    Luts() : pm(256 * 256) {
        for (int v = 0; v < 256; ++v) dec[v] = rl::dec(v);
        for (int a = 0; a < 256; ++a)
            for (int k = 0; k < 256; ++k) pm[static_cast<size_t>((a * 256) + k)] = dec[k] * dec[a];
    }
};
const Luts& luts() {
    static const Luts l;
    return l;
}

// Maps destination pixel centre (x + 0.5, y + 0.5) through `n`. Returns false when the pixel is
// behind the horizon or its source position fails the ±4 px range test (§12.4).
inline bool map_px(const Mat3& n, int x, int y, int sw, int sh, double& sx, double& sy) {
    const double X = static_cast<double>(x) + 0.5;
    const double Y = static_cast<double>(y) + 0.5;
    const double u = ((n[0] * X) + (n[1] * Y)) + n[2];
    const double v = ((n[3] * X) + (n[4] * Y)) + n[5];
    const double w = ((n[6] * X) + (n[7] * Y)) + n[8];
    if (!(w > 0.0)) return false;
    sx = u / w;
    sy = v / w;
    const double W = static_cast<double>(sw), H = static_cast<double>(sh);
    return sx >= -4.0 && sx <= W + 4.0 && sy >= -4.0 && sy <= H + 4.0;
}

struct Taps {
    int64_t cx[4], cy[4];
    double wx[4], wy[4];
};

inline void bicubic_taps(double sx, double sy, Taps& t) {
    const bool m30 = mut::active(30);
    const double fx = m30 ? sx : (sx - 0.5);
    const double fy = m30 ? sy : (sy - 0.5);
    const int64_t ix = static_cast<int64_t>(std::floor(fx));
    const int64_t iy = static_cast<int64_t>(std::floor(fy));
    for (int k = 0; k < 4; ++k) {
        t.cx[k] = (ix - 1) + k;
        t.wx[k] = keys(fx - static_cast<double>(t.cx[k]));
        t.cy[k] = (iy - 1) + k;
        t.wy[k] = keys(fy - static_cast<double>(t.cy[k]));
    }
}

}  // namespace

namespace {

// Source adapters for the warp kernels: a dense row-major buffer or a tiled image (TileGrid).
template <class Px>
struct DenseSrc {
    const std::vector<Px>& v;
    int w;
    const Px& at(int x, int y) const { return v[idx(x, y, w)]; }
};

// §12.4 RGBA kernel over destination rows [y0, y1); `out` holds (y1 - y0) * dw pixels and receives
// (0, 0, 0, 0) wherever the source position fails the tests.
template <class Src>
void warp_rgba_rows(const Src& src, int sw, int sh, int dw, const Mat3& inv, Interp interp, int y0, int y1,
                    Rgba8* out) {
    const Luts& L = luts();
    const bool m11 = mut::active(11);
    std::fill(out, out + (static_cast<size_t>(y1 - y0) * static_cast<size_t>(dw)), Rgba8{});
    Taps t{};
    for (int y = y0; y < y1; ++y) {
        for (int x = 0; x < dw; ++x) {
            double sx = 0.0, sy = 0.0;
            if (!map_px(inv, x, y, sw, sh, sx, sy)) continue;  // (0, 0, 0, 0)
            Rgba8& o = out[idx(x, y - y0, dw)];
            if (interp == Interp::Nearest) {
                const int64_t ix = static_cast<int64_t>(std::floor(sx));
                const int64_t iy = static_cast<int64_t>(std::floor(sy));
                if (ix >= 0 && iy >= 0 && ix < sw && iy < sh) o = src.at(static_cast<int>(ix), static_cast<int>(iy));
                continue;
            }
            bicubic_taps(sx, sy, t);
            double acc[4] = {0.0, 0.0, 0.0, 0.0};  // r, g, b, a
            for (int ky = 0; ky < 4; ++ky) {
                double row[4] = {0.0, 0.0, 0.0, 0.0};
                const int64_t cy = t.cy[ky];
                const bool yin = cy >= 0 && cy < sh;
                for (int kx = 0; kx < 4; ++kx) {
                    const int64_t cx = t.cx[kx];
                    double p[4] = {0.0, 0.0, 0.0, 0.0};
                    if (yin && cx >= 0 && cx < sw) {
                        const Rgba8 s = src.at(static_cast<int>(cx), static_cast<int>(cy));
                        if (m11) {
                            p[0] = L.dec[s.r];
                            p[1] = L.dec[s.g];
                            p[2] = L.dec[s.b];
                        } else {
                            const size_t base = static_cast<size_t>(s.a) * 256;
                            p[0] = L.pm[base + s.r];
                            p[1] = L.pm[base + s.g];
                            p[2] = L.pm[base + s.b];
                        }
                        p[3] = L.dec[s.a];
                    }
                    const double w = t.wx[kx];
                    for (int c = 0; c < 4; ++c) row[c] = row[c] + (w * p[c]);
                }
                const double w = t.wy[ky];
                for (int c = 0; c < 4; ++c) acc[c] = acc[c] + (w * row[c]);
            }
            const double A = clamp(acc[3], 0.0, 1.0);
            if (m11) {
                Rgba8 r{q(clamp(acc[0], 0.0, 1.0)), q(clamp(acc[1], 0.0, 1.0)), q(clamp(acc[2], 0.0, 1.0)), q(A)};
                o = canonicalize(r);
                continue;
            }
            if (A == 0.0) continue;
            Rgba8 r{q(clamp(acc[0], 0.0, A) / A), q(clamp(acc[1], 0.0, A) / A), q(clamp(acc[2], 0.0, A) / A), q(A)};
            o = canonicalize(r);
        }
    }
}

// §11.4 grey kernel over destination rows [y0, y1); failing pixels are 255 (doc 50 §4).
template <class Src>
void warp_gray_rows(const Src& src, int sw, int sh, int dw, const Mat3& inv, int y0, int y1, uint8_t* out) {
    const Luts& L = luts();
    std::fill(out, out + (static_cast<size_t>(y1 - y0) * static_cast<size_t>(dw)), uint8_t{255});
    Taps t{};
    for (int y = y0; y < y1; ++y) {
        for (int x = 0; x < dw; ++x) {
            double sx = 0.0, sy = 0.0;
            if (!map_px(inv, x, y, sw, sh, sx, sy)) continue;  // 255 (doc 50 §4)
            bicubic_taps(sx, sy, t);
            double acc = 0.0;
            for (int ky = 0; ky < 4; ++ky) {
                double row = 0.0;
                const int64_t cy = t.cy[ky];
                const bool yin = cy >= 0 && cy < sh;
                for (int kx = 0; kx < 4; ++kx) {
                    const int64_t cx = t.cx[kx];
                    const double p =
                        (yin && cx >= 0 && cx < sw) ? L.dec[src.at(static_cast<int>(cx), static_cast<int>(cy))] : 1.0;
                    row = row + (t.wx[kx] * p);
                }
                acc = acc + (t.wy[ky] * row);
            }
            out[idx(x, y - y0, dw)] = q(clamp(acc, 0.0, 1.0));
        }
    }
}

}  // namespace

std::vector<Rgba8> warp_rgba(const std::vector<Rgba8>& src, int sw, int sh, int dw, int dh, const Mat3& inv,
                             Interp interp) {
    std::vector<Rgba8> out(static_cast<size_t>(dw) * static_cast<size_t>(dh));
    warp_rgba_rows(DenseSrc<Rgba8>{src, sw}, sw, sh, dw, inv, interp, 0, dh, out.data());
    return out;
}

std::vector<uint8_t> warp_gray(const std::vector<uint8_t>& src, int sw, int sh, int dw, int dh, const Mat3& inv) {
    std::vector<uint8_t> out(static_cast<size_t>(dw) * static_cast<size_t>(dh));
    warp_gray_rows(DenseSrc<uint8_t>{src, sw}, sw, sh, dw, inv, 0, dh, out.data());
    return out;
}

RgbaImage warp_rgba_tiled(const RgbaImage& src, int dw, int dh, const Mat3& inv, Interp interp) {
    const geom::TileGrid<Rgba8> g(src);
    return geom::from_bands<Rgba8>(dw, dh, Rgba8{}, [&](int y0, int y1, Rgba8* band) {
        warp_rgba_rows(g, src.width(), src.height(), dw, inv, interp, y0, y1, band);
    });
}

GrayImage warp_gray_tiled(const GrayImage& src, int dw, int dh, const Mat3& inv, Gray8 out_bg) {
    const geom::TileGrid<Gray8> g(src);
    return geom::from_bands<Gray8>(dw, dh, out_bg, [&](int y0, int y1, Gray8* band) {
        warp_gray_rows(g, src.width(), src.height(), dw, inv, y0, y1, band);
    });
}

std::optional<RgbaImage> transform_image(const RgbaImage& src, const Mat3& m, Interp interp) {
    const auto inv = inverse(m);
    if (!inv) return std::nullopt;
    return warp_rgba_tiled(src, src.width(), src.height(), *inv, interp);
}

}  // namespace rl::transform
