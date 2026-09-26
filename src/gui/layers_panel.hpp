// SPDX-License-Identifier: GPL-3.0-or-later
//
// Layers panel: the layer tree (top of the stack first) with visibility, lock, blend mode,
// opacity, fill, masks, clipping, groups (pass-through / isolated), drag reordering and the
// new / delete / merge buttons. Every edit is one doc-10 op through the EditorSession.
#pragma once

#include <QTreeWidget>
#include <QWidget>

#include <string>

class QAction;
class QComboBox;
class QLineEdit;
class QSpinBox;
class QToolButton;
class QHBoxLayout;
class QLabel;

namespace rl {
struct Node;
}

namespace rl::gui {

class EditorSession;

// A tree whose drops become move_layer ops instead of moving items itself.
class LayerTree : public QTreeWidget {
    Q_OBJECT
public:
    explicit LayerTree(QWidget* parent = nullptr);

signals:
    // parent: "root" or a group id; index: 0 = bottom, counted after removal (doc 10 move_layer).
    void move_requested(const QString& id, const QString& parent, int index);

protected:
    void dropEvent(QDropEvent* e) override;
};

class LayersPanel : public QWidget {
    Q_OBJECT
public:
    explicit LayersPanel(EditorSession* s, QWidget* parent = nullptr);
    void add_footer_action(QAction* a);
    void add_footer_menu(const QIcon& icon, const QString& tip, QMenu* menu);
    void add_footer_stretch();
    LayerTree* tree() const { return tree_; }

    // Blend-mode entries (display name, JSON value) in menu order; groups get Pass Through first.
    static QList<QPair<QString, QString>> blend_modes(bool group);
    // Actions offered by the right-click menu (the main window owns them).
    void set_context_actions(const QList<QAction*>& actions) { context_actions_ = actions; }
    // Inline rename of a node (sends set_name; Enter commits, Esc cancels).
    void begin_rename(const std::string& id);
    QLineEdit* rename_editor() const { return rename_; }
    // Commits a rename (also used by tests).
    bool rename(const std::string& id, const QString& name);

signals:
    void edit_adjustment_requested(const std::string& id);
    void select_alpha_requested(const std::string& id, Qt::KeyboardModifiers mods);

public slots:
    void rebuild();
    void refresh_thumbnails();
    void sync_controls();

private:
    void add_items(QTreeWidgetItem* parent_item, const rl::Node& container);
    void on_item_clicked(QTreeWidgetItem* item, int column);
    void on_item_double_clicked(QTreeWidgetItem* item, int column);
    void on_selection_changed();
    QIcon layer_thumb(const rl::Node& n) const;
    QIcon mask_thumb(const rl::Node& n) const;
    bool eventFilter(QObject* o, QEvent* e) override;
    QTreeWidgetItem* item_for(const std::string& id) const;
    void end_rename(bool commit);

    EditorSession* s_;
    LayerTree* tree_;
    QComboBox* blend_;
    QSpinBox* opacity_;
    QSpinBox* fill_;
    QToolButton* lock_;
    QToolButton* clip_;
    QHBoxLayout* footer_;
    bool syncing_ = false;
    bool rebuilding_ = false;
    QList<QAction*> context_actions_;
    QLineEdit* rename_ = nullptr;
    std::string rename_id_;
    QPoint press_pos_;
    Qt::KeyboardModifiers press_mods_;
};

}  // namespace rl::gui
