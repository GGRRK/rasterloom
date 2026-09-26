// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/main_window.hpp"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QFileDialog>
#include <QVBoxLayout>
#include <QUrl>
#include <QMimeData>
#include <QListWidget>
#include <QElapsedTimer>
#include <QDropEvent>
#include <QDragEnterEvent>
#include <QDir>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QImageWriter>
#include <QFontMetrics>
#include <QInputDialog>
#include <QLabel>
#include <QMenuBar>
#include <QPixmap>
#include <QPainter>
#include <QMessageBox>
#include <QSaveFile>
#include <QSettings>
#include <QSpinBox>
#include <QStatusBar>
#include <QStyle>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>

#include <DockAreaWidget.h>
#include <DockManager.h>
#include <DockWidget.h>

#include <cmath>

#include "core/adjust/adjustment.hpp"
#include "core/base/version.hpp"
#include "gui/app_info.hpp"
#include "gui/canvas.hpp"
#include "gui/canvas_gl.hpp"
#include "gui/canvas_widgets.hpp"
#include "gui/diagnostics.hpp"
#include "gui/dialogs.hpp"
#include "gui/file_io.hpp"
#include "gui/icons.hpp"
#include "gui/image_import.hpp"
#include "gui/json_util.hpp"
#include "gui/layers_panel.hpp"
#include "gui/panels.hpp"
#include "gui/session.hpp"
#include "gui/theme.hpp"
#include "gui/tool_options.hpp"

namespace rl::gui {

namespace {

QSettings settings() { return QSettings(QStringLiteral("Rasterloom"), QStringLiteral("Rasterloom")); }

struct AdjInfo {
    const char* type;
    const char* name;
};
const AdjInfo kAdjustments[] = {{"levels", "Levels"},
                                {"curves", "Curves"},
                                {"brightness_contrast", "Brightness/Contrast"},
                                {"hue_saturation", "Hue/Saturation"},
                                {"black_white", "Black & White"},
                                {"invert", "Invert"},
                                {"posterize", "Posterize"},
                                {"threshold", "Threshold"}};

struct FilterInfo {
    const char* key;
    const char* op;
    const char* name;
};
const FilterInfo kFilters[] = {{"gaussian_blur", "filter_gaussian_blur", "Gaussian Blur"},
                               {"motion_blur", "filter_motion_blur", "Motion Blur"},
                               {"unsharp_mask", "filter_unsharp_mask", "Unsharp Mask"},
                               {"add_noise", "filter_add_noise", "Add Noise"},
                               {"high_pass", "filter_high_pass", "High Pass"},
                               {"offset", "filter_offset", "Offset"}};

// Rich tool tooltip: name, shortcut, and the modifiers the tool understands.
QString tool_tooltip(const ToolInfo& ti) {
    QString hint;
    switch (ti.tool) {
        case Tool::Move: hint = QObject::tr("Drag moves the active layer; arrow keys nudge 1 px (Shift: 10 px)"); break;
        case Tool::MarqueeRect:
        case Tool::MarqueeEllipse:
        case Tool::Lasso:
            hint = QObject::tr("Shift adds, Alt subtracts, Shift+Alt intersects; a click deselects");
            break;
        case Tool::PolygonLasso: hint = QObject::tr("Click to add points; double-click or Enter closes; Backspace removes the last point"); break;
        case Tool::Wand: hint = QObject::tr("Shift adds, Alt subtracts; tolerance in the options bar"); break;
        case Tool::Crop: hint = QObject::tr("Drag a rectangle, drag inside to move it; Enter or double-click crops, Esc cancels"); break;
        case Tool::Eyedropper: hint = QObject::tr("Click picks the foreground colour; Alt+click the background"); break;
        case Tool::Brush: hint = QObject::tr("[ and ] resize; Alt+click picks a colour; paints the mask when the mask thumbnail is active"); break;
        case Tool::Eraser: hint = QObject::tr("[ and ] resize; the pen's eraser end erases with any tool"); break;
        case Tool::Clone: hint = QObject::tr("Alt+click sets the source; [ and ] resize; Aligned in the options bar"); break;
        case Tool::Gradient: hint = QObject::tr("Drag from the foreground to the background colour"); break;
        case Tool::Bucket: hint = QObject::tr("Click fills similar colours with the foreground colour"); break;
        case Tool::Hand: hint = QObject::tr("Drag pans; hold Space with any tool; the wheel pans too"); break;
        case Tool::Zoom: hint = QObject::tr("Click zooms in, Alt+click out; drag a rectangle to zoom to it; Ctrl+wheel or Alt+wheel zooms at the cursor"); break;
        case Tool::Transform: break;
    }
    return QStringLiteral("<b>%1</b> &nbsp;<span style='color:#9aa0a8'>%2</span><br>%3")
        .arg(QObject::tr(ti.name), QString::fromLatin1(ti.key), hint.toHtmlEscaped());
}

const QStringList kEdgeLabels = {QObject::tr("Clamp to edge"), QObject::tr("Transparent")};
const QStringList kEdgeValues = {QStringLiteral("clamp"), QStringLiteral("transparent")};

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setObjectName(QStringLiteral("MainWindow"));
    session_ = new EditorSession(this);
    tools_ = new ToolState(this);
    canvas_ = new CanvasController(session_, tools_, this);
    {
        QSettings s = settings();
        session_->set_history_depth(static_cast<size_t>(s.value(QStringLiteral("history/depth"), 50).toInt()));
    }

    build_docks();
    options_ = new ToolOptionsBar(tools_, canvas_, session_, this);
    addToolBar(Qt::TopToolBarArea, options_);
    build_tools_bar();
    build_menus();
    build_status_bar();

    connect(session_, &EditorSession::message, this, &MainWindow::show_message);
    connect(canvas_, &CanvasController::message, this, &MainWindow::show_message);
    connect(canvas_, &CanvasController::color_sampled, this, [this](const QColor& c, bool bg) {
        QColor o = c;
        o.setAlpha(255);
        if (bg) tools_->set_bg(o);
        else tools_->set_fg(o);
    });
    connect(session_, &EditorSession::history_changed, this, &MainWindow::update_actions);
    connect(session_, &EditorSession::tree_changed, this, &MainWindow::update_actions);
    connect(session_, &EditorSession::selection_changed, this, &MainWindow::update_actions);
    connect(session_, &EditorSession::active_layer_changed, this, &MainWindow::update_actions);
    connect(session_, &EditorSession::modified_changed, this, &MainWindow::update_title);
    connect(session_, &EditorSession::document_reset, this, &MainWindow::update_title);
    connect(session_, &EditorSession::tiles_dirty, layers_, &LayersPanel::refresh_thumbnails);
    connect(tools_, &ToolState::tool_changed, this, [this](Tool t) {
        if (QAction* a = tool_actions_.value(t)) a->setChecked(true);
        else if (QAction* c = tool_group_->checkedAction()) c->setChecked(false);  // Free Transform
    });

