// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/geometry/canvas_ops.hpp"

#include <cmath>
#include <deque>
#include <functional>
#include <vector>

#include "core/base/mutation.hpp"
#include "core/base/quant.hpp"
#include "core/geometry/dense.hpp"

namespace rl::geom {

namespace {

using transform::Interp;
using transform::Mat3;

// Applies `rgba` to every raster layer's pixels and `gray` to every mask plane, then sets the new
// canvas size and clears the selection (§2). Each layer's dense source and result are charged to the
// hard limit first (`what` names the op in the MemoryError); the first layer is checked before any
// node changes, and the GUI rolls a later failure back.
template <class RgbaFn, class GrayFn>
void remap_document(DocState& d, int w2, int h2, const char* what, RgbaFn&& rgba, GrayFn&& gray) {
    const int W = d.w, H = d.h;
    const uint64_t rgba_bytes = dense_bytes(W, H, sizeof(Rgba8)) + dense_bytes(w2, h2, sizeof(Rgba8));
    const uint64_t gray_bytes = dense_bytes(W, H, sizeof(Gray8)) + dense_bytes(w2, h2, sizeof(Gray8));
    for_each_layer_and_mask(
        d.root,
        [&](Node& n) {
            const mem::Reservation hold = reserve_dense(rgba_bytes, what);
            const std::vector<Rgba8> src = to_dense(n.pixels);
            n.pixels = from_dense<Rgba8>(rgba(src, W, H), w2, h2);
        },
        [&](Node& n) {
            const mem::Reservation hold = reserve_dense(gray_bytes, what);
            const Gray8 bg = n.mask->plane.background();
            const std::vector<uint8_t> src = to_dense(n.mask->plane);
            n.mask->plane = from_dense<Gray8>(gray(src, W, H), w2, h2, bg);
        });
    d.w = w2;
    d.h = h2;
    d.selection.reset(w2, h2);
}

// The same for ops that map tiled images to tiled images directly (no dense canvas copies).
template <class RgbaFn, class GrayFn>
void remap_document_tiled(DocState& d, int w2, int h2, RgbaFn&& rgba, GrayFn&& gray) {
    for_each_layer_and_mask(
        d.root, [&](Node& n) { n.pixels = rgba(n.pixels); },
        [&](Node& n) { n.mask->plane = gray(n.mask->plane, n.mask->plane.background()); });
    d.w = w2;
    d.h = h2;
    d.selection.reset(w2, h2);
}

// Exact index mapping new(x, y) = old(fx(x, y), fy(x, y)) with `outside` for out-of-canvas sources.
template <class Px, class Map>
std::vector<Px> index_map(const std::vector<Px>& src, int W, int H, int w2, int h2, Px outside, Map&& map) {
    std::vector<Px> out(static_cast<size_t>(w2) * static_cast<size_t>(h2), outside);
    for (int y = 0; y < h2; ++y)
        for (int x = 0; x < w2; ++x) {
            int64_t sx = 0, sy = 0;
            map(x, y, sx, sy);
            if (sx >= 0 && sy >= 0 && sx < W && sy < H)
                out[idx(x, y, w2)] = src[idx(static_cast<int>(sx), static_cast<int>(sy), W)];
        }
    return out;
}

// ---- §13 bicubic: Pillow-style per-axis coefficients -----------------------------------------------
struct AxisCoeffs {
    std::vector<int64_t> lo;           // first tap per output index
    std::vector<std::vector<double>> w;  // normalised weights, ascending taps
};

AxisCoeffs axis_coeffs(int nin, int nout) {
    AxisCoeffs c;
    const double scale = static_cast<double>(nin) / static_cast<double>(nout);
    const double fs = std::max(scale, 1.0);
    const double supp = 2.0 * fs;
    const double inv_fs = 1.0 / fs;
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

// Separable resample of `ch`-channel double rows (interleaved): horizontal pass if the width
// changes, then vertical if the height changes, no quantisation between passes - streamed. Output
// rows are produced in increasing y; each horizontally resampled input row is computed once and
// kept only while a later output row still needs it (a window of about 4 * max(1, H / h2) rows),
// so the working memory is O(window * width) instead of two whole-canvas double planes. Every
// sum runs over the same taps in the same order as the whole-plane form: identical doubles.
class RowResampler {
public:
    using SrcRow = std::function<void(int y, double* row)>;  // W * ch doubles

    RowResampler(int ch, int W, int H, int w2, int h2, SrcRow src)
        : ch_(ch), w2_(w2), hx_(w2 != W), vy_(h2 != H), src_(std::move(src)) {
        if (hx_) cx_ = axis_coeffs(W, w2);
        if (vy_) cy_ = axis_coeffs(H, h2);
        if (hx_) tmp_.resize(static_cast<size_t>(W) * static_cast<size_t>(ch));
    }

    // Output row y (w2 * ch doubles); y must not decrease between calls.
    void row(int y, double* out) {
        const size_t n = static_cast<size_t>(w2_) * static_cast<size_t>(ch_);
        if (!vy_) {
            drop_below(y);
            const std::vector<double>& r = hrow(y);
            std::copy(r.begin(), r.begin() + static_cast<std::ptrdiff_t>(n), out);
            return;
        }
        const auto& wt = cy_.w[static_cast<size_t>(y)];
        const int lo = static_cast<int>(cy_.lo[static_cast<size_t>(y)]);
        drop_below(lo);
        for (size_t i = 0; i < wt.size(); ++i) (void)hrow(lo + static_cast<int>(i));  // fill the window
        for (int x = 0; x < w2_; ++x)
            for (int c = 0; c < ch_; ++c) {
                const size_t k = (static_cast<size_t>(x) * static_cast<size_t>(ch_)) + static_cast<size_t>(c);
                double acc = 0.0;
                for (size_t i = 0; i < wt.size(); ++i)
                    acc = acc + (wt[i] * cache_[static_cast<size_t>(lo - first_) + i][k]);
                out[k] = acc;
            }
    }

private:
    void drop_below(int y) {
        while (!cache_.empty() && first_ < y) {
            cache_.pop_front();
            ++first_;
        }
        if (cache_.empty()) first_ = y;
    }

    // Horizontally resampled input row y (cached; rows are requested in non-decreasing windows).
    const std::vector<double>& hrow(int y) {
        if (cache_.empty()) first_ = y;  // callers never ask below the window (drop_below first)
        while (first_ + static_cast<int>(cache_.size()) <= y) {
            const int r = first_ + static_cast<int>(cache_.size());
            std::vector<double> o(static_cast<size_t>(w2_) * static_cast<size_t>(ch_));
            if (!hx_) {
                src_(r, o.data());
            } else {
                src_(r, tmp_.data());
                for (int x = 0; x < w2_; ++x) {
                    const auto& wt = cx_.w[static_cast<size_t>(x)];
                    const int64_t lo = cx_.lo[static_cast<size_t>(x)];
                    for (int c = 0; c < ch_; ++c) {
                        double acc = 0.0;
                        for (size_t i = 0; i < wt.size(); ++i)
                            acc = acc + (wt[i] * tmp_[(static_cast<size_t>(lo + static_cast<int64_t>(i)) * static_cast<size_t>(ch_)) +
                                                      static_cast<size_t>(c)]);
                        o[(static_cast<size_t>(x) * static_cast<size_t>(ch_)) + static_cast<size_t>(c)] = acc;
                    }
                }
            }
            cache_.push_back(std::move(o));
        }
        return cache_[static_cast<size_t>(y - first_)];
    }

    int ch_, w2_;
    bool hx_, vy_;
    SrcRow src_;
    AxisCoeffs cx_, cy_;
    std::deque<std::vector<double>> cache_;  // cache_[k] = horizontally resampled row first_ + k
    int first_ = 0;
    std::vector<double> tmp_;
};

RgbaImage image_size_rgba(const RgbaImage& img, int w2, int h2) {
    const int W = img.width(), H = img.height();
    const bool m11 = mut::active(11);
    const TileGrid<Rgba8> g(img);
    std::vector<Rgba8> srow(static_cast<size_t>(W));
    RowResampler rs(4, W, H, w2, h2, [&](int y, double* p) {
        g.row(y, srow.data());
        for (size_t i = 0; i < srow.size(); ++i) {
            const Rgba8 s = srow[i];
            const double pa = dec(s.a);
            if (m11) {
                p[(i * 4) + 0] = dec(s.r);
                p[(i * 4) + 1] = dec(s.g);
                p[(i * 4) + 2] = dec(s.b);
            } else {
                p[(i * 4) + 0] = dec(s.r) * pa;
                p[(i * 4) + 1] = dec(s.g) * pa;
                p[(i * 4) + 2] = dec(s.b) * pa;
            }
            p[(i * 4) + 3] = pa;
        }
    });
    std::vector<double> orow(static_cast<size_t>(w2) * 4);
    return from_bands<Rgba8>(w2, h2, Rgba8{}, [&](int y0, int y1, Rgba8* band) {
        for (int y = y0; y < y1; ++y) {
            rs.row(y, orow.data());
            Rgba8* out = band + idx(0, y - y0, w2);
            for (int x = 0; x < w2; ++x) {
                const double* a = &orow[static_cast<size_t>(x) * 4];
                const double A = clamp(a[3], 0.0, 1.0);
                if (m11) {
                    out[x] = canonicalize(
                        Rgba8{q(clamp(a[0], 0.0, 1.0)), q(clamp(a[1], 0.0, 1.0)), q(clamp(a[2], 0.0, 1.0)), q(A)});
                    continue;
                }
                if (A == 0.0) {
                    out[x] = Rgba8{};
                    continue;
                }
                out[x] = canonicalize(
                    Rgba8{q(clamp(a[0], 0.0, A) / A), q(clamp(a[1], 0.0, A) / A), q(clamp(a[2], 0.0, A) / A), q(A)});
            }
        }
    });
}

GrayImage image_size_gray(const GrayImage& img, int w2, int h2, Gray8 bg) {
    const int W = img.width(), H = img.height();
    const TileGrid<Gray8> g(img);
    std::vector<Gray8> srow(static_cast<size_t>(W));
    RowResampler rs(1, W, H, w2, h2, [&](int y, double* p) {
        g.row(y, srow.data());
        for (size_t i = 0; i < srow.size(); ++i) p[i] = dec(srow[i]);
    });
    std::vector<double> orow(static_cast<size_t>(w2));
    return from_bands<Gray8>(w2, h2, bg, [&](int y0, int y1, Gray8* band) {
        for (int y = y0; y < y1; ++y) {
            rs.row(y, orow.data());
            for (int x = 0; x < w2; ++x) band[idx(x, y - y0, w2)] = q(clamp(orow[static_cast<size_t>(x)], 0.0, 1.0));
        }
    });
}

}  // namespace

bool parse_anchor(const std::string& s, Anchor& out) {
    static const char* names[] = {"tl", "t", "tr", "l", "c", "r", "bl", "b", "br"};
    for (int i = 0; i < 9; ++i)
        if (s == names[i]) {
            out = static_cast<Anchor>(i);
            return true;
        }
    return false;
}

// ---- §13 ---------------------------------------------------------------------------------------------
void image_size(DocState& d, int w2, int h2, Interp interp) {
    const int W = d.w, H = d.h;
    if (w2 == W && h2 == H) {  // pixels untouched; the selection is still cleared (doc 50 §4)
        d.selection.reset(W, H);
        return;
    }
    if (interp == Interp::Nearest) {
        const double rx = static_cast<double>(W) / static_cast<double>(w2);
        const double ry = static_cast<double>(H) / static_cast<double>(h2);
        auto map = [&](int x, int y, int64_t& sx, int64_t& sy) {
            sx = std::min<int64_t>(static_cast<int64_t>(std::floor((static_cast<double>(x) + 0.5) * rx)), W - 1);
            sy = std::min<int64_t>(static_cast<int64_t>(std::floor((static_cast<double>(y) + 0.5) * ry)), H - 1);
        };
        remap_document(
            d, w2, h2, "image_size (nearest): dense layer copies",
            [&](const std::vector<Rgba8>& s, int sw, int sh) { return index_map(s, sw, sh, w2, h2, Rgba8{}, map); },
            [&](const std::vector<uint8_t>& s, int sw, int sh) {
                return index_map(s, sw, sh, w2, h2, uint8_t{255}, map);
            });
        return;
    }
    remap_document_tiled(
        d, w2, h2, [&](const RgbaImage& s) { return image_size_rgba(s, w2, h2); },
        [&](const GrayImage& s, Gray8 bg) { return image_size_gray(s, w2, h2, bg); });
}

// ---- §14 ---------------------------------------------------------------------------------------------
void crop(DocState& d, int64_t cx, int64_t cy, int cw, int ch) {
    // Offsets beyond ±2^40 put every source outside the canvas; clamping keeps x + cx exact.
    constexpr int64_t kFar = int64_t{1} << 40;
    cx = std::min(std::max(cx, -kFar), kFar);
    cy = std::min(std::max(cy, -kFar), kFar);
    auto map = [&](int x, int y, int64_t& sx, int64_t& sy) {
        sx = static_cast<int64_t>(x) + cx;
        sy = static_cast<int64_t>(y) + cy;
    };
    remap_document(
        d, cw, ch, "crop / canvas_size: dense layer copies",
        [&](const std::vector<Rgba8>& s, int sw, int sh) { return index_map(s, sw, sh, cw, ch, Rgba8{}, map); },
        [&](const std::vector<uint8_t>& s, int sw, int sh) { return index_map(s, sw, sh, cw, ch, uint8_t{255}, map); });
}

namespace {
int64_t floor_div2(int64_t v) { return (v >= 0) ? (v / 2) : -((-v + 1) / 2); }
}  // namespace

void canvas_size(DocState& d, int w2, int h2, Anchor a) {
    const int64_t dw = static_cast<int64_t>(w2) - d.w, dh = static_cast<int64_t>(h2) - d.h;
    int64_t ox = 0, oy = 0;
    switch (a) {
        case Anchor::TL: case Anchor::L: case Anchor::BL: ox = 0; break;
        case Anchor::T: case Anchor::C: case Anchor::B: ox = floor_div2(dw); break;
        case Anchor::TR: case Anchor::R: case Anchor::BR: ox = dw; break;
    }
    switch (a) {
        case Anchor::TL: case Anchor::T: case Anchor::TR: oy = 0; break;
        case Anchor::L: case Anchor::C: case Anchor::R: oy = floor_div2(dh); break;
        case Anchor::BL: case Anchor::B: case Anchor::BR: oy = dh; break;
    }
    crop(d, -ox, -oy, w2, h2);
}

// ---- §15 ---------------------------------------------------------------------------------------------
namespace {
double normalise_angle(double angle) { return angle - (360.0 * std::floor(angle / 360.0)); }
}  // namespace

std::optional<Size> rotate_canvas_size(int W, int H, double angle) {
    const double a = normalise_angle(angle);
    if (a == 0.0 || a == 180.0) return Size{W, H};
    if (a == 90.0 || a == 270.0) return Size{H, W};
    const double rad = a * (3.141592653589793 / 180.0);
    const double c = std::cos(rad), s = std::sin(rad);
    const double w = static_cast<double>(W), h = static_cast<double>(H);
    const double bw = std::abs(w * c) + std::abs(h * s);
    const double bh = std::abs(w * s) + std::abs(h * c);
    const double cw = std::ceil(bw - 1e-6), chh = std::ceil(bh - 1e-6);
    if (!(cw <= kMaxCanvasSide) || !(chh <= kMaxCanvasSide)) return std::nullopt;
    return Size{std::max(static_cast<int>(cw), 1), std::max(static_cast<int>(chh), 1)};
}

void rotate_canvas(DocState& d, double angle) {
    const int W = d.w, H = d.h;
    const double a = normalise_angle(angle);
    if (a == 0.0) {
        d.selection.reset(W, H);
        return;
    }
    if (a == 90.0 || a == 180.0 || a == 270.0) {
        const int w2 = (a == 180.0) ? W : H, h2 = (a == 180.0) ? H : W;
        auto map = [&](int x, int y, int64_t& sx, int64_t& sy) {
            if (a == 90.0) {
                sx = y;
                sy = (H - 1) - x;
            } else if (a == 180.0) {
                sx = (W - 1) - x;
                sy = (H - 1) - y;
            } else {
                sx = (W - 1) - y;
                sy = x;
            }
        };
        remap_document(
            d, w2, h2, "rotate_canvas: dense layer copies",
            [&](const std::vector<Rgba8>& s, int sw, int sh) { return index_map(s, sw, sh, w2, h2, Rgba8{}, map); },
            [&](const std::vector<uint8_t>& s, int sw, int sh) {
                return index_map(s, sw, sh, w2, h2, uint8_t{255}, map);
            });
        return;
    }
    const Size sz = *rotate_canvas_size(W, H, angle);
    const int w2 = sz.w, h2 = sz.h;
    Mat3 M = transform::mul(transform::Ro(a), transform::Tr(-(static_cast<double>(W) / 2.0), -(static_cast<double>(H) / 2.0)));
    M = transform::mul(transform::Tr(static_cast<double>(w2) / 2.0, static_cast<double>(h2) / 2.0), M);
    const auto inv = transform::inverse(M);
    // A rotation has det 1; inverse() cannot fail here (only under mutation 31's transpose, which
    // is also a rotation).
    const Mat3 n = inv ? *inv : transform::kIdentity;
    remap_document_tiled(
        d, w2, h2, [&](const RgbaImage& s) { return transform::warp_rgba_tiled(s, w2, h2, n, Interp::Bicubic); },
        [&](const GrayImage& s, Gray8 bg) { return transform::warp_gray_tiled(s, w2, h2, n, bg); });
}

// ---- §16 ---------------------------------------------------------------------------------------------
namespace {
template <class Px>
std::vector<Px> flip_dense(const std::vector<Px>& s, int W, int H, bool horizontal) {
    return index_map(s, W, H, W, H, Px{}, [&](int x, int y, int64_t& sx, int64_t& sy) {
        sx = horizontal ? ((W - 1) - x) : x;
        sy = horizontal ? y : ((H - 1) - y);
    });
}
}  // namespace

void flip_canvas(DocState& d, bool horizontal) {
    const int W = d.w, H = d.h;
    remap_document(
        d, W, H, "flip_canvas: dense layer copies",
        [&](const std::vector<Rgba8>& s, int sw, int sh) { return flip_dense(s, sw, sh, horizontal); },
        [&](const std::vector<uint8_t>& s, int sw, int sh) { return flip_dense(s, sw, sh, horizontal); });
}

void flip_layer(Node& raster, bool horizontal) {
    const int W = raster.pixels.width(), H = raster.pixels.height();
    const mem::Reservation hold = reserve_dense(2 * dense_bytes(W, H, sizeof(Rgba8)), "flip_layer: dense layer copies");
    raster.pixels = from_dense<Rgba8>(flip_dense(to_dense(raster.pixels), W, H, horizontal), W, H);
}

bool transform_layer(Node& raster, const Mat3& m, Interp interp) {
    auto img = transform::transform_image(raster.pixels, m, interp);
    if (!img) return false;
    raster.pixels = std::move(*img);
    return true;
}

}  // namespace rl::geom
