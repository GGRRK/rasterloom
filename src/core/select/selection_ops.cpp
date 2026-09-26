// SPDX-License-Identifier: GPL-3.0-or-later
//
// docs/math/30-geometry-selection.md §1.3 (sample grid), §3 (inside tests), §4 (combine), §5, §6
// (region), §7 (feather), §8 (expand/contract).
//
// Rasterisation strategy (§1.3 allows any shortcut that yields the identical n per pixel): sample
// column k (k = 16*x + i for AA, k = x for non-AA) sits at the exact double (2k + 1) / (2F), which
// equals x + o(i) (AA, F = 16) or x + 0.5 (non-AA, F = 1) bit for bit. For every sample row the set
// of inside sample columns is computed as exact intervals with each shape's own inside test
// evaluated exactly as the doc writes it, then counted per pixel.
#include "core/select/selection_ops.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <unordered_map>

#include "core/base/mutation.hpp"
#include "core/base/quant.hpp"
#include "core/geometry/dense.hpp"

namespace rl::select {

namespace {

using geom::idx;

// Position of sample k on an axis sampled F times per pixel: (2k + 1) / (2F), exact.
inline double spos(int64_t k, int F) {
    return static_cast<double>((2 * k) + 1) / static_cast<double>(2 * F);
}

// Smallest k in [0, N] with spos(k) >= c (N when none). spos is strictly increasing in k.
int64_t first_ge(double c, int64_t N, int F) {
    int64_t lo = 0, hi = N;  // answer in [lo, hi]
    while (lo < hi) {
        const int64_t mid = lo + ((hi - lo) / 2);
        if (spos(mid, F) >= c)
            hi = mid;
        else
            lo = mid + 1;
    }
    return lo;
}

// Adds the sample-column interval [ka, kb) (clipped to [0, F*W)) to the per-pixel counts.
void add_interval(std::vector<int>& n, int64_t ka, int64_t kb, int F) {
    const int64_t N = static_cast<int64_t>(n.size()) * F;
    ka = std::max<int64_t>(ka, 0);
    kb = std::min<int64_t>(kb, N);
    if (ka >= kb) return;
    const int64_t x0 = ka / F, x1 = (kb - 1) / F;
    if (x0 == x1) {
        n[static_cast<size_t>(x0)] += static_cast<int>(kb - ka);
        return;
    }
    n[static_cast<size_t>(x0)] += static_cast<int>((F * (x0 + 1)) - ka);
    for (int64_t x = x0 + 1; x < x1; ++x) n[static_cast<size_t>(x)] += F;
    n[static_cast<size_t>(x1)] += static_cast<int>(kb - (F * x1));
}

// Drives the per-sample-row interval producer `row_intervals(py, emit)` over the canvas and turns
// counts into coverage bytes (§1.3).
template <class RowFn>
std::vector<uint8_t> rasterize(int W, int H, bool aa, RowFn&& row_intervals) {
    const int F = aa ? 16 : 1;
    const mem::Reservation hold = geom::reserve_dense(geom::dense_bytes(W, H, 1), "selection shape coverage");
    std::vector<uint8_t> out(static_cast<size_t>(W) * static_cast<size_t>(H), 0);
    std::vector<int> n(static_cast<size_t>(W), 0);
    const int64_t N = static_cast<int64_t>(W) * F;
    for (int y = 0; y < H; ++y) {
        std::fill(n.begin(), n.end(), 0);
        bool any = false;
        for (int j = 0; j < F; ++j) {
            const double py = spos((static_cast<int64_t>(y) * F) + j, F);
            row_intervals(py, N, F, [&](int64_t ka, int64_t kb) {
                if (ka < kb) any = true;
                add_interval(n, ka, kb, F);
            });
        }
        if (!any) continue;
        uint8_t* row = &out[idx(0, y, W)];
        for (int x = 0; x < W; ++x) {
            const int c = n[static_cast<size_t>(x)];
            if (aa)
                row[x] = q(static_cast<double>(c) / 256.0);
            else
                row[x] = c > 0 ? uint8_t{255} : uint8_t{0};
        }
    }
    return out;
}

int64_t isqrt64(int64_t v) {
    int64_t s = static_cast<int64_t>(std::sqrt(static_cast<double>(v)));
    while (s > 0 && s * s > v) --s;
    while ((s + 1) * (s + 1) <= v) ++s;
    return s;
}

}  // namespace

bool parse_mode(const std::string& s, Mode& out) {
    if (s == "new") out = Mode::New;
    else if (s == "add") out = Mode::Add;
    else if (s == "subtract") out = Mode::Subtract;
    else if (s == "intersect") out = Mode::Intersect;
    else return false;
    return true;
}

// ---- §3.1 rectangle --------------------------------------------------------------------------------
std::vector<uint8_t> rasterize_rect(int W, int H, double x, double y, double w, double h, bool aa) {
    const double x1 = x + w;
    const double y1 = y + h;
    return rasterize(W, H, aa, [&](double py, int64_t N, int F, auto&& emit) {
        if (!((y <= py) && (py < y1))) return;
        // (x <= px) and (px < x1)  <=>  first_ge(x) <= k < first_ge(x1)
        emit(first_ge(x, N, F), first_ge(x1, N, F));
    });
}

// ---- §3.2 ellipse ----------------------------------------------------------------------------------
std::vector<uint8_t> rasterize_ellipse(int W, int H, double x, double y, double w, double h, bool aa) {
    const double rx = w / 2.0;
    const double ry = h / 2.0;
    const double cx = x + rx;
    const double cy = y + ry;
    return rasterize(W, H, aa, [&](double py, int64_t N, int F, auto&& emit) {
        const double dy = (py - cy) / ry;
        const double dy2 = dy * dy;
        if (!(dy2 <= 1.0)) return;  // (dx*dx) + dy2 >= dy2 > 1 for every sample
        auto inside = [&](int64_t k) {
            const double dx = (spos(k, F) - cx) / rx;
            return ((dx * dx) + dy2) <= 1.0;
        };
        // Left of the centre (px < cx) the test is monotone false..true in k; right of it (px >= cx)
        // true..false (correctly rounded -, /, *, + are monotone).
        const int64_t sc = first_ge(cx, N, F);
        int64_t a = sc, b = sc;  // inside columns = [a, b)
        if (sc > 0 && inside(sc - 1)) {
            int64_t lo = 0, hi = sc - 1;  // lowest true in [lo, hi]
            while (lo < hi) {
                const int64_t mid = lo + ((hi - lo) / 2);
                if (inside(mid))
                    hi = mid;
                else
                    lo = mid + 1;
            }
            a = lo;
        }
        if (sc < N && inside(sc)) {
            int64_t lo = sc, hi = N - 1;  // highest true in [lo, hi]
            while (lo < hi) {
                const int64_t mid = lo + ((hi - lo + 1) / 2);
                if (inside(mid))
                    lo = mid;
                else
                    hi = mid - 1;
            }
            b = lo + 1;
        }
        emit(a, b);
    });
}

// ---- §3.3 polygon ----------------------------------------------------------------------------------
std::vector<uint8_t> rasterize_polygon(int W, int H, const std::vector<Pt>& P, bool aa) {
    const size_t n = P.size();
    if (n < 3) return std::vector<uint8_t>(static_cast<size_t>(W) * static_cast<size_t>(H), 0);
    double ymin = P[0].y, ymax = P[0].y;
    for (const Pt& p : P) {
        ymin = std::min(ymin, p.y);
        ymax = std::max(ymax, p.y);
    }
    const bool winding = mut::active(33);
    struct Cross {
        double xc;
        int dir;
    };
    std::vector<Cross> xs;
    std::vector<int> suffix;
    return rasterize(W, H, aa, [&](double py, int64_t N, int F, auto&& emit) {
        // A crossing needs one endpoint above py and one at/below it.
        if (!(py >= ymin && py < ymax)) return;
        xs.clear();
        for (size_t k = 0; k < n; ++k) {
            const double xi = P[k].x, yi = P[k].y;
            const double xj = P[(k + n - 1) % n].x, yj = P[(k + n - 1) % n].y;
            if ((yi > py) != (yj > py)) {
                const double xc = (((xj - xi) * (py - yi)) / (yj - yi)) + xi;
                if (std::isnan(xc)) continue;  // `px < NaN` is never true: the edge never toggles
                xs.push_back(Cross{xc, (yi > yj) ? +1 : -1});
            }
        }
        if (xs.empty()) return;
        std::sort(xs.begin(), xs.end(), [](const Cross& a, const Cross& b) { return a.xc < b.xc; });
        const size_t m = xs.size();
        // For px in [c_j, c_{j+1}) exactly the crossings with sorted index >= j (0-based: > j-1)
        // satisfy px < xc. suffix[j] = sum of dir over indices >= j.
        suffix.assign(m + 1, 0);
        for (size_t j = m; j-- > 0;) suffix[j] = suffix[j + 1] + xs[j].dir;
        int64_t start = 0;  // first sample column of the current gap
        for (size_t j = 0; j <= m; ++j) {
            const int64_t end = (j < m) ? first_ge(xs[j].xc, N, F) : N;
            // Gap j: samples with c_{j-1} <= px < c_j; crossings counted = indices j..m-1.
            const size_t count = m - j;
            const bool in = winding ? (suffix[j] != 0) : ((count % 2) == 1);
            if (in && start < end) emit(start, end);
            start = std::max(start, end);
        }
    });
}

// ---- §6 region -------------------------------------------------------------------------------------
std::vector<uint8_t> region(const std::vector<Rgba8>& px, int W, int H, int64_t sx, int64_t sy, int t,
                            bool contiguous, bool aa) {
    const size_t total = static_cast<size_t>(W) * static_cast<size_t>(H);
    // Rg, d and (antialiased with t > 0) the fringe plane; the flood stack is charged as it grows.
    const mem::Reservation hold =
        geom::reserve_dense(geom::dense_bytes(W, H, (aa && t > 0) ? 3 : 2), "colour region planes");
    std::vector<uint8_t> Rg(total, 0);
    if (sx < 0 || sy < 0 || sx >= W || sy >= H) return Rg;
    const Rgba8 s = px[idx(static_cast<int>(sx), static_cast<int>(sy), W)];
    std::vector<uint8_t> d(total);
    for (size_t i = 0; i < total; ++i) {
        const Rgba8 p = px[i];
        const int dr = std::abs(static_cast<int>(p.r) - static_cast<int>(s.r));
        const int dg = std::abs(static_cast<int>(p.g) - static_cast<int>(s.g));
        const int db = std::abs(static_cast<int>(p.b) - static_cast<int>(s.b));
        const int da = std::abs(static_cast<int>(p.a) - static_cast<int>(s.a));
        d[i] = static_cast<uint8_t>(std::max(std::max(dr, dg), std::max(db, da)));
    }
    const bool m35 = mut::active(35);
    auto match = [&](size_t i) {
        if (m35) return (static_cast<double>(d[i]) / 255.0) <= static_cast<double>(t);
        return static_cast<int>(d[i]) <= t;
    };
    // C: 255 marks membership.
    if (!contiguous) {
        for (size_t i = 0; i < total; ++i)
            if (match(i)) Rg[i] = 255;
    } else {
        const bool four = mut::active(15);
        std::vector<std::pair<int, int>> stack;
        mem::Reservation stack_hold;
        auto push = [&](int px_, int py_) {
            if (stack.size() == stack.capacity()) {
                const size_t cap = std::max<size_t>(64, stack.capacity() * 2);
                // Charged before the vector grows; old and new buffers are both live while it moves.
                mem::Reservation grown(cap * sizeof(stack[0]), "flood-fill stack");
                stack.reserve(cap);
                stack_hold = std::move(grown);
            }
            stack.emplace_back(px_, py_);
        };
        push(static_cast<int>(sx), static_cast<int>(sy));
        Rg[idx(static_cast<int>(sx), static_cast<int>(sy), W)] = 255;
        while (!stack.empty()) {
            const auto [x, y] = stack.back();
            stack.pop_back();
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    if (dx == 0 && dy == 0) continue;
                    if (four && dx != 0 && dy != 0) continue;
                    const int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
                    const size_t i = idx(nx, ny, W);
                    if (Rg[i] == 255 || !match(i)) continue;
                    Rg[i] = 255;
                    push(nx, ny);
                }
            }
        }
    }
    if (aa && t > 0) {
        // Fringe: the 1-pixel ring (8-neighbourhood, unchanged under mutation 15) around C.
        std::vector<uint8_t> fringe(total, 0);
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                const size_t i = idx(x, y, W);
                if (Rg[i] == 255) continue;
                bool near = false;
                for (int dy = -1; dy <= 1 && !near; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (dx == 0 && dy == 0) continue;
                        const int nx = x + dx, ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
                        if (Rg[idx(nx, ny, W)] == 255) {
                            near = true;
                            break;
                        }
                    }
                if (!near) continue;
                const double a = 1.5 - (static_cast<double>(d[i]) / static_cast<double>(t));
                fringe[i] = (a <= 0.0) ? uint8_t{0} : q(a * 2.0);
            }
        }
        for (size_t i = 0; i < total; ++i)
            if (Rg[i] != 255) Rg[i] = fringe[i];
    }
    return Rg;
}

