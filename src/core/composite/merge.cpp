// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/composite/merge.hpp"

#include "core/base/error.hpp"
#include "core/base/quant.hpp"
#include "core/composite/composite.hpp"
#include "core/composite/render.hpp"

namespace rl::composite {

namespace {

template <class F>
void for_each_px(const TileCtx& t, F&& f) {
    for (int ly = 0; ly < t.ny; ++ly)
        for (int lx = 0; lx < t.nx; ++lx)
            f((ly * kTileSize) + lx, static_cast<uint32_t>(t.x0 + lx), static_cast<uint32_t>(t.y0 + ly));
}

// Properties of a layer created by merge_visible / flatten (§9.2 step 3).
Node new_merged_layer(const std::string& id, RgbaImage pixels) {
    Node n = Node::make_raster(id, pixels.width(), pixels.height());
    n.pixels = std::move(pixels);
    return n;
}

}  // namespace

void apply_mask(Node& n, const std::string& context) {
    if (!n.is_raster()) throw ScriptError(context + ": '" + n.id + "' is not a raster layer");
    if (!n.mask) throw ScriptError(context + ": '" + n.id + "' has no mask");
    const GrayImage& m = n.mask->plane;
    RgbaImage& px = n.pixels;
    for (const TileKey& k : px.sorted_keys()) {  // absent tiles have A = 0 and stay (0,0,0,0)
        const TileCtx t = TileCtx::make(k.tx, k.ty, px.width(), px.height());
        RgbaTile out = px.tile(k.tx, k.ty);
        const GrayTile& mt = m.tile(k.tx, k.ty);
        for_each_px(t, [&](int i, uint32_t, uint32_t) {
            const size_t j = static_cast<size_t>(i);
            Rgba8 p = out.px[j];
            p.a = q(dec(p.a) * dec(mt.px[j]));
            out.px[j] = canonicalize(p);
        });
        px.put_tile_sparse(k.tx, k.ty, out);
    }
    n.mask.reset();
}

void merge_down(Document& doc, const std::string& upper_id, const std::string& context) {
    NodeRef ur = doc.require(upper_id, context);
    Node& U = *ur.node;
    std::vector<Node>& siblings = ur.parent->children;
    if (!(U.is_raster() || U.is_adjustment()))
        throw ScriptError(context + ": '" + upper_id + "' is a " + node_kind_name(U.kind) +
                          "; merge_down needs a raster or adjustment layer");
    if (ur.index == 0) throw ScriptError(context + ": '" + upper_id + "' is at the bottom of its container");
    Node& L = siblings[ur.index - 1];
    if (!L.is_raster())
        throw ScriptError(context + ": the node below '" + upper_id + "' ('" + L.id + "') is a " +
                          node_kind_name(L.kind) + ", not a raster layer");
    if (!U.visible || !L.visible) throw ScriptError(context + ": both layers must be visible");
    if (ur.index + 1 < siblings.size() && siblings[ur.index + 1].clip)
        throw ScriptError(context + ": the node above '" + upper_id + "' is clipped; merging would change its base");

    const int W = doc.width(), H = doc.height();
    RgbaImage result(W, H);
    const bool clip_path = U.clip && !L.clip;
    const std::vector<const Node*> vis{&U};
    RgbaTile Lp, out, G;
    for (int ty = 0; ty < result.tiles_y(); ++ty) {
        for (int tx = 0; tx < result.tiles_x(); ++tx) {
            // L absent and U contributing nothing on transparency: the result is exactly
            // (0,0,0,0) on every path (ao == 0, Ab == 0, or S == 0).
            if (L.pixels.is_absent(tx, ty) && (U.is_adjustment() || U.pixels.is_absent(tx, ty))) continue;
            const TileCtx t = TileCtx::make(tx, ty, W, H);
            const RgbaTile& lpx = L.pixels.tile(tx, ty);
            const GrayTile* lmt = enabled_mask_tile(L, t);
            out.px.fill(Rgba8{});
            if (clip_path) {
                build_clip_interior(L, vis, t, G);
                for_each_px(t, [&](int i, uint32_t, uint32_t) {
                    const size_t k = static_cast<size_t>(i);
                    const double S = clip_shape(L, lpx, lmt, i);
                    const Rgba8 g = G.px[k];
                    out.px[k] = canonicalize(Rgba8{g.r, g.g, g.b, q(S * dec(g.a))});
                });
            } else {
                // 1. L' = L with its mask and fill baked into alpha.
                Lp.px.fill(Rgba8{});
                for_each_px(t, [&](int i, uint32_t, uint32_t) {
                    const size_t k = static_cast<size_t>(i);
                    Rgba8 p = lpx.px[k];
                    p.a = q((dec(p.a) * mask_factor(lmt, i)) * L.fill);
                    Lp.px[k] = canonicalize(p);
                });
                const GrayTile* umt = enabled_mask_tile(U, t);
                if (U.is_raster()) {
                    const RgbaTile& upx = U.pixels.tile(tx, ty);
                    for_each_px(t, [&](int i, uint32_t x, uint32_t y) {
                        const size_t k = static_cast<size_t>(i);
                        const double cu = ((dec(upx.px[k].a) * mask_factor(umt, i)) * U.fill) * U.opacity;
                        out.px[k] = composite_px(Lp.px[k], upx.px[k], cu, U.mode, U.seed, x, y);
                    });
                } else {
                    for_each_px(t, [&](int i, uint32_t x, uint32_t y) {
                        const size_t k = static_cast<size_t>(i);
                        const double cu = ((1.0 * mask_factor(umt, i)) * U.fill) * U.opacity;
                        out.px[k] = adjust_px(Lp.px[k], *U.adjustment, cu, U.mode, U.seed, x, y);
                    });
                }
            }
            result.put_tile_sparse(tx, ty, out);
        }
    }
    // 3. L keeps id, index, opacity, mode, seed, visible, clip, clbl, lock_alpha; fill 1; no mask.
    L.pixels = std::move(result);
    L.fill = 1.0;
    L.mask.reset();
    siblings.erase(siblings.begin() + static_cast<std::ptrdiff_t>(ur.index));
}

void merge_visible(Document& doc, const std::string& id, const std::string& context) {
    std::vector<Node>& top = doc.root().children;
    size_t first_visible = top.size();
    for (size_t i = 0; i < top.size(); ++i)
        if (top[i].visible) {
            first_visible = i;
            break;
        }
    if (first_visible == top.size()) return;  // nothing visible: no-op

    RgbaImage P = render_image(doc.state(), /*onto_bg=*/false);  // 1. NOT onto bg

    // 3. k = hidden top-level nodes below the bottommost removed node.
    size_t k = first_visible;  // every node below the first visible one is hidden
    std::vector<Node> kept;    // 2 + 4. hidden top-level nodes stay, in order
    for (Node& n : top)
        if (!n.visible) kept.push_back(std::move(n));
    top = std::move(kept);

    doc.check_new_id(id, context);  // duplicate check after step 2
    top.insert(top.begin() + static_cast<std::ptrdiff_t>(k), new_merged_layer(id, std::move(P)));
}

void flatten(Document& doc, const std::string& id, const std::string& context) {
    RgbaImage P = render_image(doc.state(), /*onto_bg=*/true);  // 1. full render, bg included
    doc.root().children.clear();                                // 2.
    doc.check_new_id(id, context);
    doc.root().children.push_back(new_merged_layer(id, std::move(P)));  // 3.
    doc.set_bg(Rgba8{});                                                // 4.
}

}  // namespace rl::composite
