// SPDX-License-Identifier: GPL-3.0-or-later
//
// StrokeSession: the doc 40 stroke pipeline (§3), evaluated exactly in the doc's order (C1).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

#include "core/base/error.hpp"
#include "core/base/mutation.hpp"
#include "core/base/quant.hpp"
#include "core/brush/brush.hpp"

namespace rl::brush {

namespace {

constexpr double kPi = 3.141592653589793;
constexpr size_t kMaxDabs = 1000000;

// doc 40 notation rhu(x): round half up. Never std::round (differs for negative x).
double rhu(double x) {
    const double fl = std::floor(x);
    return fl + (((x - fl) >= 0.5) ? 1.0 : 0.0);
}

// Canvas-pixel range helper: clamps a double to [lo, hi] and converts (both finite here).
int clamp_to_int(double v, int lo, int hi) {
    if (v < static_cast<double>(lo)) return lo;
    if (v > static_cast<double>(hi)) return hi;
    return static_cast<int>(v);
}

void check_range(double v, double lo, double hi, const std::string& ctx, const char* name) {
    if (!std::isfinite(v) || !(v >= lo && v <= hi)) {
        char buf[160];
        std::snprintf(buf, sizeof buf, ": %s = %.17g out of range [%.17g, %.17g]", name, v, lo, hi);
        throw ScriptError(ctx + buf);
    }
}

void check_curve(const Curve& c, const std::string& ctx, const char* name) {
    const std::string where = ctx + ": " + name;
    if (c.size() < 2 || c.size() > 16) throw ScriptError(where + " must have 2 to 16 points");
    for (size_t i = 0; i < c.size(); ++i) {
        for (double v : c[i])
            if (!std::isfinite(v) || !(v >= 0.0 && v <= 1.0))
                throw ScriptError(where + ": point coordinates must be in [0, 1]");
        if (i > 0 && !(c[i][0] > c[i - 1][0])) throw ScriptError(where + ": x must be strictly increasing");
    }
    if (c.front()[0] != 0.0) throw ScriptError(where + ": first x must be exactly 0.0");
    if (c.back()[0] != 1.0) throw ScriptError(where + ": last x must be exactly 1.0");
}

// One stroke-buffer tile: 64x64 doubles, row-major, zero-initialised.
struct BTile {
    std::array<double, kTilePixels> v{};
};

// Per-dab geometry (doc 40 §3.5/§3.6).
struct DabGeom {
    double cx, cy, R, Rm, he, ca, sa;
};

// doc 40 §3.6 dab mask for pixel (px, py). Mutation 36 inverts the falloff band.
inline double mask_at(const DabGeom& g, int px, int py) {
    const double ddx = (static_cast<double>(px) + 0.5) - g.cx;
    const double ddy = (static_cast<double>(py) + 0.5) - g.cy;
    const double uu = (ddx * g.ca) - (ddy * g.sa);
    const double vv = (ddx * g.sa) + (ddy * g.ca);
    const double nu = uu / g.R;
    const double nv = vv / g.Rm;
    const double r = std::sqrt((nu * nu) + (nv * nv));
    if (r >= 1.0) return 0.0;
    if (r <= g.he) return 1.0;
    const double t = (r - g.he) / (1.0 - g.he);
    if (mut::active(36)) return (t * t) * (3.0 - (2.0 * t));
    return 1.0 - ((t * t) * (3.0 - (2.0 * t)));
}

// doc 40 §3.7 one stroke-buffer update (revised 2026-09-26): the ceiling is the dab's opacity O
// (Wash) or full coverage (Build-up, kb = f * O); the mask m scales the rate, so overlapping soft
// falloffs accumulate as 1 - prod(1 - f * m_i) instead of taking their maximum.
// Mutation 7: repeated source-over. Mutation 42: the pre-2026-09-26 model (mask-shaped ceiling).
inline double buffer_update(double b, double m, double f, double O, double kb, bool wash, bool m7, bool m42) {
    if (m7) return b + (((f * O) * m) * (1.0 - b));
    if (m42) {
        if (wash) {
            const double T = O * m;
            return (T > b) ? b + (f * (T - b)) : b;
        }
        return (m > b) ? b + (kb * (m - b)) : b;
    }
    if (wash) return (O > b) ? b + ((f * m) * (O - b)) : b;
    return b + ((kb * m) * (1.0 - b));
}

DabGeom make_geom(double cx, double cy, double d, double hardness, double roundness, double ca, double sa) {
    const double d_draw = std::max(d, 1.0);
    const double R = d_draw / 2.0;
    const double Rm = R * roundness;
    const double w = (Rm > 1.0) ? (1.0 / Rm) : 1.0;
    const double he = std::min(hardness, 1.0 - w);
    return DabGeom{cx, cy, R, Rm, he, ca, sa};
}

std::string hex_color(Rgba8 c) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "#%02X%02X%02X", c.r, c.g, c.b);
    return buf;
}

}  // namespace