    qApp->installEventFilter(this);
    setAcceptDrops(true);
    resize(1480, 920);
    default_layout_ = dock_->saveState();
    {
        QSettings s = settings();
        dock_->loadPerspectives(s);
        const QByteArray st = s.value(QStringLiteral("layout/dock")).toByteArray();
        if (!st.isEmpty()) restored_layout_ = dock_->restoreState(st);
        const QByteArray geo = s.value(QStringLiteral("layout/window")).toByteArray();
        if (!geo.isEmpty()) restoreGeometry(geo);
    }
    set_tool(Tool::Brush);
    new_document(1600, 1000, 0);
    session_->mark_placeholder();  // "no document open" until the user touches it
}

MainWindow::~MainWindow() = default;

// ---- construction --------------------------------------------------------------------------------------

void MainWindow::build_docks() {
    ads::CDockManager::setConfigFlags(ads::CDockManager::DefaultOpaqueConfig);
    ads::CDockManager::setConfigFlag(ads::CDockManager::FocusHighlighting, true);
    ads::CDockManager::setConfigFlag(ads::CDockManager::DockAreaHasUndockButton, false);
    dock_ = new ads::CDockManager(this);
    dock_->setObjectName(QStringLiteral("DockManager"));

    CanvasChoice choice;
    canvas_widget_ = create_canvas_widget(canvas_, nullptr, &choice);
    if (auto* gl = qobject_cast<CanvasGLWidget*>(canvas_widget_)) {
        // Late GL failure (initializeGL could not get 3.3 functions or compile shaders): swap in
        // the raster widget, never leave a blank canvas.
        connect(gl, &CanvasGLWidget::gl_failed, this, [this](const QString& why) {
            QTimer::singleShot(0, this, [this, why] {
                auto* cw = docks_.value(QStringLiteral("canvas"));
                if (!cw) return;
                diag().canvas_backend = QStringLiteral("raster");
                diag().canvas_reason = QStringLiteral("GL failed late: %1").arg(why);
                auto* raster = new CanvasRasterWidget(canvas_);
                canvas_->set_widget(raster);
                QWidget* old = cw->takeWidget();
                cw->setWidget(raster, ads::CDockWidget::ForceNoScrollArea);
                canvas_widget_ = raster;
                if (old) old->deleteLater();
                if (backend_label_) backend_label_->setText(QStringLiteral("raster"));
                show_message(tr("OpenGL canvas failed (%1); using the raster canvas.").arg(why), true);
            });
        });
    }
    auto* canvas_dock = new ads::CDockWidget(dock_, tr("Canvas"));
    canvas_dock->setObjectName(QStringLiteral("CanvasDock"));
    // No QScrollArea around the canvas: QScrollArea::setWidget() turns autoFillBackground on, and on
    // a QOpenGLWidget that makes every QPainter::begin() clear the frame the GL pass just drew.
    canvas_dock->setWidget(canvas_widget_, ads::CDockWidget::ForceNoScrollArea);
    canvas_dock->setFeature(ads::CDockWidget::NoTab, true);
    canvas_dock->setFeature(ads::CDockWidget::DockWidgetClosable, false);
    canvas_dock->setFeature(ads::CDockWidget::DockWidgetFloatable, false);
    canvas_dock->setFeature(ads::CDockWidget::DockWidgetMovable, false);
    central_area_ = dock_->setCentralWidget(canvas_dock);
    docks_.insert(QStringLiteral("canvas"), canvas_dock);

    const auto make_dock = [this](const QString& key, const QString& title, QWidget* w, const QString& ic) {
        auto* d = new ads::CDockWidget(dock_, title);
        d->setObjectName(key + QStringLiteral("Dock"));
        d->setWidget(w, ads::CDockWidget::ForceNoScrollArea);
        d->setIcon(icon(ic));
        d->setMinimumSizeHintMode(ads::CDockWidget::MinimumSizeHintFromDockWidget);
        docks_.insert(key, d);
        return d;
    };
    navigator_ = new NavigatorPanel(session_, canvas_);
    color_ = new ColorPanel(tools_);
    layers_ = new LayersPanel(session_);
    history_ = new HistoryPanel(session_);
    auto* nav = make_dock(QStringLiteral("navigator"), tr("Navigator"), navigator_, QStringLiteral("navigator"));
    auto* col = make_dock(QStringLiteral("color"), tr("Color"), color_, QStringLiteral("color"));
    auto* lay = make_dock(QStringLiteral("layers"), tr("Layers"), layers_, QStringLiteral("layers"));
    auto* his = make_dock(QStringLiteral("history"), tr("History"), history_, QStringLiteral("history"));
    right_top_ = dock_->addDockWidget(ads::RightDockWidgetArea, nav);
    dock_->addDockWidgetTabToArea(col, right_top_);
    auto* right_mid = dock_->addDockWidget(ads::BottomDockWidgetArea, lay, right_top_);
    dock_->addDockWidget(ads::BottomDockWidgetArea, his, right_mid);
    right_top_->setCurrentIndex(1);  // Color in front, Navigator one tab away
}

void MainWindow::apply_default_sizes() {
    // Right column ~330 px; Layers gets the most height.
    const int w = std::max(800, dock_->width());
    dock_->setSplitterSizes(central_area_, {w - 336, 336});
    const int h = std::max(500, dock_->height());
    dock_->setSplitterSizes(right_top_, {static_cast<int>(h * 0.36), static_cast<int>(h * 0.40), h - static_cast<int>(h * 0.36) - static_cast<int>(h * 0.40)});
}

void MainWindow::showEvent(QShowEvent* e) {
    QMainWindow::showEvent(e);
    if (sized_) return;
    sized_ = true;
    if (!restored_layout_) {
        apply_default_sizes();
        QTimer::singleShot(0, this, [this] {
            apply_default_sizes();
            default_layout_ = dock_->saveState();
        });
    }
}

void MainWindow::build_tools_bar() {
    tools_bar_ = new QToolBar(tr("Tools"), this);
    tools_bar_->setObjectName(QStringLiteral("Tools"));
    tools_bar_->setOrientation(Qt::Vertical);
    tools_bar_->setMovable(false);
    tools_bar_->setIconSize(QSize(20, 20));
    addToolBar(Qt::LeftToolBarArea, tools_bar_);
    tool_group_ = new QActionGroup(this);
    tool_group_->setExclusionPolicy(QActionGroup::ExclusionPolicy::ExclusiveOptional);
    Tool prev_group_head = Tool::Transform;
    for (const ToolInfo& ti : tool_list()) {
        // Separators between the families (move/select, paint, navigate).
        if (ti.tool == Tool::Brush || ti.tool == Tool::Hand || ti.tool == Tool::MarqueeRect) tools_bar_->addSeparator();
        (void)prev_group_head;
        auto* a = new QAction(icon(QString::fromLatin1(ti.icon)), tr(ti.name), this);
        a->setObjectName(QStringLiteral("tool_%1").arg(QString::fromLatin1(ti.id)));
        a->setCheckable(true);
        a->setToolTip(tool_tooltip(ti));
        a->setStatusTip(QStringLiteral("%1 (%2)").arg(tr(ti.name), QString::fromLatin1(ti.key)));
        const Tool t = ti.tool;
        connect(a, &QAction::triggered, this, [this, t] { set_tool(t); });
        tool_group_->addAction(a);
        tools_bar_->addAction(a);
        tool_actions_.insert(t, a);
    }
    // Single-key shortcuts; shared keys cycle within their group.
    const auto key = [this](const QKeySequence& k, std::function<void()> fn, const QString& name) {
        auto* a = new QAction(this);
        a->setObjectName(QStringLiteral("key_") + name);
        a->setShortcut(k);
        a->setShortcutContext(Qt::WindowShortcut);
        connect(a, &QAction::triggered, this, fn);
        addAction(a);
    };
    key(QKeySequence(Qt::Key_V), [this] { set_tool(Tool::Move); }, QStringLiteral("move"));
    key(QKeySequence(Qt::Key_M), [this] { cycle_tool({Tool::MarqueeRect, Tool::MarqueeEllipse}); }, QStringLiteral("marquee"));
    key(QKeySequence(Qt::Key_L), [this] { cycle_tool({Tool::Lasso, Tool::PolygonLasso}); }, QStringLiteral("lasso"));
    key(QKeySequence(Qt::Key_W), [this] { set_tool(Tool::Wand); }, QStringLiteral("wand"));
    key(QKeySequence(Qt::Key_C), [this] { set_tool(Tool::Crop); }, QStringLiteral("crop"));
    key(QKeySequence(Qt::Key_I), [this] { set_tool(Tool::Eyedropper); }, QStringLiteral("eyedropper"));
    key(QKeySequence(Qt::Key_B), [this] { set_tool(Tool::Brush); }, QStringLiteral("brush"));
    key(QKeySequence(Qt::Key_E), [this] { set_tool(Tool::Eraser); }, QStringLiteral("eraser"));
    key(QKeySequence(Qt::Key_S), [this] { set_tool(Tool::Clone); }, QStringLiteral("clone"));
    key(QKeySequence(Qt::Key_G), [this] { cycle_tool({Tool::Gradient, Tool::Bucket}); }, QStringLiteral("gradient"));
    key(QKeySequence(Qt::Key_H), [this] { set_tool(Tool::Hand); }, QStringLiteral("hand"));
    key(QKeySequence(Qt::Key_Z), [this] { set_tool(Tool::Zoom); }, QStringLiteral("zoom"));
    key(QKeySequence(Qt::Key_X), [this] { tools_->swap_colors(); }, QStringLiteral("swap"));
    key(QKeySequence(Qt::Key_D), [this] { tools_->reset_colors(); }, QStringLiteral("default_colors"));
    key(QKeySequence(Qt::Key_BracketLeft), [this] { adjust_brush_size(1.0 / 1.25); }, QStringLiteral("smaller"));
    key(QKeySequence(Qt::Key_BracketRight), [this] { adjust_brush_size(1.25); }, QStringLiteral("bigger"));
}

QAction* MainWindow::add_action(QMenu* menu, const QString& key, const QString& text, const QKeySequence& shortcut,
                                std::function<void()> fn, const QString& icon_name) {
    auto* a = new QAction(text, this);
    a->setObjectName(QStringLiteral("act_") + key);
    if (!shortcut.isEmpty()) a->setShortcut(shortcut);
    if (!icon_name.isEmpty()) a->setIcon(icon(icon_name));
    connect(a, &QAction::triggered, this, [fn] { fn(); });
    if (menu) menu->addAction(a);
    addAction(a);  // shortcuts work even when the menu bar is hidden
    actions_.insert(key, a);
    return a;
}

QAction* MainWindow::add_op_action(QMenu* menu, const QString& key, const QString& text, const QKeySequence& shortcut,
                                   const std::string& op_name, std::function<void()> fn) {
    QAction* a = add_action(menu, key, text, shortcut, std::move(fn));
    if (!op_available(op_name)) {
        a->setToolTip(tr("%1 (not available in this build: op '%2')").arg(text, to_q(op_name)));
        a->setStatusTip(a->toolTip());
    }
    return a;
}

void MainWindow::build_menus() {
    QMenuBar* mb = menuBar();
    // ---- File ----
    QMenu* file = mb->addMenu(tr("&File"));
    add_action(file, QStringLiteral("new"), tr("&New..."), QKeySequence::New, [this] { new_document_dialog(); }, QStringLiteral("new_doc"));
    add_action(file, QStringLiteral("open"), tr("&Open..."), QKeySequence::Open, [this] { open_dialog(); }, QStringLiteral("open"));
    recent_menu_ = file->addMenu(tr("Open &Recent"));
    recent_menu_->setObjectName(QStringLiteral("RecentMenu"));
    connect(recent_menu_, &QMenu::aboutToShow, this, &MainWindow::rebuild_recent_menu);
    add_op_action(file, QStringLiteral("place"), tr("&Place..."), {}, "place_image", [this] { place_dialog(); });
    file->addSeparator();
    add_action(file, QStringLiteral("save"), tr("&Save"), QKeySequence::Save, [this] { save(); }, QStringLiteral("save"));
    add_action(file, QStringLiteral("save_as"), tr("Save &As..."), QKeySequence::SaveAs, [this] { save_as(); });
    QMenu* exp = file->addMenu(tr("&Export"));
    add_action(exp, QStringLiteral("export_png"), tr("PNG..."), QKeySequence(tr("Ctrl+Alt+Shift+W")), [this] { export_as(QStringLiteral("png")); });
    add_action(exp, QStringLiteral("export_jpeg"), tr("JPEG..."), {}, [this] { export_as(QStringLiteral("jpeg")); });
    add_action(exp, QStringLiteral("export_tiff"), tr("TIFF..."), {}, [this] { export_as(QStringLiteral("tiff")); });
    add_action(exp, QStringLiteral("export_psd"), tr("PSD / PSB..."), {}, [this] { export_as(QStringLiteral("psd")); });
    add_action(exp, QStringLiteral("export_ora"), tr("OpenRaster (.ora)..."), {}, [this] { export_as(QStringLiteral("ora")); });
    if (QImageWriter::supportedImageFormats().contains("webp"))
        add_action(exp, QStringLiteral("export_webp"), tr("WebP (lossless)..."), {}, [this] { export_as(QStringLiteral("webp")); });
    add_action(exp, QStringLiteral("export_bmp"), tr("BMP..."), {}, [this] { export_as(QStringLiteral("bmp")); });
    exp->addSeparator();
    add_action(exp, QStringLiteral("export_script"), tr("Session as Render Script..."), {}, [this] { export_session_script(); });
    file->addSeparator();
    add_action(file, QStringLiteral("quit"), tr("&Quit"), QKeySequence::Quit, [this] { close(); });

    // ---- Edit ----
    QMenu* edit = mb->addMenu(tr("&Edit"));
    add_action(edit, QStringLiteral("undo"), tr("&Undo"), QKeySequence(tr("Ctrl+Z")), [this] { session_->undo(); }, QStringLiteral("undo"));
    QAction* redo = add_action(edit, QStringLiteral("redo"), tr("&Redo"), QKeySequence(tr("Ctrl+Shift+Z")), [this] { session_->redo(); },
                               QStringLiteral("redo"));
    redo->setShortcuts({QKeySequence(tr("Ctrl+Shift+Z")), QKeySequence(tr("Ctrl+Y"))});
    edit->addSeparator();
    add_op_action(edit, QStringLiteral("cut"), tr("Cu&t"), QKeySequence(tr("Ctrl+X")), "clear", [this] { copy_selection(false, true); });
    add_action(edit, QStringLiteral("copy"), tr("&Copy"), QKeySequence(tr("Ctrl+C")), [this] { copy_selection(false, false); });
    add_action(edit, QStringLiteral("copy_merged"), tr("Copy &Merged"), QKeySequence(tr("Ctrl+Shift+C")), [this] { copy_selection(true, false); });
    add_op_action(edit, QStringLiteral("paste"), tr("&Paste"), QKeySequence(tr("Ctrl+V")), "place_image", [this] { paste(false); });
    add_op_action(edit, QStringLiteral("paste_in_place"), tr("Paste in P&lace"), QKeySequence(tr("Ctrl+Shift+V")), "place_image",
                  [this] { paste(true); });
    add_op_action(edit, QStringLiteral("clear"), tr("Cl&ear"), QKeySequence(Qt::Key_Delete), "clear", [this] { clear_selected(); });
    edit->addSeparator();
    add_op_action(edit, QStringLiteral("fill"), tr("&Fill..."), QKeySequence(tr("Shift+F5")), "fill_selection", [this] { fill_dialog(); });
    add_op_action(edit, QStringLiteral("free_transform"), tr("Free &Transform"), QKeySequence(tr("Ctrl+T")), "transform",
                  [this] { canvas_->begin_free_transform(); });
    edit->addSeparator();
    add_action(edit, QStringLiteral("preferences"), tr("Pre&ferences..."), QKeySequence(tr("Ctrl+K")), [this] { preferences_dialog(); });

    // ---- Image ----
    QMenu* image = mb->addMenu(tr("&Image"));
    add_op_action(image, QStringLiteral("image_size"), tr("Image &Size..."), QKeySequence(tr("Ctrl+Alt+I")), "image_size",
                  [this] { image_size_dialog(); });
    add_op_action(image, QStringLiteral("canvas_size"), tr("&Canvas Size..."), QKeySequence(tr("Ctrl+Alt+C")), "canvas_size",
                  [this] { canvas_size_dialog(); });
    add_op_action(image, QStringLiteral("crop"), tr("C&rop"), {}, "crop", [this] { crop_action(); });
    QMenu* rot = image->addMenu(tr("&Rotate Canvas"));
    add_op_action(rot, QStringLiteral("rotate_180"), tr("180°"), {}, "rotate_canvas",
                  [this] { apply({{"op", "rotate_canvas"}, {"angle", 180}}, tr("Rotate 180°")); });
    add_op_action(rot, QStringLiteral("rotate_90cw"), tr("90° Clockwise"), {}, "rotate_canvas",
                  [this] { apply({{"op", "rotate_canvas"}, {"angle", 90}}, tr("Rotate 90° CW")); });
    add_op_action(rot, QStringLiteral("rotate_90ccw"), tr("90° Counter-Clockwise"), {}, "rotate_canvas",
                  [this] { apply({{"op", "rotate_canvas"}, {"angle", -90}}, tr("Rotate 90° CCW")); });
    add_op_action(rot, QStringLiteral("rotate_arbitrary"), tr("Arbitrary..."), {}, "rotate_canvas", [this] { rotate_arbitrary_dialog(); });
    image->addSeparator();
    add_op_action(image, QStringLiteral("flip_canvas_h"), tr("Flip Canvas &Horizontal"), {}, "flip",
                  [this] { apply({{"op", "flip"}, {"axis", "h"}}, tr("Flip Canvas Horizontal")); });
    add_op_action(image, QStringLiteral("flip_canvas_v"), tr("Flip Canvas &Vertical"), {}, "flip",
                  [this] { apply({{"op", "flip"}, {"axis", "v"}}, tr("Flip Canvas Vertical")); });
    add_op_action(image, QStringLiteral("flip_layer_h"), tr("Flip Layer Horizontal"), {}, "flip", [this] {
        if (require_raster(tr("Flip Layer"))) apply({{"op", "flip"}, {"axis", "h"}, {"layer", session_->active_layer()}}, tr("Flip Layer Horizontal"));
    });
    add_op_action(image, QStringLiteral("flip_layer_v"), tr("Flip Layer Vertical"), {}, "flip", [this] {
        if (require_raster(tr("Flip Layer"))) apply({{"op", "flip"}, {"axis", "v"}, {"layer", session_->active_layer()}}, tr("Flip Layer Vertical"));
    });

    // ---- Layer ----
    QMenu* layer = mb->addMenu(tr("&Layer"));
    QAction* nl = add_action(layer, QStringLiteral("new_layer"), tr("&New Layer"), QKeySequence(tr("Ctrl+Shift+N")), [this] { new_layer(); },
                             QStringLiteral("plus"));
    QAction* ng = add_action(layer, QStringLiteral("new_group"), tr("New &Group"), QKeySequence(tr("Ctrl+G")), [this] { new_group(); },
                             QStringLiteral("folder"));
    QMenu* adj = layer->addMenu(icon(QStringLiteral("adjust")), tr("New &Adjustment Layer"));
    for (const AdjInfo& ai : kAdjustments) {
        const QString type = QString::fromLatin1(ai.type);
        QAction* a = add_op_action(adj, QStringLiteral("adj_") + type, tr(ai.name) + (type == QLatin1String("invert") ? QString() : QStringLiteral("...")),
                                   {}, "add_adjustment", [this, type] { new_adjustment(type); });
        (void)a;
    }
    QAction* del = add_op_action(layer, QStringLiteral("delete_layer"), tr("&Delete Layer"), QKeySequence(tr("Ctrl+Shift+Backspace")),
                                 "delete_layer", [this] { delete_layer(); });
    del->setIcon(icon(QStringLiteral("trash")));
    add_op_action(layer, QStringLiteral("duplicate_layer"), tr("D&uplicate Layer"), QKeySequence(tr("Ctrl+J")), "duplicate_layer",
                  [this] { duplicate_layer(); });
    add_op_action(layer, QStringLiteral("layer_via_copy"), tr("Layer via Cop&y"), {}, "layer_via_copy", [this] { layer_via(false); });
    add_op_action(layer, QStringLiteral("layer_via_cut"), tr("Layer via Cu&t"), QKeySequence(tr("Ctrl+Shift+J")), "layer_via_copy",
                  [this] { layer_via(true); });
    add_action(layer, QStringLiteral("rename_layer"), tr("&Rename Layer..."), QKeySequence(tr("F2")), [this] { begin_rename(); });
    // Layer Properties: an adjustment layer's properties are its params (set_adjustment, doc 60 §5);
    // any other layer's editable property here is its name (set_name, §3).
    add_action(layer, QStringLiteral("layer_properties"), tr("Layer &Properties..."), {}, [this] {
        const rl::Node* n = session_->has_document() ? session_->active_node() : nullptr;
        if (!n) return;
        if (n->is_adjustment()) edit_adjustment(n->id);
        else begin_rename();
    });
    add_op_action(layer, QStringLiteral("edit_adjustment"), tr("Adjustment &Settings..."), {}, "set_adjustment", [this] {
        if (session_->has_document() && session_->active_node()) edit_adjustment(session_->active_layer());
    });
    QAction* iso = add_op_action(layer, QStringLiteral("isolate_group"), tr("&Isolate Group"), {}, "set_group_mode", [this] {
        const rl::Node* n = session_->has_document() ? session_->active_node() : nullptr;
        if (!n || !n->is_group()) return show_message(tr("Isolate Group needs a group: select one in the Layers panel."), true);
        const bool pass = n->mode == rl::BlendMode::Pass;
        apply({{"op", "set_group_mode"}, {"layer", n->id}, {"mode", pass ? "isolated" : "pass"}},
              pass ? tr("Isolate Group") : tr("Pass Through Group"));
    });
    iso->setCheckable(true);
    add_op_action(layer, QStringLiteral("select_alpha"), tr("Select Layer &Pixels"), {}, "select_alpha", [this] {
        if (session_->has_document() && session_->active_node()) select_layer_alpha(session_->active_layer(), Qt::NoModifier);
    });
    layer->addSeparator();
    QMenu* mask = layer->addMenu(icon(QStringLiteral("mask")), tr("Layer &Mask"));
    QAction* add_mask = add_op_action(mask, QStringLiteral("mask_reveal"), tr("Reveal All"), {}, "add_mask", [this] {
        if (require_doc() && session_->active_node())
            apply({{"op", "add_mask"}, {"layer", session_->active_layer()}, {"fill", "solid"}, {"value", 255}}, tr("Add Layer Mask"));
    });
    add_mask->setIcon(icon(QStringLiteral("mask")));
    add_op_action(mask, QStringLiteral("mask_hide"), tr("Hide All"), {}, "add_mask", [this] {
        if (require_doc() && session_->active_node())
            apply({{"op", "add_mask"}, {"layer", session_->active_layer()}, {"fill", "solid"}, {"value", 0}}, tr("Add Layer Mask (Hide All)"));
    });
    mask->addSeparator();
    add_action(mask, QStringLiteral("mask_edit"), tr("Paint on Mask"), {}, [this] {
        session_->set_edit_mask(!session_->edit_mask());
        layers_->refresh_thumbnails();
        show_message(session_->edit_mask() ? tr("Brush strokes now paint the layer mask (black hides, white reveals).")
                                           : tr("Brush strokes paint the layer pixels."),
                     false);
    });
    add_op_action(mask, QStringLiteral("mask_toggle"), tr("Enable / Disable"), {}, "set_mask_enabled", [this] {
        const rl::Node* n = session_->active_node();
        if (n && n->mask)
            apply({{"op", "set_mask_enabled"}, {"layer", n->id}, {"value", !n->mask->enabled}},
                  n->mask->enabled ? tr("Disable Layer Mask") : tr("Enable Layer Mask"));
    });
    add_op_action(mask, QStringLiteral("mask_apply"), tr("Apply"), {}, "apply_mask", [this] {
        if (session_->active_node()) apply({{"op", "apply_mask"}, {"layer", session_->active_layer()}}, tr("Apply Layer Mask"));
    });
    add_op_action(mask, QStringLiteral("mask_delete"), tr("Delete"), {}, "delete_mask", [this] {
        if (session_->active_node()) apply({{"op", "delete_mask"}, {"layer", session_->active_layer()}}, tr("Delete Layer Mask"));
    });
    add_op_action(layer, QStringLiteral("clip"), tr("Create / Release &Clipping Mask"), QKeySequence(tr("Ctrl+Alt+G")), "set_clip", [this] {
        const rl::Node* n = session_->active_node();
        if (n && !n->is_group())
            apply({{"op", "set_clip"}, {"layer", n->id}, {"value", !n->clip}}, n->clip ? tr("Release Clipping Mask") : tr("Create Clipping Mask"));
    });
    add_op_action(layer, QStringLiteral("clbl"), tr("Blend Clipped Layers as Group"), {}, "set_clbl", [this] {
        const rl::Node* n = session_->active_node();
        if (n && !n->is_group())
            apply({{"op", "set_clbl"}, {"layer", n->id}, {"value", !n->clbl}}, n->clbl ? tr("Blend Clipped Individually") : tr("Blend Clipped as Group"));
    });
    add_op_action(layer, QStringLiteral("lock_alpha"), tr("Lock &Transparency"), QKeySequence(Qt::Key_Slash), "lock_transparency", [this] {
        const rl::Node* n = session_->active_node();
        if (n && n->is_raster())
            apply({{"op", "lock_transparency"}, {"layer", n->id}, {"value", !n->lock_alpha}},
                  n->lock_alpha ? tr("Unlock Transparency") : tr("Lock Transparency"));
    });
    layer->addSeparator();
    QAction* md = add_op_action(layer, QStringLiteral("merge_down"), tr("Merge &Down"), QKeySequence(tr("Ctrl+E")), "merge_down", [this] {
        if (session_->active_node()) apply({{"op", "merge_down"}, {"layer", session_->active_layer()}}, tr("Merge Down"));
    });
    md->setIcon(icon(QStringLiteral("merge")));
    add_op_action(layer, QStringLiteral("merge_visible"), tr("Merge &Visible"), QKeySequence(tr("Ctrl+Shift+E")), "merge_visible", [this] {
        if (require_doc()) apply({{"op", "merge_visible"}, {"id", session_->unique_id("Merged")}}, tr("Merge Visible"));
    });
    add_op_action(layer, QStringLiteral("flatten"), tr("&Flatten Image"), {}, "flatten", [this] {
        if (require_doc()) apply({{"op", "flatten"}, {"id", session_->unique_id("Flattened")}}, tr("Flatten Image"));
    });

    // Layers-panel footer: the same actions.
    layers_->add_footer_action(nl);
    layers_->add_footer_action(ng);
    layers_->add_footer_action(add_mask);
    layers_->add_footer_menu(icon(QStringLiteral("adjust")), tr("New adjustment layer"), adj);
    layers_->add_footer_stretch();
    layers_->add_footer_action(md);
    layers_->add_footer_action(del);
    layers_->set_context_actions({action(QStringLiteral("rename_layer")), action(QStringLiteral("duplicate_layer")),
                                  action(QStringLiteral("delete_layer")), nullptr, action(QStringLiteral("edit_adjustment")),
                                  action(QStringLiteral("isolate_group")), action(QStringLiteral("select_alpha")), nullptr,
                                  action(QStringLiteral("merge_down"))});
    connect(layers_, &LayersPanel::edit_adjustment_requested, this, &MainWindow::edit_adjustment);
    connect(layers_, &LayersPanel::select_alpha_requested, this, &MainWindow::select_layer_alpha);

    // ---- Select ----
    QMenu* sel = mb->addMenu(tr("&Select"));
    add_op_action(sel, QStringLiteral("select_all"), tr("&All"), QKeySequence(tr("Ctrl+A")), "select_all",
                  [this] { apply({{"op", "select_all"}}, tr("Select All")); });
    add_op_action(sel, QStringLiteral("deselect"), tr("&Deselect"), QKeySequence(tr("Ctrl+D")), "deselect",
                  [this] { apply({{"op", "deselect"}}, tr("Deselect")); });
    add_op_action(sel, QStringLiteral("reselect"), tr("&Reselect"), QKeySequence(tr("Ctrl+Shift+D")), "reselect",
                  [this] { apply({{"op", "reselect"}}, tr("Reselect")); });
    add_op_action(sel, QStringLiteral("inverse"), tr("&Inverse"), QKeySequence(tr("Ctrl+Shift+I")), "select_inverse",
                  [this] { apply({{"op", "select_inverse"}}, tr("Select Inverse")); });
    sel->addSeparator();
    add_op_action(sel, QStringLiteral("feather"), tr("&Feather..."), QKeySequence(tr("Shift+F6")), "feather", [this] { feather_dialog(); });
    add_op_action(sel, QStringLiteral("expand"), tr("&Expand..."), {}, "expand", [this] { expand_contract_dialog(true); });
    add_op_action(sel, QStringLiteral("contract"), tr("&Contract..."), {}, "contract", [this] { expand_contract_dialog(false); });

    // ---- Filter ----
    QMenu* filt = mb->addMenu(tr("Fil&ter"));
    add_action(filt, QStringLiteral("repeat_filter"), tr("Repeat Last Filter"), QKeySequence(tr("Ctrl+F")), [this] { repeat_last_filter(); });
    filt->addSeparator();
    for (const FilterInfo& f : kFilters) {
        const QString key = QString::fromLatin1(f.key);
        add_op_action(filt, QStringLiteral("filter_") + key, tr(f.name) + QStringLiteral("..."), {}, f.op, [this, key] { filter_dialog(key); });
        if (key == QLatin1String("motion_blur") || key == QLatin1String("add_noise")) filt->addSeparator();
    }

    // ---- View ----
    QMenu* view = mb->addMenu(tr("&View"));
    QAction* zi = add_action(view, QStringLiteral("zoom_in"), tr("Zoom &In"), QKeySequence(tr("Ctrl+=")), [this] { canvas_->zoom_in(); });
    zi->setShortcuts({QKeySequence(tr("Ctrl+=")), QKeySequence(tr("Ctrl++"))});
    add_action(view, QStringLiteral("zoom_out"), tr("Zoom &Out"), QKeySequence(tr("Ctrl+-")), [this] { canvas_->zoom_out(); });
    add_action(view, QStringLiteral("fit"), tr("&Fit on Screen"), QKeySequence(tr("Ctrl+0")), [this] { canvas_->fit_to_view(); }, QStringLiteral("fit"));
    add_action(view, QStringLiteral("actual"), tr("&Actual Pixels (100 %)"), QKeySequence(tr("Ctrl+1")), [this] { canvas_->actual_pixels(); },
               QStringLiteral("actual"));

    // ---- Window ----
    window_menu_ = mb->addMenu(tr("&Window"));
    connect(window_menu_, &QMenu::aboutToShow, this, &MainWindow::rebuild_window_menu);
    add_action(nullptr, QStringLiteral("save_perspective"), tr("Save Perspective..."), {}, [this] { save_perspective(); });
    add_action(nullptr, QStringLiteral("delete_perspective"), tr("Delete Perspective..."), {}, [this] { delete_perspective(); });
    add_action(nullptr, QStringLiteral("reset_layout"), tr("Reset Layout"), {}, [this] {
        dock_->restoreState(default_layout_);
        for (auto* d : docks_) d->toggleView(true);
    });
    rebuild_window_menu();

    // ---- Help ----
    QMenu* help = mb->addMenu(tr("&Help"));
    add_action(help, QStringLiteral("diagnostics"), tr("&Input Diagnostics..."), QKeySequence(tr("Ctrl+Alt+Shift+D")), [this] { show_diagnostics(); },
               QStringLiteral("info"));
    help->addSeparator();
    add_action(help, QStringLiteral("about"), tr("&About Rasterloom"), {}, [this] {
        AboutDialog d(this);
        d.exec();
    });
    add_action(help, QStringLiteral("about_qt"), tr("About &Qt"), {}, [] { QApplication::aboutQt(); });
}

void MainWindow::rebuild_window_menu() {
    window_menu_->clear();
    for (const QString& k : {QStringLiteral("layers"), QStringLiteral("history"), QStringLiteral("color"), QStringLiteral("navigator")})
        if (auto* d = docks_.value(k)) window_menu_->addAction(d->toggleViewAction());
    window_menu_->addAction(options_->toggleViewAction());
    window_menu_->addAction(tools_bar_->toggleViewAction());
    window_menu_->addSeparator();
    QMenu* p = window_menu_->addMenu(tr("&Perspectives"));
    const QStringList names = dock_->perspectiveNames();
    if (names.isEmpty()) {
        QAction* none = p->addAction(tr("(none saved)"));
        none->setEnabled(false);
    }
    for (const QString& n : names) {
        QAction* a = p->addAction(n);
        connect(a, &QAction::triggered, this, [this, n] { dock_->openPerspective(n); });
    }
    window_menu_->addAction(actions_.value(QStringLiteral("save_perspective")));
    window_menu_->addAction(actions_.value(QStringLiteral("delete_perspective")));
    window_menu_->addAction(actions_.value(QStringLiteral("reset_layout")));
}

void MainWindow::build_status_bar() {
    QStatusBar* sb = statusBar();
    msg_label_ = new QLabel;
    msg_label_->setObjectName(QStringLiteral("StatusMessage"));
    sb->addWidget(msg_label_, 1);
    pos_label_ = new QLabel;
    size_label_ = new QLabel;
    zoom_label_ = new QLabel;
    backend_label_ = new QLabel(diag().canvas_backend);
    backend_label_->setToolTip(diag().canvas_reason);
    for (QLabel* l : {pos_label_, size_label_, zoom_label_, backend_label_}) sb->addPermanentWidget(l);
    color_swatch_ = new QLabel;
    color_swatch_->setFixedSize(14, 14);
    color_label_ = new QLabel;
    color_label_->setMinimumWidth(QFontMetrics(color_label_->font()).horizontalAdvance(QStringLiteral("255 255 255 255  #FFFFFF")) + 8);
    color_label_->setToolTip(tr("Composite colour under the cursor (R, G, B, A)"));
    sb->insertPermanentWidget(0, color_swatch_);
    sb->insertPermanentWidget(1, color_label_);
    pos_label_->setMinimumWidth(90);
    pos_label_->setToolTip(tr("Cursor position in image pixels"));
    size_label_->setToolTip(tr("Document size"));
    zoom_label_->setToolTip(tr("Zoom (Ctrl+wheel or Alt+wheel zooms at the cursor; Ctrl+0 fits, Ctrl+1 is 100 %)"));
    color_timer_.setSingleShot(true);
    color_timer_.setInterval(40);  // sample at most 25 times a second
    connect(&color_timer_, &QTimer::timeout, this, &MainWindow::update_color_under_cursor);
    connect(canvas_, &CanvasController::cursor_moved, this, [this](QPointF p, bool inside) {
        pos_label_->setText(inside ? QStringLiteral("X %1  Y %2").arg(static_cast<int>(std::floor(p.x()))).arg(static_cast<int>(std::floor(p.y())))
                                   : QString());
        cursor_c_ = p;
        cursor_inside_ = inside;
        if (!color_timer_.isActive()) color_timer_.start();
    });
    connect(canvas_, &CanvasController::view_changed, this, [this] {
        zoom_label_->setText(QStringLiteral("%1 %").arg(canvas_->zoom() * 100.0, 0, 'f', canvas_->zoom() < 0.1 ? 1 : 0));
    });
    connect(session_, &EditorSession::document_reset, this, [this] {
        if (session_->has_document())
            size_label_->setText(QStringLiteral("%1 × %2 px").arg(session_->doc().width()).arg(session_->doc().height()));
        cursor_inside_ = false;
        pos_label_->clear();
        color_label_->clear();
        color_swatch_->clear();
    });
}

void MainWindow::update_color_under_cursor() {
    if (!cursor_inside_ || !session_->has_document() || canvas_->stroke_active()) {
        if (!cursor_inside_) {
            color_label_->clear();
            color_swatch_->clear();
        }
        return;
    }
    const auto c = canvas_->composite_pixel(static_cast<int>(std::floor(cursor_c_.x())), static_cast<int>(std::floor(cursor_c_.y())));
    if (!c) return;
    color_label_->setText(QStringLiteral("%1 %2 %3 %4  #%5")
                              .arg(c->red(), 3)
                              .arg(c->green(), 3)
                              .arg(c->blue(), 3)
                              .arg(c->alpha(), 3)
                              .arg(QStringLiteral("%1%2%3")
                                       .arg(c->red(), 2, 16, QLatin1Char('0'))
                                       .arg(c->green(), 2, 16, QLatin1Char('0'))
                                       .arg(c->blue(), 2, 16, QLatin1Char('0'))
                                       .toUpper()));
    QPixmap pm(14, 14);
    pm.fill(QColor(c->red(), c->green(), c->blue()));
    {
        QPainter p(&pm);
        p.setPen(QColor(0, 0, 0, 140));
        p.drawRect(0, 0, 13, 13);
    }
    color_swatch_->setPixmap(pm);
}

// ---- helpers ---------------------------------------------------------------------------------------------

QAction* MainWindow::action(const QString& key) const { return actions_.value(key); }

void MainWindow::show_message(const QString& text, bool warning) {
    last_message_ = text;
    msg_label_->setText(text);
    msg_label_->setStyleSheet(warning ? QStringLiteral("color:#e8a33d;") : QStringLiteral("color:#a4a8b0;"));
    // Per window: a function-local static outlived the window that parented it, and the next
    // MainWindow in the process started a deleted timer.
    if (!msg_timer_) {
        msg_timer_ = new QTimer(this);
        msg_timer_->setSingleShot(true);
        connect(msg_timer_, &QTimer::timeout, this, [this] { msg_label_->clear(); });
    }
    msg_timer_->start(warning ? 9000 : 5000);
}

bool MainWindow::require_doc() { return session_->has_document(); }

bool MainWindow::require_raster(const QString& what) {
    const rl::Node* n = session_->active_node();
    if (n && n->is_raster()) return true;
    show_message(tr("%1 needs a raster layer: select one in the Layers panel.").arg(what), true);
    return false;
}

std::string MainWindow::active_parent() const {
    if (!session_->has_document() || session_->active_layer().empty()) return "root";
    rl::NodeRef r = session_->doc().find(session_->active_layer());
    if (!r.node) return "root";
    // A selected group receives new nodes inside it (at its top).
    if (r.node->is_group()) return r.node->id;
    return r.parent && r.parent->id != "root" ? r.parent->id : std::string("root");
}

void MainWindow::apply(const Json& op, const QString& label) {
    if (!require_doc()) return;
    session_->apply(op, label);
}

Json MainWindow::with_coverage(Json op) const {
    if (session_->has_document() && session_->state().selection.active()) op["coverage"] = {{"src", "selection"}};
    return op;
}

void MainWindow::run_op_dialog(ParamDialog& d, const std::function<Json()>& make_op, const QString& label) {
    const auto do_preview = [this, &d, make_op] {
        if (!d.preview_enabled()) {
            session_->cancel_preview();
            d.set_status(QString(), false);
            return;
        }
        const OpResult r = session_->preview(make_op());
        if (r.status == OpStatus::NotAvailable) d.set_status(tr("Not available in this build (%1). OK records nothing.").arg(to_q(r.message)), true);
        else if (!r.ok()) d.set_status(to_q(r.message), true);
        else d.set_status(QString(), false);
    };
    connect(&d, &ParamDialog::changed, &d, do_preview);
    QTimer::singleShot(0, &d, do_preview);
    if (d.exec() == QDialog::Accepted) session_->commit_preview(make_op(), label);
    else session_->cancel_preview();
}

void MainWindow::set_tool(Tool t) {
    tools_->set_tool(t);
    if (QAction* a = tool_actions_.value(t)) a->setChecked(true);
}

void MainWindow::cycle_tool(const QList<Tool>& group) {
    const int i = group.indexOf(tools_->tool());
    set_tool(i < 0 ? group.front() : group[(i + 1) % group.size()]);
}

void MainWindow::adjust_brush_size(double factor) {
    BrushSettings* b = tools_->brush_settings_for(tools_->tool());
    if (!b) return;
    b->size = std::clamp(std::round(b->size * factor * 10.0) / 10.0, 1.0, 5000.0);
    if (factor > 1.0 && b->size < 3) b->size += 1;
    tools_->notify_settings();
}

void MainWindow::update_title() {
    const QString name = session_->has_document() ? session_->display_name() : QString();
    setWindowTitle(QStringLiteral("%1%2 - Rasterloom %3").arg(name, session_->modified() ? QStringLiteral(" *") : QString(),
                                                                QString::fromLatin1(rl::kVersion)));
}

void MainWindow::update_actions() {
    if (QAction* a = action(QStringLiteral("undo"))) {
        a->setEnabled(session_->can_undo());
        a->setText(session_->can_undo() ? tr("&Undo %1").arg(session_->entries()[session_->applied_count() - 1].label) : tr("&Undo"));
    }
    if (QAction* a = action(QStringLiteral("redo"))) {
        a->setEnabled(session_->can_redo());
        a->setText(session_->can_redo() ? tr("&Redo %1").arg(session_->entries()[session_->applied_count()].label) : tr("&Redo"));
    }
    const rl::Node* n = session_->active_node();
    if (QAction* a = action(QStringLiteral("delete_layer"))) a->setEnabled(n != nullptr);
    if (QAction* a = action(QStringLiteral("duplicate_layer"))) a->setEnabled(n != nullptr);
    if (QAction* a = action(QStringLiteral("rename_layer"))) a->setEnabled(n != nullptr);
    if (QAction* a = action(QStringLiteral("layer_properties"))) a->setEnabled(n != nullptr);
    if (QAction* a = action(QStringLiteral("edit_adjustment"))) a->setEnabled(n && n->is_adjustment());
    if (QAction* a = action(QStringLiteral("select_alpha"))) a->setEnabled(n && n->is_raster());
    if (QAction* a = action(QStringLiteral("isolate_group"))) {
        a->setEnabled(n && n->is_group());
        a->setChecked(n && n->is_group() && n->mode != rl::BlendMode::Pass);
    }
    if (QAction* a = action(QStringLiteral("merge_down"))) a->setEnabled(n != nullptr);
    update_repeat_filter_action();
}

// ---- file --------------------------------------------------------------------------------------------------

bool MainWindow::maybe_save(const QString& action) {
    if (!session_->has_document() || !session_->modified() || !confirm_close_) return true;
    QMessageBox box(QMessageBox::Warning, action,
                    tr("Save the changes to \"%1\" before %2?").arg(session_->display_name(), action.toLower()),
                    QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, this);
    box.setInformativeText(tr("Your changes will be lost if you don't save them."));
    box.setDefaultButton(QMessageBox::Save);
    const int b = box.exec();
    if (b == QMessageBox::Cancel) return false;
    if (b == QMessageBox::Discard) return true;
    save();
    return !session_->modified();  // a cancelled or failed save keeps the document open
}

void MainWindow::show_warnings(const QString& title, const QString& intro, const QStringList& warnings) {
    if (warnings.isEmpty()) return;
    if (warn_dialog_) warn_dialog_->close();
    auto* d = new QDialog(this);
    d->setObjectName(QStringLiteral("FileWarnings"));
    d->setAttribute(Qt::WA_DeleteOnClose);
    d->setWindowTitle(title);
    auto* lay = new QVBoxLayout(d);
    auto* head = new QLabel(intro);
    head->setWordWrap(true);
    lay->addWidget(head);
    auto* list = new QListWidget;
    list->setObjectName(QStringLiteral("WarningList"));
    list->setWordWrap(true);
    list->setAlternatingRowColors(true);
    for (const QString& w : warnings) {
        auto* it = new QListWidgetItem(style()->standardIcon(QStyle::SP_MessageBoxWarning), w);
        it->setToolTip(w);
        list->addItem(it);
    }
    lay->addWidget(list, 1);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok);
    connect(bb, &QDialogButtonBox::accepted, d, &QDialog::close);
    lay->addWidget(bb);
    d->resize(620, std::min(520, 150 + 44 * static_cast<int>(warnings.size())));
    d->setModal(false);
    d->show();
    warn_dialog_ = d;
}

