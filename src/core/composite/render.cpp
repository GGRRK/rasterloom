// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/composite/render.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>

#include "core/base/mutation.hpp"
#include "core/base/quant.hpp"
#include "core/composite/composite.hpp"
#include "core/tile/memory.hpp"

namespace rl::composite {

namespace {

// Per-pixel extra coverage factor. Only ever non-null under mutation 19 (pass-through opacity/mask
// applied per child); the correct path never uses it.
using Extra = std::array<double, kTilePixels>;

inline double apply_extra(double c, const Extra* extra, int i) {
    return extra ? c * (*extra)[static_cast<size_t>(i)] : c;
}

template <class F>
inline void for_each_px(const TileCtx& t, F&& f) {
    for (int ly = 0; ly < t.ny; ++ly) {
        for (int lx = 0; lx < t.nx; ++lx) {
            f((ly * kTileSize) + lx, static_cast<uint32_t>(t.x0 + lx), static_cast<uint32_t>(t.y0 + ly));
        }
    }
}

inline bool effclip(const Node& n) { return n.clip && (n.is_raster() || n.is_adjustment()); }

// §4.3 coverage of a raster layer at tile index i.
inline double raster_coverage(const Node& n, const RgbaTile& px, const GrayTile* mt, int i) {
    const double m = mask_factor(mt, i);
    return ((dec(px.px[static_cast<size_t>(i)].a) * m) * n.fill) * n.opacity;
}

// §4.3 coverage of an adjustment layer at tile index i.
inline double adjustment_coverage(const Node& n, const GrayTile* mt, int i) {
    const double m = mask_factor(mt, i);
    return ((1.0 * m) * n.fill) * n.opacity;
}

void render_children_x(const std::vector<Node>& children, const TileCtx& t, RgbaTile& D, const Extra* extra);

// §7 GROUP(g, D).
void render_group(const Node& g, const TileCtx& t, RgbaTile& D, const Extra* extra) {
    const GrayTile* mt = enabled_mask_tile(g, t);
    const bool pass = (g.mode == BlendMode::Pass);

    if (pass && !mut::active(4)) {
        if (mut::active(19)) {
            // Mutation 19: each child's coverage is multiplied by w = mg * og instead of one lerp.
            Extra w{};
            for_each_px(t, [&](int i, uint32_t, uint32_t) {
                const double wi = mask_factor(mt, i) * g.opacity;
                w[static_cast<size_t>(i)] = extra ? (*extra)[static_cast<size_t>(i)] * wi : wi;
            });
            render_children_x(g.children, t, D, &w);
            return;
        }
        // §7.1 pass-through.
        const RgbaTile B0 = D;  // 1. the pre-group backdrop
        RgbaTile R = D;         // 2. children onto the running backdrop
        render_children_x(g.children, t, R, nullptr);
        // 3. one lerp in premultiplied space against the pre-group backdrop.
        for_each_px(t, [&](int i, uint32_t, uint32_t) {
            const size_t k = static_cast<size_t>(i);
            const double w = mask_factor(mt, i) * g.opacity;
            const Rgba8 b0 = B0.px[k];
            const Rgba8 r = R.px[k];
            const double a0 = dec(b0.a);
            const double ar = dec(r.a);
            const double ao = ((1.0 - w) * a0) + (w * ar);
            if (ao == 0.0) {
                D.px[k] = Rgba8{};
                return;
            }
            const double c0[3] = {dec(b0.r), dec(b0.g), dec(b0.b)};
            const double cr[3] = {dec(r.r), dec(r.g), dec(r.b)};
            uint8_t out[3];
            for (int ch = 0; ch < 3; ++ch) {
                const double p = (((1.0 - w) * a0) * c0[ch]) + ((w * ar) * cr[ch]);
                const double co = p / ao;
                out[ch] = q(co);
            }
            D.px[k] = canonicalize(Rgba8{out[0], out[1], out[2], q(ao)});
        });
        return;
    }

    // §7.2 isolated (mutation 4 routes pass-through groups here with mode Normal).
    const BlendMode mode = pass ? BlendMode::Norm : g.mode;
    RgbaTile T;
    T.px.fill(Rgba8{});  // 1. children onto a transparent buffer
    render_children_x(g.children, t, T, nullptr);
    for_each_px(t, [&](int i, uint32_t x, uint32_t y) {
        const size_t k = static_cast<size_t>(i);
        double c = (dec(T.px[k].a) * mask_factor(mt, i)) * g.opacity;  // groups have no fill
        c = apply_extra(c, extra, i);
        D.px[k] = composite_px(D.px[k], T.px[k], c, mode, g.seed, x, y);
    });
}

// §5 LONE(node, D).
void render_lone(const Node& n, const TileCtx& t, RgbaTile& D, const Extra* extra) {
    switch (n.kind) {
        case NodeKind::Raster: {
            // A tile that was never written is all (0,0,0,0): c == 0 everywhere, and skipping is
            // exact (§4.1 "Skipping is exact").
            if (n.pixels.is_absent(t.tx, t.ty)) return;
            const RgbaTile& px = n.pixels.tile(t.tx, t.ty);
            const GrayTile* mt = enabled_mask_tile(n, t);
            for_each_px(t, [&](int i, uint32_t x, uint32_t y) {
                const size_t k = static_cast<size_t>(i);
                const double c = apply_extra(raster_coverage(n, px, mt, i), extra, i);
                D.px[k] = composite_px(D.px[k], px.px[k], c, n.mode, n.seed, x, y);
            });
            return;
        }
        case NodeKind::Adjustment: {
            const GrayTile* mt = enabled_mask_tile(n, t);
            const adjust::Adjustment& adj = *n.adjustment;
            for_each_px(t, [&](int i, uint32_t x, uint32_t y) {
                const size_t k = static_cast<size_t>(i);
                const double c = apply_extra(adjustment_coverage(n, mt, i), extra, i);
                D.px[k] = adjust_px(D.px[k], adj, c, n.mode, n.seed, x, y);
            });
            return;
        }
        case NodeKind::Group:
            render_group(n, t, D, extra);
            return;
    }
}

// §6 CLIPGROUP(base, vis, D).
void render_clip_group(const Node& base, const std::vector<const Node*>& vis, const TileCtx& t, RgbaTile& D,
                       const Extra* extra) {
    const RgbaTile& base_px = base.pixels.tile(t.tx, t.ty);
    const GrayTile* base_mt = enabled_mask_tile(base, t);

    if (base.clbl || mut::active(20)) {
        // §6.1 clbl = true (mutation 20: clbl = false bases also come here).
        RgbaTile G;
        build_clip_interior(base, vis, t, G);
        const double o0 = base.opacity;
        const double f0 = base.fill;
        for_each_px(t, [&](int i, uint32_t x, uint32_t y) {
            const size_t k = static_cast<size_t>(i);
            const double S = clip_shape(base, base_px, base_mt, i);
            double cg = 0.0;
            if (mut::active(6))
                cg = (S * dec(G.px[k].a)) * (o0 * f0);  // mutation 6
            else
                cg = (S * dec(G.px[k].a)) * o0;
            cg = apply_extra(cg, extra, i);
            D.px[k] = composite_px(D.px[k], G.px[k], cg, base.mode, base.seed, x, y);
        });
        return;
    }

    // §6.2 clbl = false.
    render_lone(base, t, D, extra);
    for (const Node* li : vis) {
        const GrayTile* mt = enabled_mask_tile(*li, t);
        if (li->is_raster()) {
            if (li->pixels.is_absent(t.tx, t.ty)) continue;  // c == 0 everywhere: exact skip
            const RgbaTile& px = li->pixels.tile(t.tx, t.ty);
            for_each_px(t, [&](int i, uint32_t x, uint32_t y) {
                const size_t k = static_cast<size_t>(i);
                const double S = clip_shape(base, base_px, base_mt, i);
                const double ci = raster_coverage(*li, px, mt, i);
                const double cip = apply_extra((ci * S) * base.opacity, extra, i);
                D.px[k] = composite_px(D.px[k], px.px[k], cip, li->mode, li->seed, x, y);
            });
        } else {
            const adjust::Adjustment& adj = *li->adjustment;
            for_each_px(t, [&](int i, uint32_t x, uint32_t y) {
                const size_t k = static_cast<size_t>(i);
                const double S = clip_shape(base, base_px, base_mt, i);
                const double ci = adjustment_coverage(*li, mt, i);
                const double cip = apply_extra((ci * S) * base.opacity, extra, i);
                D.px[k] = adjust_px(D.px[k], adj, cip, li->mode, li->seed, x, y);
            });
        }
    }
}

// §5 RENDER(children, D).
void render_children_x(const std::vector<Node>& children, const TileCtx& t, RgbaTile& D, const Extra* extra) {
    size_t i = 0;
    std::vector<const Node*> vis;
    while (i < children.size()) {
        const Node& node = children[i];
        if (effclip(node)) {  // bottom of container, or its base is not a raster layer
            if (node.visible) render_lone(node, t, D, extra);
            i = i + 1;
            continue;
        }
        size_t j = i + 1;
        vis.clear();
        if (node.is_raster()) {
            while (j < children.size() && effclip(children[j])) {
                if (children[j].visible) vis.push_back(&children[j]);
                j = j + 1;
            }
        }
        if (!node.visible) {  // a hidden base hides its whole clip group
            i = j;
            continue;
        }
        if (!vis.empty())
            render_clip_group(node, vis, t, D, extra);
        else
            render_lone(node, t, D, extra);
        i = j;
    }
}

}  // namespace

TileCtx TileCtx::make(int tx, int ty, int canvas_w, int canvas_h) {
    TileCtx t;
    t.tx = tx;
    t.ty = ty;
    t.x0 = tx * kTileSize;
    t.y0 = ty * kTileSize;
    t.nx = std::min(kTileSize, canvas_w - t.x0);
    t.ny = std::min(kTileSize, canvas_h - t.y0);
    return t;
}

void fill_tile(RgbaTile& buf, const TileCtx& t, Rgba8 v) {
    buf.px.fill(Rgba8{});
    for_each_px(t, [&](int i, uint32_t, uint32_t) { buf.px[static_cast<size_t>(i)] = v; });
}

const GrayTile* enabled_mask_tile(const Node& n, const TileCtx& t) {
    if (!n.mask || !n.mask->enabled) return nullptr;
    return &n.mask->plane.tile(t.tx, t.ty);
}

double clip_shape(const Node& /*base*/, const RgbaTile& base_px, const GrayTile* base_mask, int i) {
    const double a0 = dec(base_px.px[static_cast<size_t>(i)].a);
    if (mut::active(23)) return a0;  // mutation 23: S = n(A0)
    return a0 * mask_factor(base_mask, i);
}

void build_clip_interior(const Node& base, const std::vector<const Node*>& vis, const TileCtx& t, RgbaTile& G) {
    const RgbaTile& base_px = base.pixels.tile(t.tx, t.ty);
    const GrayTile* base_mt = enabled_mask_tile(base, t);
    // 1. Seed: colour = base colour, alpha byte = q(fill) (mutation 6: q(1.0)).
    const uint8_t seed_a = mut::active(6) ? q(1.0) : q(base.fill);
    G.px.fill(Rgba8{});
    for_each_px(t, [&](int i, uint32_t, uint32_t) {
        const Rgba8 p = base_px.px[static_cast<size_t>(i)];
        G.px[static_cast<size_t>(i)] = canonicalize(Rgba8{p.r, p.g, p.b, seed_a});
    });
    // 2. Each visible clipped node, bottom to top, with its own coverage (NOT multiplied by S).
    for (const Node* li : vis) {
        const GrayTile* mt = enabled_mask_tile(*li, t);
        if (li->is_raster()) {
            const RgbaTile& px = li->pixels.tile(t.tx, t.ty);
            for_each_px(t, [&](int i, uint32_t x, uint32_t y) {
                const size_t k = static_cast<size_t>(i);
                double ci = raster_coverage(*li, px, mt, i);
                if (mut::active(5)) ci = ci * clip_shape(base, base_px, base_mt, i);  // mutation 5
                G.px[k] = composite_px(G.px[k], px.px[k], ci, li->mode, li->seed, x, y);
            });
        } else {
            const adjust::Adjustment& adj = *li->adjustment;
            for_each_px(t, [&](int i, uint32_t x, uint32_t y) {
                const size_t k = static_cast<size_t>(i);
                double ci = adjustment_coverage(*li, mt, i);
                if (mut::active(5)) ci = ci * clip_shape(base, base_px, base_mt, i);  // mutation 5
                G.px[k] = adjust_px(G.px[k], adj, ci, li->mode, li->seed, x, y);
            });
        }
    }
}

void render_children(const std::vector<Node>& children, const TileCtx& t, RgbaTile& D) {
    render_children_x(children, t, D, nullptr);
}

void render_tile_kernel(const DocState& s, int tx, int ty, bool onto_bg, RgbaTile& out) {
    const TileCtx t = TileCtx::make(tx, ty, s.w, s.h);
    fill_tile(out, t, onto_bg ? canonicalize(s.bg) : Rgba8{});
    render_children_x(s.root.children, t, out, nullptr);
}

// ---- scheduling ------------------------------------------------------------------------------------

void DeterministicScheduler::render(const DocState& s, const std::vector<TileKey>& keys, bool onto_bg,
                                    const TileSink& sink) {
    mem::ReadScope rs;  // no eviction while tiles are being read
    RgbaTile buf;
    for (const TileKey& k : keys) {
        render_tile_kernel(s, k.tx, k.ty, onto_bg, buf);
        sink(k.tx, k.ty, buf);
    }
}

namespace {

struct SchedulerSlot {
    std::mutex m;
    DeterministicScheduler deterministic;
    std::unique_ptr<ICompositeScheduler> installed;
    std::atomic<bool> forced{false};
};

SchedulerSlot& slot() {
    static SchedulerSlot* s = new SchedulerSlot();  // outlives every static destructor
    return *s;
}

}  // namespace

ICompositeScheduler& scheduler() {
    SchedulerSlot& s = slot();
    if (s.forced.load(std::memory_order_acquire)) return s.deterministic;
    std::lock_guard<std::mutex> lk(s.m);
    return s.installed ? *s.installed : static_cast<ICompositeScheduler&>(s.deterministic);
}

void set_scheduler(std::unique_ptr<ICompositeScheduler> sched) {
    SchedulerSlot& s = slot();
    std::lock_guard<std::mutex> lk(s.m);
    if (s.forced.load(std::memory_order_acquire)) return;
    s.installed = std::move(sched);
}

void force_deterministic() { slot().forced.store(true, std::memory_order_release); }
bool deterministic_forced() { return slot().forced.load(std::memory_order_acquire); }

void render_tiles(const DocState& s, std::vector<TileKey> keys, bool onto_bg, const TileSink& sink) {
    const int nx = (s.w + kTileSize - 1) / kTileSize, ny = (s.h + kTileSize - 1) / kTileSize;
    keys.erase(std::remove_if(keys.begin(), keys.end(),
                              [&](const TileKey& k) { return k.tx < 0 || k.ty < 0 || k.tx >= nx || k.ty >= ny; }),
               keys.end());
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    if (keys.empty()) return;
    scheduler().render(s, keys, onto_bg, sink);
}

void render_tile(const DocState& s, int tx, int ty, bool onto_bg, RgbaTile& out) {
    const int nx = (s.w + kTileSize - 1) / kTileSize, ny = (s.h + kTileSize - 1) / kTileSize;
    if (tx < 0 || ty < 0 || tx >= nx || ty >= ny) {  // outside the canvas: nothing to composite
        out.px.fill(Rgba8{});
        return;
    }
    scheduler().render(s, {TileKey{tx, ty}}, onto_bg, [&out](int, int, const RgbaTile& t) { out = t; });
}

RgbaImage render_image(const DocState& s, bool onto_bg) {
    RgbaImage img(s.w, s.h);
    std::vector<TileKey> keys;
    keys.reserve(img.total_tiles());
    for (int ty = 0; ty < img.tiles_y(); ++ty)
        for (int tx = 0; tx < img.tiles_x(); ++tx) keys.push_back(TileKey{tx, ty});
    scheduler().render(s, keys, onto_bg, [&img](int tx, int ty, const RgbaTile& t) { img.put_tile_sparse(tx, ty, t); });
    return img;
}

}  // namespace rl::composite
