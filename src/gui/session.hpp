// SPDX-License-Identifier: GPL-3.0-or-later
//
// EditorSession: the live rl::Document plus the GUI's view of its history.
//
// Every document change goes through apply(): one render-script op, executed by the core registry
// (op_runner), pushes exactly one core history record and one History-panel entry. Undo is the
// core `undo` op. Redo has no script op (50-resolutions ruling 4), so the session keeps, for every
// entry, the DocState right after it; a DocState copy shares all tiles (copy-on-write), so redo is
// an O(1) restore of the exact committed bytes (doc 40 §6 "redo restores the committed result
// exactly") with no core API change.
//
// Dialog previews run the op on the live document and roll it back or keep it, so what the user
// previews is what the op produces; a previewed op still becomes exactly one history entry.
#pragma once

#include <QObject>
#include <QString>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/doc/document.hpp"
#include "gui/op_runner.hpp"
#include "gui/state_diff.hpp"

namespace rl::gui {

class EditorSession : public QObject {
    Q_OBJECT
public:
    struct Entry {
        QString label;
        Json op;
        rl::DocState after;
    };

    explicit EditorSession(QObject* parent = nullptr);
    ~EditorSession() override;

    // ---- document lifetime -----------------------------------------------------------------
    // New document. `background`: 0 = white layer, 1 = transparent (no layer), 2 = `bg_color` layer.
    void new_document(int w, int h, int background, rl::Rgba8 bg_color = {255, 255, 255, 255});
    // Takes over a document built by a file loader. Its history is cleared.
    void adopt_document(std::unique_ptr<rl::Document> doc, const QString& path, const QString& base_label);

    bool has_document() const { return doc_ != nullptr; }
    rl::Document& doc() { return *doc_; }
    const rl::Document& doc() const { return *doc_; }
    const rl::DocState& state() const { return doc_->state(); }

    QString file_path() const { return path_; }
    void set_file_path(const QString& p);
    bool modified() const { return modified_; }
    void set_modified(bool m);
    QString display_name() const;

    // ---- ops -----------------------------------------------------------------------------------
    // Applies one op as one history entry labelled `label`. Emits message() on failure.
    OpResult apply(const Json& op, const QString& label);
    // The last op handed to apply() / preview(), whether or not it succeeded (diagnostics, tests).
    const Json& last_attempted_op() const { return last_op_; }
    // Records an edit that was performed directly on doc() (a live brush stroke: the engine pushed
    // its one history record and changed the document itself). `before` is the state before it.
    void record_external(const rl::DocState& before, const Json& op, const QString& label);
    // Called before every document change the session makes (apply, preview, undo, redo, new,
    // adopt). The canvas uses it to commit a live stroke first, so nothing restructures the
    // document under a running StrokeSession.
    void set_edit_guard(std::function<void()> fn) { guard_ = std::move(fn); }

    // Dialog previews. preview() replaces any previous preview; commit_preview() keeps the last
    // previewed op as one entry (or applies `op` when no preview is live); cancel_preview() restores.
    OpResult preview(const Json& op);
    OpResult commit_preview(const Json& op, const QString& label);
    void cancel_preview();
    bool previewing() const { return previewing_; }

    // ---- history ---------------------------------------------------------------------------------
    bool can_undo() const { return index_ > 0; }
    bool can_redo() const { return index_ < entries_.size(); }
    void undo(size_t steps = 1);
    void redo(size_t steps = 1);
    // Moves to the state after `applied` entries (0 = the base state).
    void jump_to(size_t applied);
    size_t applied_count() const { return index_; }
    const std::vector<Entry>& entries() const { return entries_; }
    QString base_label() const { return base_label_; }
    void set_history_depth(size_t depth);

    // The session as a render script, when it started from New (null when it started from a file).
    std::optional<Json> session_script() const;

    // ---- selection of the layer the tools act on ----------------------------------------------
    std::string active_layer() const { return active_; }
    void set_active_layer(const std::string& id);
    // The active node, or nullptr.
    const rl::Node* active_node() const;
    // Mask editing: brush strokes target the active layer's mask.
    bool edit_mask() const { return edit_mask_; }
    void set_edit_mask(bool on);

    // A layer id not used by any node: "<stem> <n>".
    std::string unique_id(const std::string& stem) const;

signals:
    void document_reset();                 // new document or canvas size changed
    void tiles_dirty(const rl::gui::StateDiff& diff);
    void tree_changed();
    void selection_changed();
    void history_changed();
    void active_layer_changed();
    void modified_changed(bool modified);
    void message(const QString& text, bool warning);

private:
    void after_change(const rl::DocState& before);
    void run_guard();
    void reconcile_depth();
    void validate_active();

    std::unique_ptr<rl::Document> doc_;
    std::vector<Entry> entries_;
    size_t index_ = 0;
    QString base_label_;
    std::optional<Json> base_script_;  // canvas + creation ops, for session_script()
    QString path_;
    bool modified_ = false;
    bool previewing_ = false;
    Json preview_op_;  // the op whose result is on screen while previewing_
    std::string active_;
    bool edit_mask_ = false;
    size_t depth_ = rl::Document::kDefaultHistoryDepth;
    std::function<void()> guard_;
    Json last_op_;
    bool in_guard_ = false;
};

}  // namespace rl::gui