const char* op_name(Tool t) {
    switch (t) {
        case Tool::Brush: return "brush_stroke";
        case Tool::Eraser: return "eraser_stroke";
        case Tool::Clone: return "clone_stroke";
    }
    return "brush_stroke";
}

void validate(const StrokeParams& p, const std::string& ctx) {
    if (p.tool == Tool::Brush && p.color.a != 255) throw ScriptError(ctx + ": color alpha must be FF");
    if (p.tool != Tool::Brush && p.target != Target::Pixels)
        throw ScriptError(ctx + ": only brush_stroke can target a mask");
    check_range(p.size, 1.0, 5000.0, ctx, "size");
    check_range(p.hardness, 0.0, 1.0, ctx, "hardness");
    check_range(p.spacing, 0.01, 10.0, ctx, "spacing");
    check_range(p.opacity, 0.0, 1.0, ctx, "opacity");
    check_range(p.flow, 0.0, 1.0, ctx, "flow");
    check_range(p.angle, -360.0, 360.0, ctx, "angle");
    check_range(p.roundness, 0.01, 1.0, ctx, "roundness");
    check_range(p.dabs_per_second, 0.0, 1000.0, ctx, "dabs_per_second");
    check_range(p.smoothing, 0.0, 0.99, ctx, "smoothing");
    check_range(p.view_zoom, 0.0, 256.0, ctx, "view_zoom");
    if (p.view_zoom == 0.0) throw ScriptError(ctx + ": view_zoom must be > 0");
    if (p.size_curve) check_curve(*p.size_curve, ctx, "size_curve");
    if (p.opacity_curve) check_curve(*p.opacity_curve, ctx, "opacity_curve");
    if (p.source) {
        check_range((*p.source)[0], -1e6, 1e6, ctx, "source x");
        check_range((*p.source)[1], -1e6, 1e6, ctx, "source y");
    }
}

void validate(const Sample& s, const std::string& ctx) {
    check_range(s.x, -1e6, 1e6, ctx, "x");
    check_range(s.y, -1e6, 1e6, ctx, "y");
    if (!std::isfinite(s.pressure)) throw ScriptError(ctx + ": pressure must be finite");
    check_range(s.tilt_x, -90.0, 90.0, ctx, "tilt_x");
    check_range(s.tilt_y, -90.0, 90.0, ctx, "tilt_y");
    check_range(s.t_ms, 0.0, std::numeric_limits<double>::max(), ctx, "t_ms");
}

