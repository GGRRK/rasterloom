// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/doc/document.hpp"

#include "core/base/error.hpp"
#include "core/base/quant.hpp"

namespace rl {

// ---- History ------------------------------------------------------------------------------------

void History::set_depth(size_t depth) {
    if (depth < 1 || depth > Document::kMaxHistoryDepth)
        throw ScriptError("history depth must be in 1..1000");
    depth_ = depth;
    while (records_.size() > depth_) records_.pop_front();
}

void History::push(const DocState& before) {
    records_.push_back(before);
    while (records_.size() > depth_) records_.pop_front();
}

DocState History::pop(size_t n) {
    if (n == 0 || n > records_.size())
        throw ScriptError("undo of " + std::to_string(n) + " step(s) exceeds the " +
                          std::to_string(records_.size()) + " history record(s) available");
    for (size_t i = 1; i < n; ++i) records_.pop_back();
    DocState s = std::move(records_.back());
    records_.pop_back();
    return s;
}

// ---- Document -----------------------------------------------------------------------------------

Document::Document(int w, int h, Rgba8 bg, size_t history_depth) : h_(history_depth) {
    s_.w = w;
    s_.h = h;
    s_.bg = canonicalize(bg);
    s_.root = Node::make_group("root", BlendMode::Pass);
    s_.selection = Selection(w, h);
    if (history_depth < 1 || history_depth > kMaxHistoryDepth)
        throw ScriptError("history depth must be in 1..1000");
}

void Document::set_bg(Rgba8 bg) { s_.bg = canonicalize(bg); }

namespace {

bool find_in(Node& container, const std::string& id, NodeRef& out) {
    for (size_t i = 0; i < container.children.size(); ++i) {
        Node& c = container.children[i];
        if (c.id == id) {
            out = NodeRef{&c, &container, i};
            return true;
        }
        if (c.is_group() && find_in(c, id, out)) return true;
    }
    return false;
}

bool exists_in(const Node& container, const std::string& id) {
    for (const Node& c : container.children) {
        if (c.id == id) return true;
        if (c.is_group() && exists_in(c, id)) return true;
    }
    return false;
}

void collect_stats(const Node& n, std::vector<Document::TileStats>& out) {
    if (n.is_raster())
        out.push_back({n.id, "pixels", n.pixels.allocated_tiles(), n.pixels.total_tiles()});
    if (n.mask) out.push_back({n.id, "mask", n.mask->plane.allocated_tiles(), n.mask->plane.total_tiles()});
    for (const Node& c : n.children) collect_stats(c, out);
}

}  // namespace

NodeRef Document::find(const std::string& id) {
    NodeRef r;
    if (id == "root") return r;
    find_in(s_.root, id, r);
    return r;
}

bool Document::id_exists(const std::string& id) const {
    return exists_in(s_.root, id);
}

NodeRef Document::require(const std::string& id, const std::string& context) {
    if (id == "root") throw ScriptError(context + ": 'root' names the root container, not a layer");
    NodeRef r = find(id);
    if (!r.node) throw ScriptError(context + ": unknown id '" + id + "'");
    return r;
}

Node& Document::require_raster(const std::string& id, const std::string& context) {
    Node& n = *require(id, context).node;
    if (!n.is_raster())
        throw ScriptError(context + ": '" + id + "' is a " + node_kind_name(n.kind) + ", not a raster layer");
    return n;
}

Node& Document::require_container(const std::string& id, const std::string& context) {
    if (id == "root") return s_.root;
    NodeRef r = find(id);
    if (!r.node) throw ScriptError(context + ": unknown parent id '" + id + "'");
    if (!r.node->is_group())
        throw ScriptError(context + ": parent '" + id + "' is a " + node_kind_name(r.node->kind) + ", not a group");
    return *r.node;
}

void Document::check_new_id(const std::string& id, const std::string& context) const {
    if (id.empty()) throw ScriptError(context + ": id must be a non-empty string");
    if (id == "root") throw ScriptError(context + ": id 'root' is reserved");
    if (id_exists(id)) throw ScriptError(context + ": duplicate id '" + id + "'");
}

Node& Document::add_node_top(const std::string& parent_id, Node n, const std::string& context) {
    check_new_id(n.id, context);
    Node& parent = require_container(parent_id, context);
    parent.children.push_back(std::move(n));
    return parent.children.back();
}

void Document::undo(size_t n) { s_ = h_.pop(n); }

std::vector<Document::TileStats> Document::tile_stats() const {
    std::vector<TileStats> out;
    for (const Node& c : s_.root.children) collect_stats(c, out);
    out.push_back({"", "selection", s_.selection.mask.allocated_tiles(), s_.selection.mask.total_tiles()});
    if (s_.selection.saved)
        out.push_back({"", "saved-selection", s_.selection.saved->allocated_tiles(), s_.selection.saved->total_tiles()});
    return out;
}

}  // namespace rl