void MainWindow::report_error(const QString& title, const QString& text) {
    show_message(text.section(QLatin1Char('\n'), 0, 0), true);
    if (confirm_close_) QMessageBox::critical(this, title, text);
}

void MainWindow::new_document_dialog() {
    ParamDialog d(tr("New Document"),
                  {ParamSpec::i("w", tr("Width"), 1, rl::kMaxCanvasSide, 1600, tr(" px")),
                   ParamSpec::i("h", tr("Height"), 1, rl::kMaxCanvasSide, 1000, tr(" px")),
                   ParamSpec::e("bg", tr("Background"), {tr("White"), tr("Transparent"), tr("Background colour")},
                                {QStringLiteral("white"), QStringLiteral("transparent"), QStringLiteral("bgcolor")}, QStringLiteral("white"))},
                  false, this);
    if (d.exec() != QDialog::Accepted) return;
    const Json v = d.values();
    const std::string bg = v["bg"];
    if (!maybe_save(tr("New Document"))) return;
    session_->new_document(v["w"].get<int>(), v["h"].get<int>(), bg == "white" ? 0 : bg == "transparent" ? 1 : 2,
                           to_rgba8(tools_->bg()));
}

void MainWindow::new_document(int w, int h, int background) {
    session_->new_document(w, h, background, to_rgba8(tools_->bg()));
    update_title();
    update_actions();
    show_message(tr("New document: %1 x %2 px").arg(w).arg(h), false);  // replaces the previous file's status
}