nlohmann::json to_op(const std::string& layer_id, const StrokeParams& p, const std::vector<Sample>& samples) {
    using nlohmann::json;
    json op = json::object();
    op["op"] = op_name(p.tool);
    op["layer"] = layer_id;
    if (p.tool == Tool::Brush) {
        op["target"] = p.target == Target::Mask ? "mask" : "pixels";
        op["color"] = hex_color(p.color);
    }
    op["size"] = p.size;
    op["hardness"] = p.hardness;
    op["spacing"] = p.spacing;
    op["opacity"] = p.opacity;
    op["flow"] = p.flow;
    op["angle"] = p.angle;
    op["roundness"] = p.roundness;
    op["mode"] = p.mode == Mode::Buildup ? "buildup" : "wash";
    op["dabs_per_second"] = p.dabs_per_second;
    op["smoothing"] = p.smoothing;
    auto curve = [](const std::optional<Curve>& c) {
        if (!c) return json(nullptr);
        json a = json::array();
        for (const auto& pt : *c) a.push_back(json::array({pt[0], pt[1]}));
        return a;
    };
    op["size_curve"] = curve(p.size_curve);
    op["opacity_curve"] = curve(p.opacity_curve);
    op["view_zoom"] = p.view_zoom;
    if (p.tool == Tool::Clone) {
        if (p.source) op["source"] = json::array({(*p.source)[0], (*p.source)[1]});
        op["aligned"] = p.aligned;
        if (!p.source_layer.empty()) op["source_layer"] = p.source_layer;
    }
    json arr = json::array();
    for (const Sample& s : samples) {
        arr.push_back(json{{"x", s.x}, {"y", s.y}, {"pressure", s.pressure}, {"tilt_x", s.tilt_x},
                           {"tilt_y", s.tilt_y}, {"t_ms", s.t_ms}});
    }
    op["samples"] = std::move(arr);
    return op;
}

namespace detail {

std::array<uint16_t, 256> bake_lut(const Curve& c) {
    std::array<uint16_t, 256> lut{};
    const size_t n = c.size();
    for (int i = 0; i < 256; ++i) {
        const double x = static_cast<double>(i) / 255.0;
        size_t k = 0;
        for (size_t j = 0; j + 1 < n; ++j)
            if (c[j][0] <= x) k = j;
        const double t = (x - c[k][0]) / (c[k + 1][0] - c[k][0]);
        const double y = c[k][1] + (t * (c[k + 1][1] - c[k][1]));
        lut[static_cast<size_t>(i)] = static_cast<uint16_t>(rhu(clamp(y, 0.0, 1.0) * 65535.0));
    }
    return lut;
}

double dab_mask(double cx, double cy, double size, double hardness, double roundness, double angle, int px,
                int py) {
    const double theta = angle * (kPi / 180.0);
    const DabGeom g = make_geom(cx, cy, size, hardness, roundness, std::cos(theta), std::sin(theta));
    return mask_at(g, px, py);
}

double accumulate(double b, double m, double flow, double opacity, Mode mode) {
    return buffer_update(b, m, flow, opacity, flow * opacity, mode == Mode::Wash, mut::active(7), mut::active(42));
}

}  // namespace detail

// =================================================================================================

struct StrokeSession::Impl {
    // ---- configuration ----
    Document* doc = nullptr;  // null in walk-only mode (detail::walk)
    std::string layer_id;
    std::string ctx;
    StrokeParams p;
    SessionOptions opts;
    CloneState* clone = nullptr;
    int W = 0, H = 0;
    bool active = false;

    // ---- snapshots (immutable for the stroke) ----
    RgbaImage s0;         // target pixels at begin (Target::Pixels)
    GrayImage s0m;        // target mask at begin (Target::Mask)
    RgbaImage src;        // clone source layer at begin
    GrayImage sel;        // selection mask at begin
    bool sel_active = false;
    bool lock = false;

    // ---- preview ----
    RgbaImage prev;
    GrayImage prevm;

    // ---- per-stroke constants ----
    bool has_size_lut = false, has_op_lut = false;
    std::array<uint16_t, 256> size_lut{}, op_lut{};
    double ca = 1.0, sa = 0.0;
    double ema_m = 0.0, ema_a = 1.0;
    double Cs[3] = {0, 0, 0};
    double luma = 0.0;