// ---- state ------------------------------------------------------------------------------------------
std::vector<uint8_t> mask_dense(const Selection& sel) { return geom::to_dense(sel.mask); }

void set_mask_dense(Selection& sel, const std::vector<uint8_t>& m) {
    sel.mask = geom::from_dense<Gray8>(m, sel.mask.width(), sel.mask.height(), 0);
}

void combine(Selection& sel, const std::vector<uint8_t>& B, Mode mode) {
    // B (the caller's plane, live for the whole call) and, when combining, the dense S.
    const mem::Reservation hold =
        geom::reserve_dense(B.size() + ((mode == Mode::New) ? 0 : B.size()), "selection combine planes");
    if (mode == Mode::New) {
        set_mask_dense(sel, B);
        return;
    }
    std::vector<uint8_t> S = mask_dense(sel);
    const bool m34 = mut::active(34);
    for (size_t i = 0; i < S.size(); ++i) {
        const int s = S[i], b = B[i];
        int r = 0;
        switch (mode) {
            case Mode::Add: r = std::max(s, b); break;
            case Mode::Subtract: r = m34 ? std::min(s, 255 - b) : std::max(s - b, 0); break;
            case Mode::Intersect: r = std::min(s, b); break;
            case Mode::New: r = b; break;
        }
        S[i] = static_cast<uint8_t>(r);
    }
    set_mask_dense(sel, S);
}

