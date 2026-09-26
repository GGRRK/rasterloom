// SPDX-License-Identifier: GPL-3.0-or-later
//
// The main window: ADS docking (canvas as the non-floatable central widget; Layers, History,
// Color, Navigator as dock widgets; perspectives saved in QSettings), the Tools and Tool Options
// bars, the menus and the status bar. It contains no image math: every document change is one
// render-script op handed to the EditorSession.
#pragma once

#include <QMainWindow>
#include <QMap>
#include <QPointF>
#include <QPointer>
#include <QTimer>

#include <functional>

#include "gui/file_io.hpp"
#include "gui/op_runner.hpp"
#include "gui/tools.hpp"

class QActionGroup;
class QDialog;
class QDragEnterEvent;
class QDropEvent;
class QLabel;
class QMenu;

namespace ads {
class CDockAreaWidget;
class CDockManager;
class CDockWidget;
}  // namespace ads

namespace rl::gui {

class CanvasController;
class EditorSession;
class HistoryPanel;
class LayersPanel;
class NavigatorPanel;
class ColorPanel;
class ParamDialog;
class ToolOptionsBar;
class InputDiagnosticsDialog;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    EditorSession* session() const { return session_; }
    CanvasController* canvas() const { return canvas_; }
    QWidget* canvas_widget() const { return canvas_widget_; }
    ToolState* tools() const { return tools_; }
    LayersPanel* layers_panel() const { return layers_; }
    HistoryPanel* history_panel() const { return history_; }
    ads::CDockManager* dock_manager() const { return dock_; }
    // Every menu action has objectName "act_<key>"; tests trigger them through this.
    QAction* action(const QString& key) const;
    QStringList action_keys() const { return actions_.keys(); }
    QString last_message() const { return last_message_; }
    void set_confirm_close(bool on) { confirm_close_ = on; }
    QDialog* warnings_dialog() const { return warn_dialog_; }
    double last_open_ms() const { return last_open_ms_; }

    // Save / export without a file dialog (scripts, tests): the same path as the menu commands.
    bool save_to(const QString& path) { return write_to(path, {}, true); }
    bool export_to(const QString& path, const SaveOptions& opt = {}) { return write_to(path, opt, false); }

public slots:
    // Opens the properties dialog of an adjustment layer (set_adjustment, live preview).
    void edit_adjustment(const std::string& id);
    void duplicate_layer();
    void begin_rename();
    void select_layer_alpha(const std::string& id, Qt::KeyboardModifiers mods);
    bool save();
    bool save_as();
    void new_document_dialog();
    void new_document(int w, int h, int background);
    bool open_file(const QString& path);
    void show_message(const QString& text, bool warning);

protected:
    void closeEvent(QCloseEvent* e) override;
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dropEvent(QDropEvent* e) override;
    void showEvent(QShowEvent* e) override;
    // Tablet proximity leave is delivered to the application, not to a widget (doc 40 §5: it ends
    // the stroke).
    bool eventFilter(QObject* o, QEvent* e) override;

private:
    // ---- construction -------------------------------------------------------------------------
    void build_docks();
    void build_tools_bar();
    void build_menus();
    void build_status_bar();
    void apply_default_sizes();
    QAction* add_action(QMenu* menu, const QString& key, const QString& text, const QKeySequence& shortcut,
                        std::function<void()> fn, const QString& icon_name = {});
    QAction* add_op_action(QMenu* menu, const QString& key, const QString& text, const QKeySequence& shortcut,
                           const std::string& op_name, std::function<void()> fn);

    // ---- helpers ------------------------------------------------------------------------------------
    bool require_doc();
    bool require_raster(const QString& what);
    std::string active_parent() const;  // container id for new nodes (the active node's parent)
    void apply(const Json& op, const QString& label);
    // Runs a param dialog with live preview; `make_op` turns its values into the op.
    void run_op_dialog(ParamDialog& d, const std::function<Json()>& make_op, const QString& label);
    Json with_coverage(Json op) const;  // lead ruling 1: selection coverage for filters

    // ---- actions ------------------------------------------------------------------------------------
    void open_dialog();
    bool maybe_save(const QString& action);  // false: the user cancelled
    void show_warnings(const QString& title, const QString& intro, const QStringList& warnings);
    void report_error(const QString& title, const QString& text);
    bool write_to(const QString& path, const SaveOptions& opt, bool becomes_document);
    QString suggested_path(const QString& ext) const;
    void export_as(const QString& format);
    void export_session_script();
    void add_recent(const QString& path);
    void rebuild_recent_menu();
    void new_layer();
    void new_group();
    void new_adjustment(const QString& type);
    bool run_adjustment_dialog(const QString& type, const QString& title, const Json* initial,
                               const std::function<Json(const Json&)>& op_for, const QString& label);
    void delete_layer();
    void fill_dialog();
    void image_size_dialog();
    void canvas_size_dialog();
    void rotate_arbitrary_dialog();
    void crop_action();
    void feather_dialog();
    void expand_contract_dialog(bool expand);
    void filter_dialog(const QString& key);
    void repeat_last_filter();
    void update_repeat_filter_action();
    void preferences_dialog();
    void set_tool(Tool t);
    void cycle_tool(const QList<Tool>& group);
    void adjust_brush_size(double factor);
    void show_diagnostics();
    void save_perspective();
    void delete_perspective();
    void rebuild_window_menu();
    void update_title();
    void update_color_under_cursor();
    void update_actions();

    EditorSession* session_ = nullptr;
    ToolState* tools_ = nullptr;
    CanvasController* canvas_ = nullptr;
    QWidget* canvas_widget_ = nullptr;
    ads::CDockManager* dock_ = nullptr;
    QMap<QString, ads::CDockWidget*> docks_;
    ads::CDockAreaWidget* central_area_ = nullptr;
    ads::CDockAreaWidget* right_top_ = nullptr;
    bool sized_ = false;
    bool restored_layout_ = false;
    LayersPanel* layers_ = nullptr;
    HistoryPanel* history_ = nullptr;
    ColorPanel* color_ = nullptr;
    NavigatorPanel* navigator_ = nullptr;
    ToolOptionsBar* options_ = nullptr;
    QToolBar* tools_bar_ = nullptr;
    QActionGroup* tool_group_ = nullptr;
    QMap<Tool, QAction*> tool_actions_;
    QMap<QString, QAction*> actions_;
    QMenu* recent_menu_ = nullptr;
    QMenu* window_menu_ = nullptr;
    QLabel* msg_label_ = nullptr;
    QLabel* pos_label_ = nullptr;
    QLabel* size_label_ = nullptr;
    QLabel* zoom_label_ = nullptr;
    QLabel* backend_label_ = nullptr;
    QLabel* color_label_ = nullptr;
    QLabel* color_swatch_ = nullptr;
    QTimer color_timer_;
    QPointF cursor_c_;
    bool cursor_inside_ = false;
    QPointer<InputDiagnosticsDialog> diag_dialog_;
    QPointer<QDialog> warn_dialog_;
    double last_open_ms_ = 0.0;
    QByteArray default_layout_;
    QString last_message_;
    bool confirm_close_ = true;
    // Filter > Repeat: the last committed filter's key and dialog values (params only; the layer
    // and the selection coverage are taken fresh on each repeat, as in Photoshop).
    QString last_filter_key_;
    Json last_filter_values_;
};

}  // namespace rl::gui