void MainWindow::open_dialog() {
    QSettings s = settings();
    const QString dir = s.value(QStringLiteral("file/last_dir")).toString();
    const QString path = QFileDialog::getOpenFileName(this, tr("Open Image"), dir, open_filter());
    if (!path.isEmpty()) open_file(path);
}

bool MainWindow::open_file(const QString& path) {
    if (!maybe_save(tr("Open"))) return false;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QElapsedTimer t;
    t.start();
    LoadResult r = load_document(path, session_->has_document() ? session_->doc().history().depth() : 50);
    QApplication::restoreOverrideCursor();
    if (!r.doc) {
        report_error(tr("Open"), r.error);
        return false;
    }
    const QString name = QFileInfo(path).fileName();
    session_->adopt_document(std::move(r.doc), path, tr("Open %1").arg(name));
    last_open_ms_ = static_cast<double>(t.nsecsElapsed()) / 1e6;
    add_recent(path);
    {
        QSettings s = settings();
        s.setValue(QStringLiteral("file/last_dir"), QFileInfo(path).absolutePath());
    }
    update_title();
    update_actions();
    show_message(tr("Opened %1 (%2 x %3 px, %4) in %5 ms")
                     .arg(name)
                     .arg(session_->doc().width())
                     .arg(session_->doc().height())
                     .arg(r.format.toUpper())
                     .arg(last_open_ms_, 0, 'f', 0),
                 false);
    show_warnings(tr("Opened with notes"),
                  tr("<b>%1</b> opened, but some of its content was converted or is kept without being shown:").arg(name.toHtmlEscaped()),
                  r.warnings);
    return true;
}