void select_all(Selection& sel) { sel.mask.reset(sel.mask.width(), sel.mask.height(), 255); }

void deselect(Selection& sel) {
    if (sel.active()) sel.saved = sel.mask;
    sel.mask.reset(sel.mask.width(), sel.mask.height(), 0);
}

void reselect(Selection& sel) {
    if (sel.saved) sel.mask = *sel.saved;
}

void select_inverse(Selection& sel) {
    const mem::Reservation hold =
        geom::reserve_dense(geom::dense_bytes(sel.mask.width(), sel.mask.height(), 1), "select_inverse plane");
    std::vector<uint8_t> S = mask_dense(sel);
    for (uint8_t& v : S) v = static_cast<uint8_t>(255 - v);
    set_mask_dense(sel, S);
}

// ---- §7 feather -------------------------------------------------------------------------------------
std::vector<int64_t> box_widths(double sigma) {
    const double s12 = (12.0 * sigma) * sigma;
    const double wIdeal = std::sqrt((s12 / 3.0) + 1.0);
    int64_t wl = static_cast<int64_t>(std::floor(wIdeal));
    if (wl % 2 == 0) wl = wl - 1;
    const int64_t wu = wl + 2;
    const double mIdeal = (((s12 - static_cast<double>(3 * wl * wl)) - static_cast<double>(12 * wl)) - 9.0) /
                          static_cast<double>((-4 * wl) - 4);
    int64_t m = static_cast<int64_t>(std::floor(mIdeal + 0.5));
    m = std::min<int64_t>(std::max<int64_t>(m, 0), 3);
    std::vector<int64_t> widths(3);
    for (int64_t i = 0; i < 3; ++i) widths[static_cast<size_t>(i)] = (i < m) ? wl : wu;
    return widths;
}