    // ---- walk state ----
    size_t k = 0;
    double frac = 0.0;
    double step = 0.0;
    size_t dab_count = 0;
    double xs_prev = 0, ys_prev = 0, p_prev = 0, te_prev = 0;
    int64_t oxi = 0, oyi = 0;

    // ---- stroke buffer, sparse by tile ----
    std::unordered_map<TileKey, std::unique_ptr<BTile>, TileKeyHash> B;
    TileKey last_key{-1, -1};
    BTile* last_tile = nullptr;

    std::vector<Sample> samples;
    IRect dirty;  // union of the dab rects placed since the last add_samples() return
    size_t call_dabs = 0;

    // walk-only tracing
    std::vector<detail::DabInfo>* trace = nullptr;

    BTile& btile(int tx, int ty) {
        const TileKey key{tx, ty};
        if (last_tile && key == last_key) return *last_tile;
        auto it = B.find(key);
        if (it == B.end()) it = B.emplace(key, std::make_unique<BTile>()).first;
        last_key = key;
        last_tile = it->second.get();
        return *last_tile;
    }

    const BTile* find_btile(int tx, int ty) const {
        auto it = B.find(TileKey{tx, ty});
        return it == B.end() ? nullptr : it->second.get();
    }

    double factor(const std::array<uint16_t, 256>& lut, double pr) const {
        int i = static_cast<int>(q(pr));  // = rhu(p * 255.0)
        if (mut::active(37)) i = std::max(i - 1, 0);
        return static_cast<double>(lut[static_cast<size_t>(i)]) / 65535.0;
    }

    double next_step(double d) const { return p.spacing * std::max(d, 1.0); }

    // ---- composite (doc 40 §3.8) ----------------------------------------------------------------
    // Pixel composite for Target::Pixels. `sp` is S0's pixel, `bv` > 0 the stroke buffer, `s` the
    // selection coverage, (x, y) the canvas pixel (for the clone source read).
    Rgba8 comp_rgba(Rgba8 sp, double bv, double s, int x, int y) const {
        const bool eraser_lock = lock && !mut::active(39);
        if (p.tool == Tool::Eraser) {
            if (eraser_lock) return sp;
            const double a_s = bv * s;
            if (a_s == 0.0) return sp;
            const double ad = dec(sp.a);
            const double a_o = ad * (1.0 - a_s);
            return canonicalize(Rgba8{sp.r, sp.g, sp.b, q(a_o)});
        }
        double a_s;
        double cs[3];
        if (p.tool == Tool::Clone) {
            const int64_t sx = static_cast<int64_t>(x) + oxi;
            const int64_t sy = static_cast<int64_t>(y) + oyi;
            Rgba8 c{};
            if (sx >= 0 && sy >= 0 && sx < W && sy < H) c = src.get(static_cast<int>(sx), static_cast<int>(sy));
            const double ac = dec(c.a);
            a_s = (bv * s) * ac;
            cs[0] = dec(c.r);
            cs[1] = dec(c.g);
            cs[2] = dec(c.b);
        } else {
            a_s = bv * s;
            cs[0] = Cs[0];
            cs[1] = Cs[1];
            cs[2] = Cs[2];
        }
        if (a_s == 0.0) return sp;
        if (lock && sp.a == 0) return sp;
        const double cd[3] = {dec(sp.r), dec(sp.g), dec(sp.b)};
        uint8_t o[3];
        if (lock) {
            for (int c = 0; c < 3; ++c) o[c] = q((cd[c] * (1.0 - a_s)) + (cs[c] * a_s));
            return Rgba8{o[0], o[1], o[2], sp.a};
        }
        const double ad = dec(sp.a);
        const double a_o = a_s + (ad * (1.0 - a_s));
        for (int c = 0; c < 3; ++c) o[c] = q(((cs[c] * a_s) + ((cd[c] * ad) * (1.0 - a_s))) / a_o);
        return canonicalize(Rgba8{o[0], o[1], o[2], q(a_o)});
    }