bool MainWindow::save() {
    if (!require_doc()) return false;
    const QString fmt = format_of(session_->file_path());
    if (session_->file_path().isEmpty() || !is_native(fmt)) return save_as();
    return write_to(session_->file_path(), {}, true);
}

bool MainWindow::write_to(const QString& path, const SaveOptions& opt, bool becomes_document) {
    canvas_->commit_stroke_if_any();
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const SaveResult r = save_document(session_->state(), path, opt);
    QApplication::restoreOverrideCursor();
    if (!r.ok) {
        report_error(becomes_document ? tr("Save") : tr("Export"), r.error);
        return false;
    }
    if (becomes_document) {
        session_->set_file_path(path);
        session_->set_modified(false);
        update_title();
    }
    add_recent(path);
    show_message(tr("%1 %2 (%3 ms)")
                     .arg(becomes_document ? tr("Saved") : tr("Exported"), QFileInfo(path).fileName())
                     .arg(r.ms, 0, 'f', 0),
                 false);
    show_warnings(becomes_document ? tr("Saved with notes") : tr("Exported with notes"),
                  tr("<b>%1</b> was written, with these notes:").arg(QFileInfo(path).fileName().toHtmlEscaped()), r.warnings);
    return true;
}

QString MainWindow::suggested_path(const QString& ext) const {
    QSettings s = settings();
    const QString dir = session_->file_path().isEmpty() ? s.value(QStringLiteral("file/last_dir"), QDir::homePath()).toString()
                                                         : QFileInfo(session_->file_path()).absolutePath();
    const QString base = session_->file_path().isEmpty() ? tr("untitled") : QFileInfo(session_->file_path()).completeBaseName();
    return QDir(dir).filePath(base + QStringLiteral(".") + ext);
}

