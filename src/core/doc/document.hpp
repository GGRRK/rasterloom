// SPDX-License-Identifier: GPL-3.0-or-later
//
// Document = canvas size + background + layer tree + selection (DocState), plus history (C10).
//
// History: a record is a full DocState snapshot. Snapshots are cheap because a DocState copy shares
// every tile with the live state (copy-on-write); only tiles written afterwards are cloned. Every
// document-changing op pushes exactly one record (the script engine does this before running the
// op); undo(n) restores the state from before the last n records. Depth is configurable
// (1..1000); render scripts use 1000, the GUI default is 50.
#pragma once

#include <cstddef>
#include <deque>
#include <map>
#include <memory>
#include <string>

#include "core/base/types.hpp"
#include "core/doc/foreign.hpp"
#include "core/doc/node.hpp"
#include "core/doc/selection.hpp"
#include "core/tile/memory.hpp"

namespace rl {

struct DocState {
    int w = 0;
    int h = 0;
    Rgba8 bg{};      // canonical (C3)
    Node root;       // a group node with id "root"; its mode is unused
    Selection selection;
    // Document-level data a file format carried that the model does not interpret (PSD image
    // resources, global tagged blocks; core/doc/foreign.hpp). Written back unchanged on save.
    ForeignData foreign;
};

// Location of a node inside the tree: its parent container and index (0 = bottom).
struct NodeRef {
    Node* node = nullptr;
    Node* parent = nullptr;
    size_t index = 0;
};

class History {
public:
    explicit History(size_t depth = 50) : depth_(depth) {}

    size_t depth() const { return depth_; }
    void set_depth(size_t depth);  // 1..1000; drops the oldest records beyond the new depth
    size_t size() const { return records_.size(); }
    void clear() { records_.clear(); }

    void push(const DocState& before);
    // Pops n records and returns the state from before the oldest popped one. Throws ScriptError
    // when fewer than n records exist.
    DocState pop(size_t n);

private:
    size_t depth_;
    std::deque<DocState> records_;
};

class Document {
public:
    static constexpr size_t kScriptHistoryDepth = 1000;
    static constexpr size_t kDefaultHistoryDepth = 50;
    static constexpr size_t kMaxHistoryDepth = 1000;

    Document(int w, int h, Rgba8 bg, size_t history_depth = kDefaultHistoryDepth);

    DocState& state() { return s_; }
    const DocState& state() const { return s_; }
    int width() const { return s_.w; }
    int height() const { return s_.h; }
    Node& root() { return s_.root; }
    const Node& root() const { return s_.root; }
    Selection& selection() { return s_.selection; }
    const Selection& selection() const { return s_.selection; }
    Rgba8 bg() const { return s_.bg; }
    void set_bg(Rgba8 bg);  // canonicalises

    // ---- node lookup ----------------------------------------------------------------------------
    // Finds a node by id (never "root"); nullptr when absent. Pointers are invalidated by any
    // structural change of the tree.
    NodeRef find(const std::string& id);
    bool id_exists(const std::string& id) const;
    // Script-error variants: unknown id, or the wrong kind for the op.
    NodeRef require(const std::string& id, const std::string& context);
    Node& require_raster(const std::string& id, const std::string& context);
    // A container: "root" or a group id.
    Node& require_container(const std::string& id, const std::string& context);

    // Throws ScriptError when `id` is empty, "root", or already used.
    void check_new_id(const std::string& id, const std::string& context) const;

    // Inserts `n` at the top of `parent_id`'s child list. Validates the id.
    Node& add_node_top(const std::string& parent_id, Node n, const std::string& context);

    // ---- history ----------------------------------------------------------------------------------
    History& history() { return h_; }
    const History& history() const { return h_; }
    size_t history_size() const { return h_.size(); }
    // Pushes one record holding the current state (call BEFORE changing the document). This is also
    // the op boundary where tile memory is tiered (core/tile/memory.hpp: mem::safe_point).
    void push_history() {
        mem::safe_point();
        h_.push(s_);
    }
    // Restores the state from before the last n records (script error if n exceeds the records).
    void undo(size_t n = 1);

    // ---- statistics -------------------------------------------------------------------------------
    struct TileStats {
        std::string node_id;
        std::string what;  // "pixels" / "mask" / "selection" / "saved-selection"
        size_t allocated = 0;
        size_t total = 0;
    };
    // Allocated-tile counts of every image in the current state, in tree order (bottom to top,
    // depth first), then the selection.
    std::vector<TileStats> tile_stats() const;

    // ---- tool state -------------------------------------------------------------------------------
    // State that belongs to a tool, not to the document: it is not part of DocState, so history
    // records never hold it and undo never changes it (e.g. the clone stamp's source point and
    // offset, doc 40 §4, which live for one script run). Keyed by the owning domain; the slot is
    // created empty on first access.
    std::shared_ptr<void>& tool_state(const std::string& key) { return tool_state_[key]; }

private:
    DocState s_;
    History h_;
    std::map<std::string, std::shared_ptr<void>> tool_state_;
};

}  // namespace rl