namespace {

// In-place box sum of half-width h over `n` elements at stride `step`, edge replicate. Sliding
// window: every intermediate is a window sum or a window sum minus one member, so it never exceeds
// the largest window sum (no signed overflow; doc 50 §4 note on doc 30 §7).
void box_sum_line(int64_t* base, size_t stride, int n, int64_t h, std::vector<int64_t>& tmp) {
    tmp.resize(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) tmp[static_cast<size_t>(i)] = base[static_cast<size_t>(i) * stride];
    auto at = [&](int64_t i) {
        const int64_t c = std::min<int64_t>(std::max<int64_t>(i, 0), n - 1);
        return tmp[static_cast<size_t>(c)];
    };
    int64_t sum = 0;
    for (int64_t k = -h; k <= h; ++k) sum += at(k);
    base[0] = sum;
    for (int64_t x = 1; x < n; ++x) {
        sum -= at(x - 1 - h);
        sum += at(x + h);
        base[static_cast<size_t>(x) * stride] = sum;
    }
}

}  // namespace

void feather(Selection& sel, double r) {
    if (r == 0.0 || !sel.active()) return;
    const int W = sel.mask.width(), H = sel.mask.height();
    const double sigma = mut::active(32) ? r : (r / 2.0);
    const std::vector<int64_t> widths = box_widths(sigma);
    // S (1 B/px), the int64 box-sum plane T (8 B/px) and the result (1 B/px).
    const mem::Reservation hold =
        geom::reserve_dense(geom::dense_bytes(W, H, 1 + sizeof(int64_t) + 1), "feather planes");
    const std::vector<uint8_t> S = mask_dense(sel);
    std::vector<int64_t> T(S.begin(), S.end());
    std::vector<int64_t> tmp;
    for (int64_t w : widths) {
        const int64_t h = (w - 1) / 2;
        for (int y = 0; y < H; ++y) box_sum_line(&T[idx(0, y, W)], 1, W, h, tmp);
        for (int x = 0; x < W; ++x) box_sum_line(&T[static_cast<size_t>(x)], static_cast<size_t>(W), H, h, tmp);
    }
    const int64_t p = (widths[0] * widths[1]) * widths[2];
    const int64_t D = p * p;
    std::vector<uint8_t> out(S.size());
    for (size_t i = 0; i < T.size(); ++i) out[i] = static_cast<uint8_t>(((2 * T[i]) + D) / (2 * D));
    set_mask_dense(sel, out);
}