bool MainWindow::save_as() {
    if (!require_doc()) return false;
    QString selected;
    const QString ora_filter = tr("OpenRaster for GIMP / Krita (*.ora)");
    QString path = QFileDialog::getSaveFileName(this, tr("Save As"), suggested_path(QStringLiteral("orp")),
                                                tr("Rasterloom (*.orp)") + QStringLiteral(";;") + ora_filter, &selected);
    if (path.isEmpty()) return false;
    const bool ora = selected == ora_filter || format_of(path) == QLatin1String("ora");
    if (!is_native(format_of(path))) path += ora ? QStringLiteral(".ora") : QStringLiteral(".orp");
    return write_to(path, {}, true);
}

void MainWindow::export_as(const QString& format) {
    if (!require_doc()) return;
    struct Fmt {
        const char* key;
        const char* ext;
        const char* filter;
    };
    static const Fmt kFmts[] = {{"png", "png", "PNG (*.png)"},        {"jpeg", "jpg", "JPEG (*.jpg *.jpeg)"},
                                {"tiff", "tif", "TIFF (*.tif *.tiff)"}, {"psd", "psd", "Photoshop (*.psd);;Photoshop Large Document (*.psb)"},
                                {"ora", "ora", "OpenRaster (*.ora)"},  {"webp", "webp", "WebP (*.webp)"},
                                {"bmp", "bmp", "BMP (*.bmp)"}};
    const Fmt* f = nullptr;
    for (const Fmt& x : kFmts)
        if (format == QLatin1String(x.key)) f = &x;
    if (!f) return;
    SaveOptions opt;
    QSettings s = settings();
    if (format == QLatin1String("jpeg")) {
        ParamDialog d(tr("Export JPEG"),
                      {ParamSpec::i("quality", tr("Quality"), 1, 100, s.value(QStringLiteral("export/jpeg_quality"), 92).toInt())}, false,
                      this);
        d.add_note(tr("JPEG has no transparency: the image is flattened onto white. 90-95 keeps detail; below 70 shows blocks."));
        if (d.exec() != QDialog::Accepted) return;
        opt.jpeg_quality = d.values()["quality"].get<int>();
        s.setValue(QStringLiteral("export/jpeg_quality"), opt.jpeg_quality);
    }
    QString selected;
    QString path = QFileDialog::getSaveFileName(this, tr("Export %1").arg(format == QLatin1String("psd") ? QStringLiteral("PSD / PSB") : format.toUpper()),
                                                suggested_path(QString::fromLatin1(f->ext)), tr(f->filter), &selected);
    if (path.isEmpty()) return;
    if (format_of(path) != format) path += QStringLiteral(".") + QString::fromLatin1(f->ext);
    if (format == QLatin1String("psd")) opt.force_psb = path.endsWith(QLatin1String(".psb"), Qt::CaseInsensitive) || selected.contains(QLatin1String("psb"));
    if (opt.force_psb && !path.endsWith(QLatin1String(".psb"), Qt::CaseInsensitive)) path = path.left(path.size() - 4) + QStringLiteral(".psb");
    write_to(path, opt, false);
}

void MainWindow::export_session_script() {
    if (!require_doc()) return;
    const std::optional<Json> s = session_->session_script();
    if (!s) {
        show_message(tr("A session script needs a document started with File > New (opened files have no script form yet)."), true);
        return;
    }
    const QString path = QFileDialog::getSaveFileName(this, tr("Export Render Script"), suggested_path(QStringLiteral("json")),
                                                      tr("Render script (*.json)"));
    if (path.isEmpty()) return;
    QSaveFile f(path);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QByteArray::fromStdString(s->dump(1)));
        if (f.commit()) {
            show_message(tr("Wrote %1: replay it with rasterloom-cli --render-script").arg(QFileInfo(path).fileName()), false);
            return;
        }
    }
    report_error(tr("Export Render Script"), tr("Could not write %1: %2").arg(QFileInfo(path).fileName(), f.errorString()));
}

void MainWindow::add_recent(const QString& path) {
    QSettings s = settings();
    QStringList r = s.value(QStringLiteral("recent")).toStringList();
    const QString abs = QFileInfo(path).absoluteFilePath();
    r.removeAll(abs);
    r.prepend(abs);
    while (r.size() > 10) r.removeLast();
    s.setValue(QStringLiteral("recent"), r);
}

void MainWindow::rebuild_recent_menu() {
    recent_menu_->clear();
    QSettings s = settings();
    const QStringList r = s.value(QStringLiteral("recent")).toStringList();
    if (r.isEmpty()) recent_menu_->addAction(tr("(empty)"))->setEnabled(false);
    int n = 0;
    for (const QString& p : r) {
        ++n;
        QAction* a = recent_menu_->addAction(QStringLiteral("&%1  %2").arg(n % 10).arg(QFileInfo(p).fileName()));
        a->setToolTip(p);
        a->setStatusTip(p);
        a->setEnabled(QFileInfo::exists(p));
        connect(a, &QAction::triggered, this, [this, p] { open_file(p); });
    }
    recent_menu_->addSeparator();
    recent_menu_->addAction(tr("Clear Recent"), this, [] {
        QSettings s2 = settings();
        s2.remove(QStringLiteral("recent"));
    });
}

void MainWindow::dragEnterEvent(QDragEnterEvent* e) {
    // Accept every drop that carries files, links or image data: what cannot be used is explained
    // in the status bar on drop, instead of a silent "no entry" cursor.
    const QMimeData* md = e->mimeData();
    bool image_data = md->hasImage();
    for (const QString& f : md->formats()) image_data = image_data || f.startsWith(QLatin1String("image/"));
    if (md->hasUrls() || image_data) e->acceptProposedAction();
    else e->ignore();
}

void MainWindow::dropEvent(QDropEvent* e) {
    // Photoshop: files dropped on an open document are placed as layers; with no document the first
    // opens and the rest are placed into it; image data (from a browser) is placed too.
    const QMimeData* md = e->mimeData();
    QStringList files, remote;
    for (const QUrl& u : md->urls()) {
        if (u.isLocalFile()) files << u.toLocalFile();
        else remote << u.toString();
    }
    std::optional<rl::io::RgbaBuffer> data;
    if (files.isEmpty())
        if (const std::optional<ClipboardImage> ci = image_from_mime(md); ci && ci->img.w > 0) data = ci->img;
    e->acceptProposedAction();
    // Handle after the drop returns (a modal prompt inside a drop handler confuses some compositors).
    QTimer::singleShot(0, this, [this, files, remote, data] { handle_drop(files, remote, data); });
}

// ---- layer actions ---------------------------------------------------------------------------------------------

void MainWindow::new_layer() {
    if (!require_doc()) return;
    const std::string id = session_->unique_id("Layer");
    if (session_->apply({{"op", "add_layer"}, {"id", id}, {"parent", active_parent()}}, tr("New Layer")).ok())
        session_->set_active_layer(id);
}

void MainWindow::new_group() {
    if (!require_doc()) return;
    const std::string id = session_->unique_id("Group");
    if (session_->apply({{"op", "add_group"}, {"id", id}, {"parent", active_parent()}, {"mode", "pass"}}, tr("New Group")).ok())
        session_->set_active_layer(id);
}

void MainWindow::delete_layer() {
    if (!require_doc() || !session_->active_node()) return;
    const QString shown = layer_display_name(*session_->active_node());
    session_->apply({{"op", "delete_layer"}, {"layer", session_->active_layer()}}, tr("Delete %1").arg(shown));
}

void MainWindow::duplicate_layer() {
    if (!require_doc() || !session_->active_node()) return;
    const rl::Node& src = *session_->active_node();
    // Ctrl+J with a selection on a raster layer is Layer via Copy (Photoshop).
    if (src.is_raster() && session_->state().selection.active()) {
        layer_via(false);
        return;
    }
    // doc 60 §4.3: the copy's descendants get "<new id>/<their id>"; every derived id must be free.
    std::vector<std::string> sub;
    std::function<void(const rl::Node&)> collect = [&](const rl::Node& n) {
        for (const rl::Node& c : n.children) {
            sub.push_back(c.id);
            collect(c);
        }
    };
    collect(src);
    std::string id;
    for (int k = 1;; ++k) {
        id = src.id + (k == 1 ? std::string(" copy") : " copy " + std::to_string(k));
        bool free = !session_->doc().id_exists(id);
        for (size_t i = 0; free && i < sub.size(); ++i) free = !session_->doc().id_exists(id + "/" + sub[i]);
        if (free) break;
    }
    if (session_->apply({{"op", "duplicate_layer"}, {"layer", src.id}, {"id", id}}, tr("Duplicate Layer")).ok())
        session_->set_active_layer(id);
}

void MainWindow::begin_rename() {
    if (!require_doc() || !session_->active_node()) return;
    layers_->begin_rename(session_->active_layer());
}

void MainWindow::select_layer_alpha(const std::string& id, Qt::KeyboardModifiers mods) {
    const bool shift = mods & Qt::ShiftModifier, alt = mods & Qt::AltModifier;
    const char* mode = shift && alt ? "intersect" : shift ? "add" : alt ? "subtract" : "new";
    apply({{"op", "select_alpha"}, {"layer", id}, {"mode", mode}}, tr("Select Layer Pixels"));
}

