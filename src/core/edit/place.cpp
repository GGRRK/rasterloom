// SPDX-License-Identifier: GPL-3.0-or-later
//
// docs/math/60-editing-ops.md §14: place_image, layer_via_copy, clear.
//
// Mutation hooks owned here: 43 (default centring truncates toward zero), 44 (COPY treats partial
// coverage as full), 45 (clear ignores the selection).
#include "core/edit/place.hpp"

#include <functional>
#include <stdexcept>

#include "core/base/error.hpp"
#include "core/base/mutation.hpp"
#include "core/base/quant.hpp"
#include "core/composite/render.hpp"
#include "core/edit/names.hpp"
#include "core/edit/png_payload.hpp"
#include "core/filters/filters.hpp"
#include "core/geometry/dense.hpp"
#include "core/select/selection_ops.hpp"

namespace rl::edit {

namespace {

// E(x, y) of doc 30 §2 as a dense plane; empty when there is no selection (255 everywhere).
std::vector<uint8_t> effective_plane(const Selection& sel) {
    if (!sel.active()) return {};
    return select::mask_dense(sel);
}

struct Placement {
    bool has_above = false;
    bool has_parent = false;
    std::string above;
    std::string parent;
};

Placement read_placement(script::Fields& f) {
    Placement p;
    p.has_above = f.has("above");
    p.has_parent = f.has("parent");
    if (p.has_above) p.above = f.req_string("above");
    if (p.has_parent) p.parent = f.req_string("parent");
    if (p.has_above && p.has_parent) f.fail("'above' and 'parent' cannot both be given");
    return p;
}

// Validates the placement target (before the document changes).
void check_placement(Document& d, const Placement& p, const std::string& ctx) {
    if (p.has_above) (void)d.require(p.above, ctx);  // any node kind; "root" and unknown ids throw
    if (p.has_parent) (void)d.require_container(p.parent, ctx);
}

// §14.3.1. `default_above` names the node to go above when no field is given ("" = top of root).
void insert_placed(Document& d, const Placement& p, const std::string& default_above, Node n, const std::string& ctx) {
    std::string above = p.has_above ? p.above : (p.has_parent ? std::string() : default_above);
    if (!above.empty()) {
        NodeRef r = d.require(above, ctx);
        auto& kids = r.parent->children;
        kids.insert(kids.begin() + static_cast<std::ptrdiff_t>(r.index + 1), std::move(n));
        return;
    }
    Node& target = d.require_container(p.has_parent ? p.parent : std::string("root"), ctx);
    target.children.push_back(std::move(n));
}

std::string read_name(script::Fields& f, const std::string& ctx) {
    const std::string name = f.string_or("name", "");
    const std::string problem = name_problem(name);
    if (!problem.empty()) throw ScriptError(ctx + ": field 'name' " + problem);
    return name;
}

// ---- place_image (§14.3) ---------------------------------------------------------------------------
void op_place_image(script::OpContext& ctx, script::Fields& f) {
    Document& d = ctx.doc;
    const std::string id = f.req_string("id");
    const std::string png = f.req_string("png");
    const bool has_x = f.has("x"), has_y = f.has("y");
    if (has_x != has_y) f.fail("'x' and 'y' must be given together");
    const int64_t x_in = has_x ? f.req_int("x", -32768, 32768) : 0;
    const int64_t y_in = has_y ? f.req_int("y", -32768, 32768) : 0;
    const std::string name = read_name(f, ctx.label);
    const Placement pl = read_placement(f);
    f.finish();
    d.check_new_id(id, ctx.label);
    check_placement(d, pl, ctx.label);

    io::RgbaBuffer img;
    try {
        img = decode_payload(png);
    } catch (const ScriptError&) {
        throw;
    } catch (const std::exception& e) {
        throw ScriptError(ctx.label + ": field 'png': " + e.what());
    }

    const int W = d.width(), H = d.height();
    int64_t x = x_in, y = y_in;
    if (!has_x) {
        if (mut::active(43)) {  // defect: C division truncates toward zero
            x = (static_cast<int64_t>(W) - img.w) / 2;
            y = (static_cast<int64_t>(H) - img.h) / 2;
        } else {
            x = floor_half(static_cast<int64_t>(W) - img.w);
            y = floor_half(static_cast<int64_t>(H) - img.h);
        }
    }

    Node n = Node::make_raster(id, W, H);
    n.name = name;
    const mem::Reservation hold = geom::reserve_dense(geom::dense_bytes(W, H, sizeof(Rgba8)), "place_image layer");
    std::vector<Rgba8> px(static_cast<size_t>(W) * static_cast<size_t>(H), Rgba8{});
    const int64_t cx0 = std::max<int64_t>(x, 0), cy0 = std::max<int64_t>(y, 0);
    const int64_t cx1 = std::min<int64_t>(x + img.w, W), cy1 = std::min<int64_t>(y + img.h, H);
    for (int64_t cy = cy0; cy < cy1; ++cy)
        for (int64_t cx = cx0; cx < cx1; ++cx)
            px[geom::idx(static_cast<int>(cx), static_cast<int>(cy), W)] =
                canonicalize(img.at(static_cast<int>(cx - x), static_cast<int>(cy - y)));
    n.pixels = geom::from_dense<Rgba8>(px, W, H);
    insert_placed(d, pl, std::string(), std::move(n), ctx.label);
}

// ---- layer_via_copy (§14.5) ------------------------------------------------------------------------
void op_layer_via_copy(script::OpContext& ctx, script::Fields& f) {
    Document& d = ctx.doc;
    const std::string id = f.req_string("id");
    const bool merged = f.bool_or("merged", false);
    const bool cut = f.bool_or("cut", false);
    const bool has_layer = f.has("layer");
    const std::string layer = has_layer ? f.req_string("layer") : std::string();
    const std::string name = read_name(f, ctx.label);
    const Placement pl = read_placement(f);
    f.finish();
    if (merged && has_layer) throw ScriptError(ctx.label + ": 'layer' must be absent when 'merged' is true");
    if (!merged && !has_layer) throw ScriptError(ctx.label + ": missing required field 'layer'");
    if (merged && cut) throw ScriptError(ctx.label + ": 'cut' cannot be used with 'merged' (nothing to cut from a composite)");
    d.check_new_id(id, ctx.label);
    const Node* src = merged ? nullptr : &d.require_raster(layer, ctx.label);
    check_placement(d, pl, ctx.label);

    const int W = d.width(), H = d.height();
    const mem::Reservation hold = geom::reserve_dense(geom::dense_bytes(W, H, sizeof(Rgba8) * 2), "layer_via_copy planes");
    std::vector<Rgba8> X = copy_pixels(d.state(), src);
    bool any = false;
    for (const Rgba8& p : X)
        if (p.a != 0) {
            any = true;
            break;
        }
    if (!any) throw ScriptError(ctx.label + ": the selected area is empty (no pixels to copy)");

    Node c = Node::make_raster(id, W, H);
    c.name = name;
    c.pixels = geom::from_dense<Rgba8>(X, W, H);
    X.clear();
    X.shrink_to_fit();
    insert_placed(d, pl, merged ? std::string() : layer, std::move(c), ctx.label);
    if (cut) clear_pixels(d.require_raster(layer, ctx.label), d.selection());
}

// ---- clear (§14.6) ---------------------------------------------------------------------------------
void op_clear(script::OpContext& ctx, script::Fields& f) {
    const std::string layer = f.req_string("layer");
    f.finish();
    Node& n = ctx.doc.require_raster(layer, ctx.label);
    clear_pixels(n, ctx.doc.selection());
}

}  // namespace

int64_t floor_half(int64_t n) { return n >= 0 ? n / 2 : -((-n + 1) / 2); }

std::vector<Rgba8> copy_pixels(const DocState& s, const Node* layer) {
    std::vector<Rgba8> src = layer ? geom::to_dense(layer->pixels) : geom::to_dense(composite::render_image(s, true));
    if (!s.selection.active()) return src;  // E == 255 everywhere: bytes unchanged
    const std::vector<uint8_t> E = select::mask_dense(s.selection);
    const bool m44 = mut::active(44);
    for (size_t i = 0; i < src.size(); ++i) {
        const uint8_t e = E[i];
        Rgba8& p = src[i];
        if (e == 255) continue;
        if (e == 0) {
            p = Rgba8{};
            continue;
        }
        if (m44) continue;  // defect: partial coverage copied as full
        p.a = q((static_cast<double>(p.a) / 255.0) * (static_cast<double>(e) / 255.0));
        p = canonicalize(p);
    }
    return src;
}

std::optional<Copied> crop_copied(const std::vector<Rgba8>& X, int W, int H) {
    int x0 = W, y0 = H, x1 = -1, y1 = -1;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            if (X[geom::idx(x, y, W)].a != 0) {
                x0 = std::min(x0, x);
                x1 = std::max(x1, x);
                y0 = std::min(y0, y);
                y1 = std::max(y1, y);
            }
    if (x1 < 0) return std::nullopt;
    Copied c;
    c.x = x0;
    c.y = y0;
    c.pixels.w = x1 - x0 + 1;
    c.pixels.h = y1 - y0 + 1;
    c.pixels.px.resize(static_cast<size_t>(c.pixels.w) * static_cast<size_t>(c.pixels.h));
    for (int y = 0; y < c.pixels.h; ++y)
        for (int x = 0; x < c.pixels.w; ++x) c.pixels.at(x, y) = X[geom::idx(x0 + x, y0 + y, W)];
    return c;
}

std::optional<Copied> copy_region(const DocState& s, const std::string& layer_id) {
    const Node* src = nullptr;
    if (!layer_id.empty()) {
        std::function<const Node*(const Node&)> find = [&](const Node& c) -> const Node* {
            for (const Node& k : c.children) {
                if (k.id == layer_id) return &k;
                if (k.is_group())
                    if (const Node* r = find(k)) return r;
            }
            return nullptr;
        };
        src = find(s.root);
        if (!src || !src->is_raster()) throw ScriptError("copy: '" + layer_id + "' is not a raster layer");
    }
    return crop_copied(copy_pixels(s, src), s.w, s.h);
}

void clear_pixels(Node& layer, const Selection& sel) {
    if (layer.lock_alpha) return;  // as the eraser on a locked layer (C8a)
    const filters::Image O = filters::to_image(layer.pixels);
    filters::Image F(O.w, O.h);  // every pixel (0, 0, 0, 0)
    const std::vector<uint8_t> M = mut::active(45) ? std::vector<uint8_t>{} : effective_plane(sel);
    filters::store_image(layer.pixels, filters::finish_filter(O, std::move(F), false, M));
}

void register_place_ops(script::OpRegistry& r) {
    r.add("place_image", op_place_image);
    r.add("layer_via_copy", op_layer_via_copy);
    r.add("clear", op_clear);
}

}  // namespace rl::edit