    Gray8 comp_mask(Gray8 m8, double bv, double s) const {
        const double a_s = bv * s;
        if (a_s == 0.0) return m8;
        const double Mv = dec(m8);
        return q(Mv + (a_s * (luma - Mv)));
    }

    // Composites the pixels of `r` (clipped to tile (tx, ty)) that have B > 0 into `out`.
    template <class Img, class Fn>
    void composite_tile(Img& out, const auto& s0img, int tx, int ty, const IRect& r, Fn&& fn) const {
        const BTile* bt = find_btile(tx, ty);
        if (!bt) return;
        const int x0 = std::max(r.x0, tx * kTileSize), x1 = std::min(r.x1, (tx + 1) * kTileSize);
        const int y0 = std::max(r.y0, ty * kTileSize), y1 = std::min(r.y1, (ty + 1) * kTileSize);
        if (x1 <= x0 || y1 <= y0) return;
        const auto& st = s0img.tile(tx, ty);
        const GrayTile* selt = sel_active ? &sel.tile(tx, ty) : nullptr;
        auto* ot = &out.mutable_tile(tx, ty);
        for (int y = y0; y < y1; ++y) {
            const int ly = y - (ty * kTileSize);
            for (int x = x0; x < x1; ++x) {
                const int lx = x - (tx * kTileSize);
                const size_t i = static_cast<size_t>((ly * kTileSize) + lx);
                const double bv = bt->v[i];
                if (!(bv > 0.0)) continue;
                const double s = selt ? dec(selt->px[i]) : 1.0;
                ot->px[i] = fn(st.px[i], bv, s, x, y);
            }
        }
    }

    IRect canvas_rect() const { return IRect{0, 0, W, H}; }

    // Builds composite(S0, B) over every buffer tile into a copy of S0.
    RgbaImage build_rgba() const {
        RgbaImage out = s0;
        const IRect all = canvas_rect();
        for (const auto& kv : B) {
            const TileKey k2 = kv.first;
            composite_tile(out, s0, k2.tx, k2.ty, all,
                           [this](Rgba8 sp, double bv, double s, int x, int y) { return comp_rgba(sp, bv, s, x, y); });
            // keep the layer sparse: an all-transparent result tile is dropped
            if (!out.is_absent(k2.tx, k2.ty)) {
                const RgbaTile& t = out.tile(k2.tx, k2.ty);
                bool zero = true;
                for (const Rgba8& px : t.px)
                    if (px.a != 0 || px.r != 0 || px.g != 0 || px.b != 0) {
                        zero = false;
                        break;
                    }
                if (zero) out.erase_tile(k2.tx, k2.ty);
            }
        }
        return out;
    }

    GrayImage build_mask() const {
        GrayImage out = s0m;
        const IRect all = canvas_rect();
        for (const auto& kv : B)
            composite_tile(out, s0m, kv.first.tx, kv.first.ty, all,
                           [this](Gray8 m8, double bv, double s, int, int) { return comp_mask(m8, bv, s); });
        return out;
    }

    // Writes composite(S0, B) into the live target (used by end() and mutation 10).
    void write_target(bool use_preview) {
        Node& n = doc->require_raster(layer_id, ctx);
        if (p.target == Target::Mask) {
            if (!n.mask) throw ScriptError(ctx + ": layer '" + layer_id + "' has no mask");
            n.mask->plane = use_preview ? prevm : build_mask();
        } else {
            n.pixels = use_preview ? prev : build_rgba();
        }
    }