bool MainWindow::run_adjustment_dialog(const QString& type, const QString& title, const Json* initial,
                                       const std::function<Json(const Json&)>& op_for, const QString& label) {
    const auto num = [&](const char* k, double def) {
        return initial && initial->contains(k) && (*initial)[k].is_number() ? (*initial)[k].get<double>() : def;
    };
    const size_t before = session_->applied_count();
    if (type == QLatin1String("levels")) {
        LevelsDialog d(this);
        d.setWindowTitle(title);
        if (initial) d.set_params(*initial);
        run_op_dialog(d, [&] { return op_for(d.params()); }, label);
        return session_->applied_count() != before;
    }
    if (type == QLatin1String("curves")) {
        CurvesDialog d(this);
        d.setWindowTitle(title);
        if (initial) d.set_params(*initial);
        run_op_dialog(d, [&] { return op_for(d.params()); }, label);
        return session_->applied_count() != before;
    }
    std::vector<ParamSpec> specs;
    const bool colorize = initial && initial->value("colorize", false);
    if (type == QLatin1String("brightness_contrast")) {
        specs = {ParamSpec::d("brightness", tr("Brightness"), -1.0, 1.0, num("brightness", 0.0), 2),
                 ParamSpec::d("contrast", tr("Contrast"), -1.0, 1.0, num("contrast", 0.0), 2)};
    } else if (type == QLatin1String("hue_saturation")) {
        specs = {ParamSpec::b("colorize", tr("Colorize"), colorize),
                 ParamSpec::d("hue", tr("Hue"), colorize ? 0.0 : -180.0, colorize ? 360.0 : 180.0, num("hue", 0.0), 1, QStringLiteral("°")),
                 ParamSpec::d("saturation", tr("Saturation"), colorize ? 0.0 : -100.0, 100.0, num("saturation", colorize ? 25.0 : 0.0), 1),
                 ParamSpec::d("lightness", tr("Lightness"), -100.0, 100.0, num("lightness", 0.0), 1)};
    } else if (type == QLatin1String("black_white")) {
        QColor tint(0xe1, 0xd3, 0xb3);
        bool tint_on = false;
        if (initial && initial->contains("tint") && (*initial)["tint"].is_string()) {
            tint = QColor(to_q((*initial)["tint"].get<std::string>()));
            tint_on = tint.isValid();
        }
        specs = {ParamSpec::d("reds", tr("Reds"), -200, 300, num("reds", 40), 1, QStringLiteral(" %")),
                 ParamSpec::d("yellows", tr("Yellows"), -200, 300, num("yellows", 60), 1, QStringLiteral(" %")),
                 ParamSpec::d("greens", tr("Greens"), -200, 300, num("greens", 40), 1, QStringLiteral(" %")),
                 ParamSpec::d("cyans", tr("Cyans"), -200, 300, num("cyans", 60), 1, QStringLiteral(" %")),
                 ParamSpec::d("blues", tr("Blues"), -200, 300, num("blues", 20), 1, QStringLiteral(" %")),
                 ParamSpec::d("magentas", tr("Magentas"), -200, 300, num("magentas", 80), 1, QStringLiteral(" %")),
                 ParamSpec::oc("tint", tr("Tint"), tint.isValid() ? tint : QColor(0xe1, 0xd3, 0xb3), tint_on)};
    } else if (type == QLatin1String("posterize")) {
        specs = {ParamSpec::i("levels", tr("Levels"), 2, 255, static_cast<int>(num("levels", 4)))};
    } else if (type == QLatin1String("threshold")) {
        specs = {ParamSpec::i("level", tr("Threshold level"), 1, 255, static_cast<int>(num("level", 128)))};
    } else {
        return false;
    }
    ParamDialog d(title, specs, true, this);
    if (type == QLatin1String("hue_saturation")) {
        // doc 20 A4: the ranges depend on colorize (switching colorize on starts saturation at 25).
        auto* col = qobject_cast<QCheckBox*>(d.field("colorize"));
        auto* hue = qobject_cast<QDoubleSpinBox*>(d.field("hue"));
        auto* sat = qobject_cast<QDoubleSpinBox*>(d.field("saturation"));
        QObject::connect(col, &QCheckBox::toggled, &d, [hue, sat](bool on) {
            if (on) {
                hue->setRange(0.0, 360.0);
                sat->setRange(0.0, 100.0);
                sat->setValue(25.0);
            } else {
                hue->setRange(-180.0, 180.0);
                sat->setRange(-100.0, 100.0);
                sat->setValue(0.0);
            }
        });
    }
    run_op_dialog(d, [&] { return op_for(d.values()); }, label);
    return session_->applied_count() != before;
}

void MainWindow::new_adjustment(const QString& type) {
    if (!require_doc()) return;
    QString name;
    for (const AdjInfo& a : kAdjustments)
        if (type == QLatin1String(a.type)) name = tr(a.name);
    const std::string id = session_->unique_id(to_std(name));
    const std::string parent = active_parent();
    const auto op_for = [&](const Json& params) {
        return Json{{"op", "add_adjustment"}, {"id", id}, {"parent", parent}, {"type", to_std(type)}, {"params", params}};
    };
    const QString label = tr("New %1 Layer").arg(name);
    if (type == QLatin1String("invert")) {
        if (session_->apply(op_for(Json::object()), label).ok()) session_->set_active_layer(id);
        return;
    }
    run_adjustment_dialog(type, tr("New %1 Layer").arg(name), nullptr, op_for, label);
    if (session_->doc().id_exists(id)) session_->set_active_layer(id);
}

void MainWindow::edit_adjustment(const std::string& id) {
    if (!require_doc()) return;
    const rl::Node* n = nullptr;
    if (rl::NodeRef r = session_->doc().find(id); r.node) n = r.node;
    if (!n || !n->is_adjustment() || !n->adjustment) return;
    const QString type = QString::fromLatin1(n->adjustment->type());
    const QString shown = layer_display_name(*n);
    if (!rl::adjust::is_known_type(to_std(type))) {
        show_message(tr("'%1' is an adjustment kept from the file (%2): Rasterloom preserves it on save but cannot show or edit it.")
                         .arg(shown, type),
                     true);
        return;
    }
    if (type == QLatin1String("invert")) {
        show_message(tr("Invert has no settings to edit."), false);
        return;
    }
    const Json initial = rl::adjust::params_to_json(n->adjustment->params());
    QString name = type;
    for (const AdjInfo& a : kAdjustments)
        if (type == QLatin1String(a.type)) name = tr(a.name);
    const auto op_for = [&](const Json& params) { return Json{{"op", "set_adjustment"}, {"layer", id}, {"params", params}}; };
    run_adjustment_dialog(type, tr("%1 - %2").arg(name, shown), &initial, op_for, tr("Edit %1").arg(name));
}

// ---- edit / image / select dialogs ---------------------------------------------------------------------------------

void MainWindow::fill_dialog() {
    if (!require_doc() || !require_raster(tr("Fill"))) return;
    ParamDialog d(tr("Fill"),
                  {ParamSpec::e("use", tr("Contents"), {tr("Foreground colour"), tr("Background colour"), tr("Black"), tr("50 % grey"), tr("White")},
                                {QStringLiteral("fg"), QStringLiteral("bg"), QStringLiteral("black"), QStringLiteral("grey"), QStringLiteral("white")},
                                QStringLiteral("fg")),
                   ParamSpec::i("opacity", tr("Opacity"), 0, 100, 100, QStringLiteral(" %"))},
                  true, this);
    d.add_note(session_->state().selection.active() ? tr("Fills the selection on the active layer.")
                                                    : tr("No selection: fills the whole active layer."));
    const std::string layer = session_->active_layer();
    run_op_dialog(
        d,
        [&] {
            const Json v = d.values();
            const std::string use = v["use"];
            QColor c = use == "fg" ? tools_->fg() : use == "bg" ? tools_->bg() : use == "black" ? QColor(Qt::black)
                                                  : use == "grey" ? QColor(128, 128, 128) : QColor(Qt::white);
            return Json{{"op", "fill_selection"}, {"layer", layer}, {"color", hex(c)}, {"opacity", v["opacity"].get<int>() / 100.0}};
        },
        tr("Fill"));
}

void MainWindow::image_size_dialog() {
    if (!require_doc()) return;
    const int W = session_->doc().width(), H = session_->doc().height();
    ParamDialog d(tr("Image Size"),
                  {ParamSpec::i("w", tr("Width"), 1, rl::kMaxCanvasSide, W, tr(" px")), ParamSpec::i("h", tr("Height"), 1, rl::kMaxCanvasSide, H, tr(" px")),
                   ParamSpec::b("keep", tr("Constrain proportions"), true),
                   ParamSpec::e("interp", tr("Resample"), {tr("Bicubic"), tr("Nearest neighbour")}, {QStringLiteral("bicubic"), QStringLiteral("nearest")},
                                QStringLiteral("bicubic"))},
                  false, this);
    auto* ws = qobject_cast<QSpinBox*>(d.field("w"));
    auto* hs = qobject_cast<QSpinBox*>(d.field("h"));
    auto* keep = qobject_cast<QCheckBox*>(d.field("keep"));
    bool guard = false;
    connect(ws, qOverload<int>(&QSpinBox::valueChanged), &d, [&](int v) {
        if (guard || !keep->isChecked()) return;
        guard = true;
        hs->setValue(std::max(1, static_cast<int>(std::lround(static_cast<double>(v) * H / W))));
        guard = false;
    });
    connect(hs, qOverload<int>(&QSpinBox::valueChanged), &d, [&](int v) {
        if (guard || !keep->isChecked()) return;
        guard = true;
        ws->setValue(std::max(1, static_cast<int>(std::lround(static_cast<double>(v) * W / H))));
        guard = false;
    });
    if (d.exec() != QDialog::Accepted) return;
    const Json v = d.values();
    apply({{"op", "image_size"}, {"w", v["w"]}, {"h", v["h"]}, {"interp", v["interp"]}}, tr("Image Size"));
}

void MainWindow::canvas_size_dialog() {
    if (!require_doc()) return;
    ParamDialog d(tr("Canvas Size"),
                  {ParamSpec::i("w", tr("Width"), 1, rl::kMaxCanvasSide, session_->doc().width(), tr(" px")),
                   ParamSpec::i("h", tr("Height"), 1, rl::kMaxCanvasSide, session_->doc().height(), tr(" px")), ParamSpec::anchor("anchor", tr("Anchor"))},
                  false, this);
    if (d.exec() != QDialog::Accepted) return;
    const Json v = d.values();
    apply({{"op", "canvas_size"}, {"w", v["w"]}, {"h", v["h"]}, {"anchor", v["anchor"]}}, tr("Canvas Size"));
}

void MainWindow::rotate_arbitrary_dialog() {
    if (!require_doc()) return;
    ParamDialog d(tr("Rotate Canvas"), {ParamSpec::d("angle", tr("Angle (clockwise)"), -3600.0, 3600.0, 15.0, 2, QStringLiteral("°"))}, false, this);
    if (d.exec() != QDialog::Accepted) return;
    const Json v = d.values();
    apply({{"op", "rotate_canvas"}, {"angle", v["angle"]}}, tr("Rotate Canvas %1°").arg(v["angle"].get<double>()));
}