// ---- §8 expand / contract ---------------------------------------------------------------------------
namespace {

// Running max (dilate) or min (erode) of half-width `half` over one row, window clipped to the
// canvas (outside values are 0 for max / 255 for min, which never win).
void window_extreme(const uint8_t* row, int W, int64_t half, bool is_max, std::vector<uint8_t>& out,
                    std::vector<int>& dq) {
    out.resize(static_cast<size_t>(W));
    dq.clear();
    size_t head = 0;
    auto better = [&](uint8_t a, uint8_t b) { return is_max ? (a >= b) : (a <= b); };
    int64_t next = 0;  // next index to push
    for (int x = 0; x < W; ++x) {
        const int64_t hi = std::min<int64_t>(x + half, W - 1);
        while (next <= hi) {
            while (dq.size() > head && better(row[next], row[dq.back()])) dq.pop_back();
            dq.push_back(static_cast<int>(next));
            ++next;
        }
        const int64_t lo = x - half;
        while (dq[head] < lo) ++head;
        out[static_cast<size_t>(x)] = row[dq[head]];
    }
}

void morph(Selection& sel, int N, bool dilate) {
    if (!sel.active()) return;
    const int W = sel.mask.width(), H = sel.mask.height();
    const mem::Reservation hold = geom::reserve_dense(geom::dense_bytes(W, H, 2), "expand/contract planes");
    const std::vector<uint8_t> S = mask_dense(sel);
    const uint8_t neutral = dilate ? 0 : 255;
    std::vector<uint8_t> out(S.size(), neutral);
    std::vector<bool> row_neutral(static_cast<size_t>(H), true);
    for (int y = 0; y < H; ++y) {
        const uint8_t* r = &S[idx(0, y, W)];
        for (int x = 0; x < W; ++x)
            if (r[x] != neutral) {
                row_neutral[static_cast<size_t>(y)] = false;
                break;
            }
    }
    std::vector<uint8_t> win;
    std::vector<int> dq;
    const int64_t NN = static_cast<int64_t>(N) * N;
    for (int dy = -N; dy <= N; ++dy) {
        const int64_t half = isqrt64(NN - (static_cast<int64_t>(dy) * dy));
        for (int y = 0; y < H; ++y) {
            const int sy = y + dy;
            if (sy < 0 || sy >= H || row_neutral[static_cast<size_t>(sy)]) continue;
            window_extreme(&S[idx(0, sy, W)], W, half, dilate, win, dq);
            uint8_t* o = &out[idx(0, y, W)];
            for (int x = 0; x < W; ++x) o[x] = dilate ? std::max(o[x], win[static_cast<size_t>(x)])
                                                      : std::min(o[x], win[static_cast<size_t>(x)]);
        }
    }
    set_mask_dense(sel, out);
}

}  // namespace

void expand(Selection& sel, int n) { morph(sel, n, true); }
void contract(Selection& sel, int n) { morph(sel, n, false); }

