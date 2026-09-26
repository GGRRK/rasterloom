// SPDX-License-Identifier: GPL-3.0-or-later
//
// Render-script ops owned by docs/math/60-editing-ops.md: delete_layer, set_name, duplicate_layer,
// set_adjustment, set_group_mode, select_alpha.
//
// Every handler reads and validates all of its fields, every id and every derived id BEFORE it
// changes the document (doc 60 §13), so a script error leaves the state untouched. The engine
// pushes the one history record per op (50 §0 ruling 4).
//
// Mutation hooks owned here: 40 (duplicate without parent goes to the top of the container),
// 41 (set_adjustment merges params into the old ones).
#include <cstdint>
#include <string>
#include <vector>

#include "core/adjust/adjustment.hpp"
#include "core/base/error.hpp"
#include "core/base/mutation.hpp"
#include "core/edit/names.hpp"
#include "core/geometry/dense.hpp"
#include "core/script/domains.hpp"
#include "core/select/selection_ops.hpp"

namespace rl::script {

namespace {

bool subtree_contains(const Node& n, const std::string& id) {
    for (const Node& c : n.children) {
        if (c.id == id || subtree_contains(c, id)) return true;
    }
    return false;
}

// ---- delete_layer (§2) ------------------------------------------------------------------------------
void op_delete_layer(OpContext& ctx, Fields& f) {
    const std::string layer = f.req_string("layer");
    f.finish();
    NodeRef r = ctx.doc.require(layer, ctx.label);
    auto& kids = r.parent->children;
    kids.erase(kids.begin() + static_cast<std::ptrdiff_t>(r.index));
}

// ---- set_name (§3) ----------------------------------------------------------------------------------
void op_set_name(OpContext& ctx, Fields& f) {
    const std::string layer = f.req_string("layer");
    const std::string name = f.req_string("name");
    f.finish();
    Node& n = *ctx.doc.require(layer, ctx.label).node;
    const std::string problem = edit::name_problem(name);
    if (!problem.empty()) throw ScriptError(ctx.label + ": field 'name' " + problem);
    n.name = name;
}

// ---- duplicate_layer (§4) ---------------------------------------------------------------------------
// Rewrites the descendants of a fresh copy: names by §4.4 (computed from the ORIGINAL id and name,
// before the id changes), ids by §4.3 (top id + "/" + the original descendant's own id).
void retarget_descendants(Node& copy, const std::string& top_id) {
    for (Node& c : copy.children) {
        const std::string& shown = edit::display_name(c);
        c.name = edit::is_valid_name(shown) ? shown : std::string();
        c.id = top_id + "/" + c.id;
        retarget_descendants(c, top_id);
    }
}

void collect_descendant_ids(const Node& n, std::vector<std::string>& out) {
    for (const Node& c : n.children) {
        out.push_back(c.id);
        collect_descendant_ids(c, out);
    }
}

void op_duplicate_layer(OpContext& ctx, Fields& f) {
    Document& d = ctx.doc;
    const std::string layer = f.req_string("layer");
    const std::string new_id = f.req_string("id");
    const bool has_parent = f.has("parent");
    const std::string parent = has_parent ? f.req_string("parent") : std::string();
    f.finish();

    NodeRef src = d.require(layer, ctx.label);

    // Every new id is checked against the ids that exist now, before anything is inserted.
    d.check_new_id(new_id, ctx.label);
    std::vector<std::string> originals;
    collect_descendant_ids(*src.node, originals);
    for (const std::string& o : originals) {
        const std::string derived = new_id + "/" + o;
        if (d.id_exists(derived))
            throw ScriptError(ctx.label + ": derived id '" + derived + "' (copy of '" + o + "') is already used");
    }
    if (has_parent) {
        if (parent == layer || subtree_contains(*src.node, parent))
            throw ScriptError(ctx.label + ": cannot duplicate '" + layer + "' into itself or a descendant");
        d.require_container(parent, ctx.label);
    }

    Node copy = *src.node;  // value copy: tiles shared copy-on-write, independent afterwards
    const std::string top_name = edit::display_name(*src.node) + " copy";
    copy.id = new_id;
    copy.name = edit::is_valid_name(top_name) ? top_name : std::string();
    retarget_descendants(copy, new_id);

    if (has_parent) {
        Node& target = d.require_container(parent, ctx.label);
        target.children.push_back(std::move(copy));
        return;
    }
    auto& kids = src.parent->children;
    const size_t at = mut::active(40) ? kids.size() : src.index + 1;
    kids.insert(kids.begin() + static_cast<std::ptrdiff_t>(at), std::move(copy));
}

// ---- set_adjustment (§5) ----------------------------------------------------------------------------
void op_set_adjustment(OpContext& ctx, Fields& f) {
    const std::string layer = f.req_string("layer");
    const Json& params = f.raw_req("params");
    f.finish();
    Node& n = *ctx.doc.require(layer, ctx.label).node;
    if (!n.is_adjustment() || !n.adjustment)
        throw ScriptError(ctx.label + ": '" + layer + "' is a " + node_kind_name(n.kind) +
                          ", not an adjustment layer");
    if (!params.is_object()) throw ScriptError(ctx.label + ": field 'params' must be an object");

    const std::string type = n.adjustment->type();
    Json effective = params;
    if (mut::active(41)) {
        // Defect: keys omitted from `params` keep their previous values (deep merge).
        effective = adjust::params_to_json(n.adjustment->params());
        effective.merge_patch(params);
    }
    auto typed = adjust::parse_adjustment_params(type, &effective, ctx.label);
    auto adj = adjust::make_adjustment(typed, ctx.label);
    n.adjustment = std::move(adj);
    n.adjust_params = effective.dump();  // kept so file formats can save the params
}

// ---- set_group_mode (§6) ----------------------------------------------------------------------------
void op_set_group_mode(OpContext& ctx, Fields& f) {
    const std::string layer = f.req_string("layer");
    const std::string mode = f.req_enum("mode", {"pass", "isolated"});
    f.finish();
    Node& n = *ctx.doc.require(layer, ctx.label).node;
    if (!n.is_group())
        throw ScriptError(ctx.label + ": '" + layer + "' is a " + node_kind_name(n.kind) + ", not a group");
    if (mode == "pass") {
        n.mode = BlendMode::Pass;
    } else if (n.mode == BlendMode::Pass) {
        n.mode = BlendMode::Norm;  // "isolated" = norm (doc 10); an isolated group keeps its blend
    }
}

// ---- select_alpha (§7) ------------------------------------------------------------------------------
void op_select_alpha(OpContext& ctx, Fields& f) {
    Document& d = ctx.doc;
    const std::string layer = f.req_string("layer");
    const std::string mode_s = f.enum_or("mode", "new", {"new", "add", "subtract", "intersect"});
    f.finish();
    const Node& n = d.require_raster(layer, ctx.label);
    select::Mode mode = select::Mode::New;
    select::parse_mode(mode_s, mode);

    // B = the layer's own stored alpha byte; mask, visibility, opacity, fill, mode, clipping and
    // lock are all ignored.
    const int W = d.width(), H = d.height();
    const mem::Reservation hold =
        geom::reserve_dense(geom::dense_bytes(W, H, 1 + sizeof(Rgba8)), "select_alpha planes");
    std::vector<uint8_t> B(static_cast<size_t>(W) * static_cast<size_t>(H), 0);
    if (n.pixels.width() == W && n.pixels.height() == H) {
        const std::vector<Rgba8> px = geom::to_dense(n.pixels);
        for (size_t i = 0; i < B.size(); ++i) B[i] = px[i].a;
    } else {
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) B[geom::idx(x, y, W)] = n.pixels.get(x, y).a;
    }
    select::combine(d.selection(), B, mode);  // Saved is untouched
}

}  // namespace

void registerEditingOps(OpRegistry& r) {
    r.add("delete_layer", op_delete_layer);
    r.add("set_name", op_set_name);
    r.add("duplicate_layer", op_duplicate_layer);
    r.add("set_adjustment", op_set_adjustment);
    r.add("set_group_mode", op_set_group_mode);
    r.add("select_alpha", op_select_alpha);
}

}  // namespace rl::script