void MainWindow::crop_action() {
    if (!require_doc()) return;
    if (canvas_->has_pending() && !canvas_->crop_rect().isEmpty()) return canvas_->commit_pending();
    const rl::Selection& sel = session_->state().selection;
    if (!sel.active()) {
        show_message(tr("Crop: make a selection or drag a rectangle with the Crop tool (C) first."), true);
        return;
    }
    // Bounding box of the selection (display reading of the mask, not image math).
    int x0 = INT32_MAX, y0 = INT32_MAX, x1 = -1, y1 = -1;
    const int W = session_->doc().width(), H = session_->doc().height();
    for (int ty = 0; ty < sel.mask.tiles_y(); ++ty)
        for (int tx = 0; tx < sel.mask.tiles_x(); ++tx) {
            if (sel.mask.is_absent(tx, ty) && sel.mask.background() == 0) continue;
            const rl::GrayTile& t = sel.mask.tile(tx, ty);
            for (int ly = 0; ly < rl::kTileSize && ty * rl::kTileSize + ly < H; ++ly)
                for (int lx = 0; lx < rl::kTileSize && tx * rl::kTileSize + lx < W; ++lx)
                    if (t.at(lx, ly)) {
                        x0 = std::min(x0, tx * rl::kTileSize + lx);
                        y0 = std::min(y0, ty * rl::kTileSize + ly);
                        x1 = std::max(x1, tx * rl::kTileSize + lx);
                        y1 = std::max(y1, ty * rl::kTileSize + ly);
                    }
        }
    if (x1 < 0) return;
    apply({{"op", "crop"}, {"x", x0}, {"y", y0}, {"w", x1 - x0 + 1}, {"h", y1 - y0 + 1}}, tr("Crop to Selection"));
}

void MainWindow::feather_dialog() {
    if (!require_doc()) return;
    ParamDialog d(tr("Feather Selection"), {ParamSpec::d("radius", tr("Radius"), 0.0, 250.0, 5.0, 1, tr(" px"))}, true, this);
    run_op_dialog(d, [&] { return Json{{"op", "feather"}, {"radius", d.values()["radius"]}}; }, tr("Feather"));
}

void MainWindow::expand_contract_dialog(bool expand) {
    if (!require_doc()) return;
    ParamDialog d(expand ? tr("Expand Selection") : tr("Contract Selection"), {ParamSpec::i("by", tr("By"), 1, 100, 4, tr(" px"))}, true, this);
    run_op_dialog(d, [&] { return Json{{"op", expand ? "expand" : "contract"}, {"by", d.values()["by"]}}; },
                  expand ? tr("Expand Selection") : tr("Contract Selection"));
}

void MainWindow::filter_dialog(const QString& key) {
    if (!require_doc() || !require_raster(tr("Filters"))) return;
    const FilterInfo* fi = nullptr;
    for (const FilterInfo& f : kFilters)
        if (key == QLatin1String(f.key)) fi = &f;
    if (!fi) return;
    std::vector<ParamSpec> specs;
    if (key == QLatin1String("gaussian_blur")) {
        specs = {ParamSpec::d("radius", tr("Radius (sigma)"), 0.1, 250.0, 3.0, 1, tr(" px")),
                 ParamSpec::e("edge", tr("Edges"), kEdgeLabels, kEdgeValues, QStringLiteral("clamp"))};
    } else if (key == QLatin1String("motion_blur")) {
        specs = {ParamSpec::d("angle", tr("Angle"), -360.0, 360.0, 0.0, 1, QStringLiteral("°")),
                 ParamSpec::d("distance", tr("Distance"), 1.0, 2000.0, 10.0, 1, tr(" px")),
                 ParamSpec::e("edge", tr("Edges"), kEdgeLabels, kEdgeValues, QStringLiteral("clamp"))};
    } else if (key == QLatin1String("unsharp_mask")) {
        specs = {ParamSpec::d("amount", tr("Amount"), 1.0, 500.0, 50.0, 0, QStringLiteral(" %")),
                 ParamSpec::d("radius", tr("Radius"), 0.1, 250.0, 1.0, 1, tr(" px")), ParamSpec::i("threshold", tr("Threshold"), 0, 255, 0, tr(" levels")),
                 ParamSpec::e("edge", tr("Edges"), kEdgeLabels, kEdgeValues, QStringLiteral("clamp"))};
    } else if (key == QLatin1String("add_noise")) {
        specs = {ParamSpec::d("amount", tr("Amount"), 0.0, 400.0, 10.0, 1, QStringLiteral(" %")),
                 ParamSpec::e("distribution", tr("Distribution"), {tr("Uniform"), tr("Gaussian")},
                              {QStringLiteral("uniform"), QStringLiteral("gaussian")}, QStringLiteral("uniform")),
                 ParamSpec::b("monochromatic", tr("Monochromatic"), false), ParamSpec::i("seed", tr("Seed"), 0, 2147483647, 0)};
    } else if (key == QLatin1String("high_pass")) {
        specs = {ParamSpec::d("radius", tr("Radius"), 0.1, 250.0, 10.0, 1, tr(" px")),
                 ParamSpec::e("edge", tr("Edges"), kEdgeLabels, kEdgeValues, QStringLiteral("clamp"))};
    } else if (key == QLatin1String("offset")) {
        const int W = session_->doc().width(), H = session_->doc().height();
        specs = {ParamSpec::i("dx", tr("Horizontal"), -65536, 65536, W / 4, tr(" px")),
                 ParamSpec::i("dy", tr("Vertical"), -65536, 65536, H / 4, tr(" px")),
                 ParamSpec::e("mode", tr("Undefined areas"), {tr("Transparent"), tr("Repeat edge pixels"), tr("Wrap around")},
                              {QStringLiteral("transparent"), QStringLiteral("repeat"), QStringLiteral("wrap")}, QStringLiteral("wrap"))};
    }
    ParamDialog d(tr(fi->name), specs, true, this);
    if (session_->state().selection.active()) d.add_note(tr("Applies inside the active selection."));
    const std::string layer = session_->active_layer();
    const std::string op = fi->op;
    const size_t before = session_->applied_count();
    run_op_dialog(
        d,
        [&] {
            Json j = d.values();
            j["op"] = op;
            j["layer"] = layer;
            return with_coverage(j);
        },
        tr(fi->name));
    if (session_->applied_count() != before) {
        last_filter_key_ = key;
        last_filter_values_ = d.values();
        update_repeat_filter_action();
    }
}

void MainWindow::update_repeat_filter_action() {
    QAction* a = action(QStringLiteral("repeat_filter"));
    if (!a) return;
    const FilterInfo* fi = nullptr;
    for (const FilterInfo& f : kFilters)
        if (last_filter_key_ == QLatin1String(f.key)) fi = &f;
    a->setEnabled(fi && session_->has_document());
    a->setText(fi ? tr("Repeat %1").arg(tr(fi->name)) : tr("Repeat Last Filter"));
    a->setStatusTip(fi ? tr("Apply %1 again with the same settings to the active layer (inside the selection, if any)").arg(tr(fi->name))
                       : tr("No filter applied yet"));
}

void MainWindow::repeat_last_filter() {
    if (last_filter_key_.isEmpty() || !require_doc() || !require_raster(tr("Filters"))) return;
    const FilterInfo* fi = nullptr;
    for (const FilterInfo& f : kFilters)
        if (last_filter_key_ == QLatin1String(f.key)) fi = &f;
    if (!fi) return;
    Json j = last_filter_values_;
    j["op"] = fi->op;
    j["layer"] = session_->active_layer();
    apply(with_coverage(j), tr(fi->name));
}

void MainWindow::preferences_dialog() {
    QSettings s = settings();
    ParamDialog d(tr("Preferences"),
                  {ParamSpec::i("depth", tr("History states"), 1, 1000, static_cast<int>(session_->has_document() ? session_->doc().history().depth() : 50)),
                   ParamSpec::e("canvas", tr("Canvas backend (restart)"), {tr("Automatic"), tr("OpenGL"), tr("Raster (software)")},
                                {QStringLiteral("auto"), QStringLiteral("gl"), QStringLiteral("raster")},
                                s.value(QStringLiteral("canvas/backend"), QStringLiteral("auto")).toString()),
                   ParamSpec::e("platform", tr("Window system (restart)"), {tr("X11 / XWayland (default)"), tr("Native Wayland (experimental)")},
                                {QStringLiteral("xcb"), QStringLiteral("wayland")}, s.value(QStringLiteral("platform"), QStringLiteral("xcb")).toString())},
                  false, this);
    d.add_note(tr("Native Wayland loses window decorations on some desktops and global cursor positions; X11 is the tested default."));
    if (d.exec() != QDialog::Accepted) return;
    const Json v = d.values();
    s.setValue(QStringLiteral("history/depth"), v["depth"].get<int>());
    s.setValue(QStringLiteral("canvas/backend"), to_q(v["canvas"].get<std::string>()));
    s.setValue(QStringLiteral("platform"), to_q(v["platform"].get<std::string>()));
    session_->set_history_depth(static_cast<size_t>(v["depth"].get<int>()));
}

void MainWindow::show_diagnostics() {
    if (!diag_dialog_) {
        diag_dialog_ = new InputDiagnosticsDialog(this);
        diag_dialog_->setAttribute(Qt::WA_DeleteOnClose);
    }
    diag_dialog_->show();
    diag_dialog_->raise();
}

void MainWindow::save_perspective() {
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Save Perspective"), tr("Name:"), QLineEdit::Normal, tr("My Layout"), &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    dock_->addPerspective(name);
    QSettings s = settings();
    dock_->savePerspectives(s);
    show_message(tr("Saved perspective '%1'").arg(name), false);
}

void MainWindow::delete_perspective() {
    const QStringList names = dock_->perspectiveNames();
    if (names.isEmpty()) return show_message(tr("No saved perspectives."), false);
    bool ok = false;
    const QString name = QInputDialog::getItem(this, tr("Delete Perspective"), tr("Perspective:"), names, 0, false, &ok);
    if (!ok) return;
    dock_->removePerspective(name);
    QSettings s = settings();
    dock_->savePerspectives(s);
}

bool MainWindow::eventFilter(QObject* o, QEvent* e) {
    if (e->type() == QEvent::TabletLeaveProximity && o == qApp) {
        if (canvas_->stroke_active()) canvas_->focus_lost();
        canvas_->set_tablet_down(false);
        diag().last_source = QStringLiteral("tablet, left proximity");
    }
    return QMainWindow::eventFilter(o, e);
}

void MainWindow::closeEvent(QCloseEvent* e) {
    canvas_->commit_stroke_if_any();
    if (!maybe_save(tr("Quit"))) {
        e->ignore();
        return;
    }
    QSettings s = settings();
    s.setValue(QStringLiteral("layout/dock"), dock_->saveState());
    s.setValue(QStringLiteral("layout/window"), saveGeometry());
    dock_->savePerspectives(s);
    QMainWindow::closeEvent(e);
}

}  // namespace rl::gui