// ---- marching ants --------------------------------------------------------------------------------
GrayImage edge_mask(const GrayImage& mask, uint8_t threshold) {
    const int W = mask.width(), H = mask.height();
    const mem::Reservation hold = geom::reserve_dense(geom::dense_bytes(W, H, 2), "selection edge planes");
    const std::vector<uint8_t> S = geom::to_dense(mask);
    auto sel = [&](int x, int y) { return x >= 0 && y >= 0 && x < W && y < H && S[idx(x, y, W)] >= threshold; };
    std::vector<uint8_t> out(S.size(), 0);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            if (sel(x, y) && (!sel(x - 1, y) || !sel(x + 1, y) || !sel(x, y - 1) || !sel(x, y + 1)))
                out[idx(x, y, W)] = 255;
    return geom::from_dense<Gray8>(out, W, H, 0);
}

std::vector<std::vector<Pt>> outline_polylines(const GrayImage& mask, uint8_t threshold) {
    const int W = mask.width(), H = mask.height();
    const mem::Reservation hold = geom::reserve_dense(geom::dense_bytes(W, H, 1), "selection outline plane");
    const std::vector<uint8_t> S = geom::to_dense(mask);
    auto sel = [&](int x, int y) { return x >= 0 && y >= 0 && x < W && y < H && S[idx(x, y, W)] >= threshold; };
    struct Edge {
        int x0, y0, x1, y1;
    };
    std::vector<Edge> edges;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            if (!sel(x, y)) continue;
            if (!sel(x, y - 1)) edges.push_back({x, y, x + 1, y});
            if (!sel(x + 1, y)) edges.push_back({x + 1, y, x + 1, y + 1});
            if (!sel(x, y + 1)) edges.push_back({x + 1, y + 1, x, y + 1});
            if (!sel(x - 1, y)) edges.push_back({x, y + 1, x, y});
        }
    // Outgoing edges per vertex (at most 2).
    const int64_t VW = static_cast<int64_t>(W) + 1;
    auto vkey = [&](int x, int y) { return (static_cast<int64_t>(y) * VW) + x; };
    std::unordered_map<int64_t, std::array<int, 2>> out;
    out.reserve(edges.size());
    for (size_t i = 0; i < edges.size(); ++i) {
        auto it = out.find(vkey(edges[i].x0, edges[i].y0));
        if (it == out.end())
            out.emplace(vkey(edges[i].x0, edges[i].y0), std::array<int, 2>{static_cast<int>(i), -1});
        else
            it->second[1] = static_cast<int>(i);
    }
    std::vector<bool> used(edges.size(), false);
    std::vector<std::vector<Pt>> loops;
    for (size_t first = 0; first < edges.size(); ++first) {
        if (used[first]) continue;
        std::vector<std::pair<int, int>> verts;
        size_t e = first;
        while (!used[e]) {
            used[e] = true;
            verts.emplace_back(edges[e].x0, edges[e].y0);
            const int dx = edges[e].x1 - edges[e].x0, dy = edges[e].y1 - edges[e].y0;
            const auto& cand = out.at(vkey(edges[e].x1, edges[e].y1));
            int next = cand[0];
            if (cand[1] >= 0) {
                // Two ways on (a corner shared by diagonal pixels): take the right turn, which keeps
                // the loop around the current region. Right of (dx, dy) in y-down space: (-dy, dx).
                const Edge& a = edges[static_cast<size_t>(cand[0])];
                const int ax = a.x1 - a.x0, ay = a.y1 - a.y0;
                next = (ax == -dy && ay == dx) ? cand[0] : cand[1];
            }
            e = static_cast<size_t>(next);
        }
        // Drop vertices in the middle of straight runs.
        std::vector<Pt> loop;
        const size_t m = verts.size();
        for (size_t i = 0; i < m; ++i) {
            const auto& p = verts[(i + m - 1) % m];
            const auto& c = verts[i];
            const auto& nx = verts[(i + 1) % m];
            const int d1x = c.first - p.first, d1y = c.second - p.second;
            const int d2x = nx.first - c.first, d2y = nx.second - c.second;
            if ((d1x * d2y) - (d1y * d2x) == 0) continue;  // collinear (never a U-turn on a boundary)
            loop.push_back(Pt{static_cast<double>(c.first), static_cast<double>(c.second)});
        }
        loops.push_back(std::move(loop));
    }
    return loops;
}

}  // namespace rl::select
