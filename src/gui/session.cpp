// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/session.hpp"

#include <QFileInfo>

#include <algorithm>

#include "gui/json_util.hpp"

namespace rl::gui {

namespace {

// Depth-first search for the first node (top of the stack first) matching `pred`.
template <class Pred>
const rl::Node* find_top_down(const rl::Node& container, Pred pred) {
    for (auto it = container.children.rbegin(); it != container.children.rend(); ++it) {
        if (pred(*it)) return &*it;
        if (it->is_group())
            if (const rl::Node* n = find_top_down(*it, pred)) return n;
    }
    return nullptr;
}

}  // namespace

EditorSession::EditorSession(QObject* parent) : QObject(parent) {}
EditorSession::~EditorSession() = default;

void EditorSession::new_document(int w, int h, int background, rl::Rgba8 bg_color) {
    run_guard();
    cancel_preview();
    doc_ = std::make_unique<rl::Document>(w, h, rl::Rgba8{0, 0, 0, 0}, depth_);
    Json script = {{"canvas", {{"w", w}, {"h", h}, {"bg", "#00000000"}}}, {"ops", Json::array()}, {"out", "png8"}};
    if (background != 1) {
        const rl::Rgba8 c = background == 0 ? rl::Rgba8{255, 255, 255, 255} : bg_color;
        Json op = {{"op", "add_layer"}, {"id", "Background"}, {"fill", "solid"}, {"color", hex(c)}};
        apply_op(*doc_, op);
        script["ops"].push_back(op);
        active_ = "Background";
    } else {
        Json op = {{"op", "add_layer"}, {"id", "Layer 1"}};
        apply_op(*doc_, op);
        script["ops"].push_back(op);
        active_ = "Layer 1";
    }
    doc_->history().clear();
    base_script_ = script;
    entries_.clear();
    index_ = 0;
    base_label_ = tr("New Document");
    path_.clear();
    edit_mask_ = false;
    set_modified(false);
    emit document_reset();
    emit tree_changed();
    emit selection_changed();
    emit history_changed();
    emit active_layer_changed();
}

void EditorSession::adopt_document(std::unique_ptr<rl::Document> doc, const QString& path, const QString& base_label) {
    run_guard();
    cancel_preview();
    doc_ = std::move(doc);
    doc_->history().clear();
    doc_->history().set_depth(depth_);
    entries_.clear();
    index_ = 0;
    base_label_ = base_label;
    base_script_.reset();
    path_ = path;
    active_.clear();
    edit_mask_ = false;
    validate_active();
    set_modified(false);
    emit document_reset();
    emit tree_changed();
    emit selection_changed();
    emit history_changed();
    emit active_layer_changed();
}

void EditorSession::run_guard() {
    if (!guard_ || in_guard_) return;
    in_guard_ = true;
    guard_();
    in_guard_ = false;
}

void EditorSession::record_external(const rl::DocState& before, const Json& op, const QString& label) {
    if (!doc_) return;
    entries_.resize(index_);
    entries_.push_back({label, op, doc_->state()});
    ++index_;
    reconcile_depth();
    set_modified(true);
    after_change(before);
    emit history_changed();
}

void EditorSession::set_file_path(const QString& p) { path_ = p; }

void EditorSession::set_modified(bool m) {
    if (m == modified_) return;
    modified_ = m;
    emit modified_changed(m);
}

QString EditorSession::display_name() const {
    return path_.isEmpty() ? tr("Untitled") : QFileInfo(path_).fileName();
}

OpResult EditorSession::apply(const Json& op, const QString& label) {
    run_guard();
    last_op_ = op;
    if (!doc_) return {OpStatus::Error, "no document"};
    if (previewing_) cancel_preview();
    const rl::DocState before = doc_->state();
    OpResult r = apply_op(*doc_, op);
    if (!r.ok()) {
        if (r.status == OpStatus::NotAvailable)
            emit message(tr("%1 is not available in this build (%2).").arg(label, to_q(r.message)), true);
        else
            emit message(label + QStringLiteral(": ") + to_q(r.message), true);
        return r;
    }
    entries_.resize(index_);
    entries_.push_back({label, op, doc_->state()});
    ++index_;
    reconcile_depth();
    set_modified(true);
    after_change(before);
    emit history_changed();
    return r;
}

OpResult EditorSession::preview(const Json& op) {
    run_guard();
    last_op_ = op;
    if (!doc_) return {OpStatus::Error, "no document"};
    const rl::DocState before = doc_->state();
    if (previewing_) {
        doc_->state() = doc_->history().pop(1);
        previewing_ = false;
    }
    OpResult r = apply_op(*doc_, op);
    previewing_ = r.ok();
    if (previewing_) preview_op_ = op;
    after_change(before);
    return r;
}

OpResult EditorSession::commit_preview(const Json& op, const QString& label) {
    // A dialog accepted before its debounced preview caught up (value typed, Enter at once) shows a
    // stale result: drop it and apply the op being recorded, so the state always matches the entry.
    if (previewing_ && op != preview_op_) cancel_preview();
    if (!previewing_) return apply(op, label);
    previewing_ = false;
    entries_.resize(index_);
    entries_.push_back({label, op, doc_->state()});
    ++index_;
    reconcile_depth();
    set_modified(true);
    emit history_changed();
    return {};
}

void EditorSession::cancel_preview() {
    if (!previewing_ || !doc_) return;
    const rl::DocState before = doc_->state();
    doc_->state() = doc_->history().pop(1);
    previewing_ = false;
    after_change(before);
}

void EditorSession::undo(size_t steps) {
    run_guard();
    if (!doc_) return;
    cancel_preview();
    steps = std::min(steps, index_);
    if (steps == 0) return;
    const rl::DocState before = doc_->state();
    OpResult r = apply_op(*doc_, Json{{"op", "undo"}, {"steps", steps}});
    if (!r.ok()) {
        emit message(tr("Undo failed: %1").arg(to_q(r.message)), true);
        return;
    }
    index_ -= steps;
    set_modified(true);
    after_change(before);
    emit history_changed();
}

void EditorSession::redo(size_t steps) {
    run_guard();
    if (!doc_) return;
    cancel_preview();
    steps = std::min(steps, entries_.size() - index_);
    if (steps == 0) return;
    const rl::DocState before = doc_->state();
    for (size_t k = 0; k < steps; ++k) {
        doc_->push_history();
        doc_->state() = entries_[index_].after;
        ++index_;
    }
    reconcile_depth();
    set_modified(true);
    after_change(before);
    emit history_changed();
}

void EditorSession::jump_to(size_t applied) {
    applied = std::min(applied, entries_.size());
    if (applied < index_) undo(index_ - applied);
    else if (applied > index_) redo(applied - index_);
}

void EditorSession::set_history_depth(size_t depth) {
    depth_ = std::clamp<size_t>(depth, 1, rl::Document::kMaxHistoryDepth);
    if (!doc_) return;
    doc_->history().set_depth(depth_);
    reconcile_depth();
    emit history_changed();
}

void EditorSession::reconcile_depth() {
    // The core history drops its oldest records beyond the depth; drop the matching entries so the
    // panel never offers a step the core can no longer undo.
    const size_t records = doc_->history_size();
    if (index_ > records) {
        const size_t drop = index_ - records;
        entries_.erase(entries_.begin(), entries_.begin() + static_cast<std::ptrdiff_t>(drop));
        index_ -= drop;
        base_label_ = tr("(older states discarded)");
        base_script_.reset();
    }
}

std::optional<Json> EditorSession::session_script() const {
    if (!base_script_) return std::nullopt;
    Json s = *base_script_;
    for (size_t i = 0; i < index_; ++i) s["ops"].push_back(entries_[i].op);
    return s;
}

void EditorSession::after_change(const rl::DocState& before) {
    const StateDiff d = diff_states(before, doc_->state());
    if (d.size_changed) emit document_reset();
    else if (d.any_pixels()) emit tiles_dirty(d);
    if (d.tree_changed || d.any_pixels()) {
        validate_active();
        emit tree_changed();
    }
    if (d.selection_changed) emit selection_changed();
}

void EditorSession::set_active_layer(const std::string& id) {
    if (id == active_) return;
    active_ = id;
    if (const rl::Node* n = active_node(); !n || !n->mask) edit_mask_ = false;
    emit active_layer_changed();
}

const rl::Node* EditorSession::active_node() const {
    if (!doc_ || active_.empty()) return nullptr;
    return find_top_down(doc_->root(), [this](const rl::Node& n) { return n.id == active_; });
}

void EditorSession::set_edit_mask(bool on) {
    const rl::Node* n = active_node();
    const bool v = on && n && n->mask.has_value();
    if (v == edit_mask_) return;
    edit_mask_ = v;
    emit active_layer_changed();
}

void EditorSession::validate_active() {
    if (!doc_) return;
    if (active_node()) {
        if (const rl::Node* n = active_node(); !n->mask && edit_mask_) {
            edit_mask_ = false;
            emit active_layer_changed();
        }
        return;
    }
    const rl::Node* n = find_top_down(doc_->root(), [](const rl::Node& x) { return x.is_raster(); });
    if (!n) n = find_top_down(doc_->root(), [](const rl::Node&) { return true; });
    active_ = n ? n->id : std::string();
    edit_mask_ = false;
    emit active_layer_changed();
}

std::string EditorSession::unique_id(const std::string& stem) const {
    for (int n = 1;; ++n) {
        std::string id = stem + " " + std::to_string(n);
        if (!doc_ || !doc_->id_exists(id)) return id;
    }
}

}  // namespace rl::gui