    void update_preview(const IRect& r) {
        if (!opts.preview || r.empty() || !doc) return;
        const int tx0 = r.x0 / kTileSize, tx1 = (r.x1 - 1) / kTileSize;
        const int ty0 = r.y0 / kTileSize, ty1 = (r.y1 - 1) / kTileSize;
        for (int ty = ty0; ty <= ty1; ++ty)
            for (int tx = tx0; tx <= tx1; ++tx) {
                if (p.target == Target::Mask)
                    composite_tile(prevm, s0m, tx, ty, r,
                                   [this](Gray8 m8, double bv, double s, int, int) { return comp_mask(m8, bv, s); });
                else
                    composite_tile(prev, s0, tx, ty, r, [this](Rgba8 sp, double bv, double s, int x, int y) {
                        return comp_rgba(sp, bv, s, x, y);
                    });
            }
    }

    // ---- dabs (doc 40 §3.5-3.7) -----------------------------------------------------------------
    // Places one dab and returns its unclamped diameter d.
    double place_dab(double cx, double cy, double pr) {
        if (dab_count >= kMaxDabs)
            throw ScriptError(ctx + ": stroke exceeds 1000000 dabs (doc 40 §3.4)");
        // Mutation 10: the stroke memento is closed (committed as a history record) after every dab
        // and a new one opened; S0 is not re-taken.
        if (mut::active(10) && doc && dab_count >= 1) {
            write_target(false);
            doc->push_history();
        }
        ++dab_count;
        ++call_dabs;

        const double fs = has_size_lut ? factor(size_lut, pr) : 1.0;
        const double fo = has_op_lut ? factor(op_lut, pr) : 1.0;
        const double d = p.size * fs;
        if (trace) trace->push_back(detail::DabInfo{cx, cy, pr, d});
        if (d <= 0.0 || !doc) return d;

        const double k_small = std::min(d, 1.0);
        const double O = (p.opacity * fo) * k_small;
        const DabGeom g = make_geom(cx, cy, d, p.hardness, p.roundness, ca, sa);
        const double f = p.flow;
        const double kb = f * O;

        const int px0 = clamp_to_int(std::ceil((cx - g.R) - 0.5), 0, W);
        const int px1 = clamp_to_int(std::floor((cx + g.R) - 0.5), -1, W - 1);
        const int py0 = clamp_to_int(std::ceil((cy - g.R) - 0.5), 0, H);
        const int py1 = clamp_to_int(std::floor((cy + g.R) - 0.5), -1, H - 1);
        if (px1 < px0 || py1 < py0) return d;

        const bool m7 = mut::active(7);
        const bool m42 = mut::active(42);
        const bool wash = p.mode == Mode::Wash;
        for (int py = py0; py <= py1; ++py) {
            const int ty = py / kTileSize;
            const int ly = py - (ty * kTileSize);
            int px = px0;
            while (px <= px1) {
                const int tx = px / kTileSize;
                const int seg_end = std::min(px1, ((tx + 1) * kTileSize) - 1);
                BTile& bt = btile(tx, ty);
                double* row = &bt.v[static_cast<size_t>(ly * kTileSize)];
                for (; px <= seg_end; ++px) {
                    const double m = mask_at(g, px, py);
                    double& b = row[px - (tx * kTileSize)];
                    b = buffer_update(b, m, f, O, kb, wash, m7, m42);
                }
            }
        }
        const IRect r{px0, py0, px1 + 1, py1 + 1};
        if (dirty.empty()) {
            dirty = r;
        } else {
            dirty.x0 = std::min(dirty.x0, r.x0);
            dirty.y0 = std::min(dirty.y0, r.y0);
            dirty.x1 = std::max(dirty.x1, r.x1);
            dirty.y1 = std::max(dirty.y1, r.y1);
        }
        return d;
    }

