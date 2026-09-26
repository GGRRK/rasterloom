// SPDX-License-Identifier: GPL-3.0-or-later
//
// Render-script ops owned by docs/math/10-compositing.md §11.2, plus `undo` (00-conventions C10).
#include <algorithm>

#include "core/base/error.hpp"
#include "core/base/quant.hpp"
#include "core/composite/fills.hpp"
#include "core/composite/merge.hpp"
#include "core/script/domains.hpp"

namespace rl::script {

namespace {

using composite::LayerFill;
using composite::MaskFill;

Rect whole_canvas(const Document& d) { return Rect{0, 0, d.width(), d.height()}; }

void require_raster_or_adjustment(const Node& n, const std::string& ctx) {
    if (!(n.is_raster() || n.is_adjustment()))
        throw ScriptError(ctx + ": '" + n.id + "' is a " + node_kind_name(n.kind) +
                          "; this op applies to raster and adjustment layers only");
}

bool is_in_subtree(const Node& n, const std::string& id) {
    for (const Node& c : n.children) {
        if (c.id == id) return true;
        if (is_in_subtree(c, id)) return true;
    }
    return false;
}

// ---- add_layer ------------------------------------------------------------------------------------
void op_add_layer(OpContext& ctx, Fields& f) {
    Document& d = ctx.doc;
    const std::string id = f.req_string("id");
    const std::string parent = f.string_or("parent", "root");
    const std::string kind = f.enum_or("fill", "empty", {"empty", "solid", "gradient", "checker", "noise"});
    LayerFill fill;
    if (kind == "solid") {
        fill.kind = LayerFill::Kind::Solid;
        fill.color = f.color_or("color", Rgba8{0, 0, 0, 255});
    } else if (kind == "gradient") {
        fill.kind = LayerFill::Kind::Gradient;
        fill.from = f.color_or("from", Rgba8{0, 0, 0, 255});
        fill.to = f.color_or("to", Rgba8{255, 255, 255, 255});
        fill.vertical = f.enum_or("dir", "h", {"h", "v"}) == "v";
    } else if (kind == "checker") {
        fill.kind = LayerFill::Kind::Checker;
        fill.a = f.color_or("a", Rgba8{255, 255, 255, 255});
        fill.b = f.color_or("b", Rgba8{204, 204, 204, 255});
        fill.cell = f.int_or("cell", 8, 1, 4096);
    } else if (kind == "noise") {
        fill.kind = LayerFill::Kind::Noise;
        fill.seed = static_cast<uint64_t>(f.int_or("seed", 0, 0, kMaxSeedExclusive - 1));
        const Json* a = f.raw("alpha");
        if (a) {
            if (a->is_string()) {
                if (a->get<std::string>() != "random")
                    f.fail("field 'alpha' must be \"random\" or an integer 0..255");
                fill.alpha_random = true;
            } else {
                fill.alpha_random = false;
                fill.alpha = static_cast<uint8_t>(Fields::as_int(*a, ctx.label + ": field 'alpha'", 0, 255));
            }
        }
    }
    fill.rect = f.rect_or("rect", whole_canvas(d));
    f.finish();

    Node n = Node::make_raster(id, d.width(), d.height());
    n.pixels = composite::make_layer_pixels(d.width(), d.height(), fill);
    d.add_node_top(parent, std::move(n), ctx.label);
}

// ---- add_group ------------------------------------------------------------------------------------
BlendMode parse_group_mode(const std::string& s, const std::string& ctx) {
    if (s == "isolated") return BlendMode::Norm;
    auto m = parse_blend_mode(s);
    if (!m) throw ScriptError(ctx + ": unknown blend mode '" + s + "'");
    return *m;
}

BlendMode parse_layer_mode(const std::string& s, const std::string& ctx) {
    auto m = parse_blend_mode(s);
    if (!m) throw ScriptError(ctx + ": unknown blend mode '" + s + "'");
    if (*m == BlendMode::Pass) throw ScriptError(ctx + ": mode 'pass' is only valid for groups");
    return *m;
}

void op_add_group(OpContext& ctx, Fields& f) {
    const std::string id = f.req_string("id");
    const std::string parent = f.string_or("parent", "root");
    const BlendMode mode = parse_group_mode(f.string_or("mode", "pass"), ctx.label);
    f.finish();
    ctx.doc.add_node_top(parent, Node::make_group(id, mode), ctx.label);
}

// ---- move_layer -----------------------------------------------------------------------------------
void op_move_layer(OpContext& ctx, Fields& f) {
    Document& d = ctx.doc;
    const std::string id = f.req_string("id");
    NodeRef r = d.require(id, ctx.label);
    const std::string cur_parent = (r.parent == &d.root()) ? std::string("root") : r.parent->id;
    const std::string parent = f.string_or("parent", cur_parent);
    const std::optional<int64_t> index = f.opt_int("index", 0, INT64_MAX);
    f.finish();

    if (parent == id || is_in_subtree(*r.node, parent))
        throw ScriptError(ctx.label + ": cannot move '" + id + "' into itself or a descendant");
    d.require_container(parent, ctx.label);  // validates before the tree changes

    Node moving = std::move(*r.node);
    r.parent->children.erase(r.parent->children.begin() + static_cast<std::ptrdiff_t>(r.index));
    Node& target = d.require_container(parent, ctx.label);
    const size_t len = target.children.size();
    const size_t at = index ? static_cast<size_t>(*index) : len;
    if (index && *index > static_cast<int64_t>(len))
        throw ScriptError(ctx.label + ": index " + std::to_string(*index) + " out of range 0.." + std::to_string(len));
    target.children.insert(target.children.begin() + static_cast<std::ptrdiff_t>(at), std::move(moving));
}

// ---- property setters -------------------------------------------------------------------------------
void op_set_blend(OpContext& ctx, Fields& f) {
    const std::string layer = f.req_string("layer");
    const std::string mode = f.req_string("mode");
    const std::optional<int64_t> seed = f.opt_int("seed", 0, kMaxSeedExclusive - 1);
    f.finish();
    Node& n = *ctx.doc.require(layer, ctx.label).node;
    n.mode = n.is_group() ? parse_group_mode(mode, ctx.label) : parse_layer_mode(mode, ctx.label);
    if (seed) n.seed = static_cast<uint64_t>(*seed);
}

void op_set_opacity(OpContext& ctx, Fields& f) {
    const std::string layer = f.req_string("layer");
    const double v = f.req_double("value", 0.0, 1.0);
    f.finish();
    ctx.doc.require(layer, ctx.label).node->opacity = v;
}

void op_set_fill(OpContext& ctx, Fields& f) {
    const std::string layer = f.req_string("layer");
    const double v = f.req_double("value", 0.0, 1.0);
    f.finish();
    Node& n = *ctx.doc.require(layer, ctx.label).node;
    require_raster_or_adjustment(n, ctx.label);
    n.fill = v;
}

void op_set_visible(OpContext& ctx, Fields& f) {
    const std::string layer = f.req_string("layer");
    const bool v = f.req_bool("value");
    f.finish();
    ctx.doc.require(layer, ctx.label).node->visible = v;
}

void op_set_clip(OpContext& ctx, Fields& f) {
    const std::string layer = f.req_string("layer");
    const bool v = f.req_bool("value");
    f.finish();
    Node& n = *ctx.doc.require(layer, ctx.label).node;
    require_raster_or_adjustment(n, ctx.label);
    n.clip = v;
}

void op_set_clbl(OpContext& ctx, Fields& f) {
    const std::string layer = f.req_string("layer");
    const bool v = f.req_bool("value");
    f.finish();
    Node& n = *ctx.doc.require(layer, ctx.label).node;
    require_raster_or_adjustment(n, ctx.label);
    n.clbl = v;
}

// ---- masks ----------------------------------------------------------------------------------------
void op_add_mask(OpContext& ctx, Fields& f) {
    Document& d = ctx.doc;
    const std::string layer = f.req_string("layer");
    const std::string kind = f.enum_or("fill", "solid", {"solid", "gradient", "noise"});
    MaskFill m;
    if (kind == "solid") {
        m.kind = MaskFill::Kind::Solid;
        m.value = static_cast<uint8_t>(f.int_or("value", 255, 0, 255));
    } else if (kind == "gradient") {
        m.kind = MaskFill::Kind::Gradient;
        m.from = static_cast<uint8_t>(f.int_or("from", 0, 0, 255));
        m.to = static_cast<uint8_t>(f.int_or("to", 255, 0, 255));
        m.vertical = f.enum_or("dir", "h", {"h", "v"}) == "v";
    } else {
        m.kind = MaskFill::Kind::Noise;
        m.seed = static_cast<uint64_t>(f.int_or("seed", 0, 0, kMaxSeedExclusive - 1));
    }
    m.rect = f.rect_or("rect", whole_canvas(d));
    m.outside = static_cast<uint8_t>(f.int_or("outside", 0, 0, 255));
    f.finish();
    Node& n = *d.require(layer, ctx.label).node;
    n.mask = LayerMask{composite::make_mask_plane(d.width(), d.height(), m), true};
}

Node& require_masked(OpContext& ctx, const std::string& layer) {
    Node& n = *ctx.doc.require(layer, ctx.label).node;
    if (!n.mask) throw ScriptError(ctx.label + ": '" + layer + "' has no mask");
    return n;
}

void op_set_mask_enabled(OpContext& ctx, Fields& f) {
    const std::string layer = f.req_string("layer");
    const bool v = f.req_bool("value");
    f.finish();
    require_masked(ctx, layer).mask->enabled = v;
}

void op_delete_mask(OpContext& ctx, Fields& f) {
    const std::string layer = f.req_string("layer");
    f.finish();
    require_masked(ctx, layer).mask.reset();
}

void op_apply_mask(OpContext& ctx, Fields& f) {
    const std::string layer = f.req_string("layer");
    f.finish();
    Node& n = ctx.doc.require_raster(layer, ctx.label);
    composite::apply_mask(n, ctx.label);
}

void op_lock_transparency(OpContext& ctx, Fields& f) {
    const std::string layer = f.req_string("layer");
    const bool v = f.bool_or("value", true);
    f.finish();
    ctx.doc.require_raster(layer, ctx.label).lock_alpha = v;
}

// ---- merges ---------------------------------------------------------------------------------------
void op_merge_down(OpContext& ctx, Fields& f) {
    const std::string layer = f.req_string("layer");
    f.finish();
    composite::merge_down(ctx.doc, layer, ctx.label);
}

void op_merge_visible(OpContext& ctx, Fields& f) {
    const std::string id = f.string_or("id", "merged");
    f.finish();
    composite::merge_visible(ctx.doc, id, ctx.label);
}

void op_flatten(OpContext& ctx, Fields& f) {
    const std::string id = f.string_or("id", "flattened");
    f.finish();
    composite::flatten(ctx.doc, id, ctx.label);
}

// ---- history --------------------------------------------------------------------------------------
void op_undo(OpContext& ctx, Fields& f) {
    const int64_t steps = f.int_or("steps", 1, 1, static_cast<int64_t>(Document::kMaxHistoryDepth));
    f.finish();
    try {
        ctx.doc.undo(static_cast<size_t>(steps));
    } catch (const ScriptError& e) {
        throw ScriptError(ctx.label + ": " + e.what());
    }
}

}  // namespace

void registerCompositingOps(OpRegistry& r) {
    r.add("add_layer", op_add_layer);
    r.add("add_group", op_add_group);
    r.add("move_layer", op_move_layer);
    r.add("set_blend", op_set_blend);
    r.add("set_opacity", op_set_opacity);
    r.add("set_fill", op_set_fill);
    r.add("set_visible", op_set_visible);
    r.add("set_clip", op_set_clip);
    r.add("set_clbl", op_set_clbl);
    r.add("add_mask", op_add_mask);
    r.add("set_mask_enabled", op_set_mask_enabled);
    r.add("delete_mask", op_delete_mask);
    r.add("apply_mask", op_apply_mask);
    r.add("lock_transparency", op_lock_transparency);
    r.add("merge_down", op_merge_down);
    r.add("merge_visible", op_merge_visible);
    r.add("flatten", op_flatten);
    r.add("undo", op_undo, /*records_history=*/false);
}

}  // namespace rl::script