    // ---- one sample (doc 40 §3.2 + §3.4) --------------------------------------------------------
    void add_one(const Sample& smp) {
        validate(smp, ctx + ": sample " + std::to_string(k));
        samples.push_back(smp);
        const double xr = smp.x, yr = smp.y;
        const double pk = clamp(smp.pressure, 0.0, 1.0);
        const double traw = smp.t_ms;
        if (k == 0) {
            if (p.tool == Tool::Clone && clone) resolve_clone_offset(xr, yr);
            xs_prev = xr;
            ys_prev = yr;
            p_prev = pk;
            te_prev = traw;
            const double d = place_dab(xr, yr, pk);
            step = next_step(d);
            frac = 0.0;
            ++k;
            return;
        }
        if (mut::active(8)) frac = 0.0;
        const double te = std::max(te_prev, traw);
        const double xs = (ema_a * xr) + (ema_m * xs_prev);
        const double ys = (ema_a * yr) + (ema_m * ys_prev);
        const double pc = mut::active(38) ? ((ema_a * pk) + (ema_m * p_prev)) : pk;
        const double dx = xs - xs_prev;
        const double dy = ys - ys_prev;
        double L = std::sqrt((dx * dx) + (dy * dy));
        if (mut::active(9)) L = L * p.view_zoom;
        const double D = (te - te_prev) / 1000.0;
        const double r_air = p.dabs_per_second;
        double u = 0.0;
        double todo = 0.0;
        for (;;) {
            const double w = 1.0 - u;
            todo = ((L * w) / step) + ((D * w) * r_air);
            if ((frac + todo) < 1.0) break;
            const double need = 1.0 - frac;
            u = u + (w * (need / todo));
            if (u > 1.0) u = 1.0;
            const double x = xs_prev + (u * dx);
            const double y = ys_prev + (u * dy);
            const double pp = p_prev + (u * (pc - p_prev));
            const double d = place_dab(x, y, pp);
            frac = 0.0;
            step = next_step(d);
        }
        frac = frac + todo;
        xs_prev = xs;
        ys_prev = ys;
        p_prev = pc;
        te_prev = te;
        ++k;
    }

    // doc 40 §4, with (x0, y0) = the first sample's raw position.
    void resolve_clone_offset(double x0, double y0) {
        if (p.aligned && clone->off) {
            // reuse
        } else {
            clone->off = std::array<int64_t, 2>{static_cast<int64_t>(rhu((*clone->src)[0] - x0)),
                                                static_cast<int64_t>(rhu((*clone->src)[1] - y0))};
        }
        oxi = (*clone->off)[0];
        oyi = (*clone->off)[1];
    }

    void setup_constants() {
        if (p.size_curve) {
            has_size_lut = true;
            size_lut = detail::bake_lut(*p.size_curve);
        }
        if (p.opacity_curve) {
            has_op_lut = true;
            op_lut = detail::bake_lut(*p.opacity_curve);
        }
        const double theta = p.angle * (kPi / 180.0);
        ca = std::cos(theta);
        sa = std::sin(theta);
        ema_m = p.smoothing;
        ema_a = 1.0 - ema_m;
        Cs[0] = dec(p.color.r);
        Cs[1] = dec(p.color.g);
        Cs[2] = dec(p.color.b);
        luma = ((0.30 * Cs[0]) + (0.59 * Cs[1])) + (0.11 * Cs[2]);
    }
};

// =================================================================================================

StrokeSession::StrokeSession() : d_(std::make_unique<Impl>()) {}
StrokeSession::~StrokeSession() = default;

void StrokeSession::begin(Document& doc, const std::string& layer_id, const StrokeParams& params,
                          const SessionOptions& opts, CloneState* clone, const std::string& context) {
    if (d_->active) throw ScriptError(context + ": a stroke is already in progress");
    validate(params, context);
    Node& n = doc.require_raster(layer_id, context);
    if (params.target == Target::Mask && !n.mask)
        throw ScriptError(context + ": layer '" + layer_id + "' has no mask (target \"mask\")");
    const Node* srcn = nullptr;
    if (params.tool == Tool::Clone) {
        if (!clone) throw ScriptError(context + ": clone stroke without clone state");
        const std::string sl = params.source_layer.empty() ? layer_id : params.source_layer;
        srcn = &doc.require_raster(sl, context);
        if (!params.source && !clone->src)
            throw ScriptError(context + ": clone_stroke has no 'source' and no earlier clone_stroke set one");
    }

    auto im = std::make_unique<Impl>();
    Impl& s = *im;
    s.doc = &doc;
    s.layer_id = layer_id;
    s.ctx = context;
    s.p = params;
    s.opts = opts;
    s.W = doc.width();
    s.H = doc.height();
    if (params.target == Target::Mask) {
        s.s0m = n.mask->plane;
        if (opts.preview) s.prevm = s.s0m;
    } else {
        s.s0 = n.pixels;
        if (opts.preview) s.prev = s.s0;
    }
    s.lock = n.lock_alpha && params.target == Target::Pixels;
    if (srcn) s.src = srcn->pixels;
    s.sel_active = doc.selection().active();
    if (s.sel_active) s.sel = doc.selection().mask;
    s.setup_constants();

    // All validation passed: now the side effects (clone tool state, history record).
    if (params.tool == Tool::Clone) {
        s.clone = clone;
        if (params.source) {
            clone->src = params.source;
            clone->off.reset();
        }
    }
    if (opts.push_history) doc.push_history();
    s.active = true;
    d_ = std::move(im);
}

StrokeDelta StrokeSession::add_samples(std::span<const Sample> samples) {
    Impl& s = *d_;
    if (!s.active) throw ScriptError("add_samples: no stroke in progress");
    s.dirty = IRect{};
    s.call_dabs = 0;
    try {
        for (const Sample& smp : samples) s.add_one(smp);
    } catch (...) {
        s.active = false;
        throw;
    }
    StrokeDelta out;
    out.rect = s.dirty;
    out.dabs = s.call_dabs;
    s.update_preview(s.dirty);
    if (!s.dirty.empty()) {
        for (int ty = s.dirty.y0 / kTileSize; ty <= (s.dirty.y1 - 1) / kTileSize; ++ty)
            for (int tx = s.dirty.x0 / kTileSize; tx <= (s.dirty.x1 - 1) / kTileSize; ++tx)
                out.tiles.push_back(TileKey{tx, ty});
    }
    return out;
}

const RgbaImage* StrokeSession::preview_pixels() const {
    return (d_->active && d_->opts.preview && d_->p.target == Target::Pixels) ? &d_->prev : nullptr;
}

const GrayImage* StrokeSession::preview_mask() const {
    return (d_->active && d_->opts.preview && d_->p.target == Target::Mask) ? &d_->prevm : nullptr;
}

nlohmann::json StrokeSession::end() {
    Impl& s = *d_;
    if (!s.active) throw ScriptError("end: no stroke in progress");
    if (s.samples.empty()) throw ScriptError(s.ctx + ": a stroke needs at least 1 sample");
    s.write_target(s.opts.preview);
    s.active = false;
    StrokeParams rp = s.p;
    return to_op(s.layer_id, rp, s.samples);
}

bool StrokeSession::active() const { return d_->active; }
size_t StrokeSession::dab_count() const { return d_->dab_count; }
size_t StrokeSession::sample_count() const { return d_->samples.size(); }
size_t StrokeSession::buffer_tiles() const { return d_->B.size(); }
const std::string& StrokeSession::layer_id() const { return d_->layer_id; }
Target StrokeSession::target() const { return d_->p.target; }

namespace detail {

std::vector<DabInfo> walk(const StrokeParams& p, const std::vector<Sample>& samples, double* final_frac) {
    StrokeSession::Impl s;
    s.p = p;
    s.ctx = "walk";
    s.setup_constants();
    std::vector<DabInfo> out;
    s.trace = &out;
    for (const Sample& smp : samples) s.add_one(smp);
    if (final_frac) *final_frac = s.frac;
    return out;
}

}  // namespace detail

}  // namespace rl::brush
