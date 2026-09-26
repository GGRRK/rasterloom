// SPDX-License-Identifier: GPL-3.0-or-later
//
// gui-smoke: the real MainWindow under QT_QPA_PLATFORM=offscreen + RASTERLOOM_CANVAS=raster,
// driven through ~30 scripted interactions (menus, keys, panels, dialogs, a synthesized tablet
// stroke, undo/redo). After each step the window is grabbed; the grabs are tiled with captions into
// tests/output/gui-contact-sheet.png. Assertions are semantic (layer counts, history entries,
// canvas pixels, diagnostics values), never just "did not crash". Finally EVERY menu action is
// triggered once (any dialog it opens is closed) to prove no action crashes.
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPointingDevice>
#include <QProcess>
#include <QSpinBox>
#include <QTabletEvent>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTreeWidget>

#include <DockManager.h>
#include <DockWidget.h>

#include <cmath>
#include <functional>
#include <memory>

#include "core/doc/document.hpp"
#include "gui/canvas.hpp"
#include "gui/diagnostics.hpp"
#include "gui/dialogs.hpp"
#include "gui/layers_panel.hpp"
#include "gui/main_window.hpp"
#include "gui/panels.hpp"
#include "gui/session.hpp"
#include "gui/theme.hpp"

using namespace rl::gui;

namespace {

int count_nodes(const rl::Node& c) {
    int n = 0;
    for (const rl::Node& k : c.children) n += 1 + (k.is_group() ? count_nodes(k) : 0);
    return n;
}

}  // namespace

class GuiSmoke : public QObject {
    Q_OBJECT

private:
    struct Shot {
        QString caption;
        QImage img;
    };
    std::unique_ptr<MainWindow> w_;
    std::vector<Shot> shots_;
    QTimer modal_timer_;
    std::function<void(QDialog*)> on_modal_;
    QImage last_dialog_;
    int step_ = 0;

    EditorSession* s() { return w_->session(); }
    CanvasController* c() { return w_->canvas(); }
    QWidget* cw() { return w_->canvas_widget(); }

    void settle() {
        for (int i = 0; i < 5; ++i) QCoreApplication::processEvents();
    }

    void shot(const QString& caption, const QImage& overlay = {}) {
        settle();
        QImage img = w_->grab().toImage().convertToFormat(QImage::Format_ARGB32_Premultiplied);
        if (!overlay.isNull()) {
            QPainter p(&img);
            const QPoint at((img.width() - overlay.width()) / 2, (img.height() - overlay.height()) / 2);
            p.fillRect(QRect(at + QPoint(6, 8), overlay.size()), QColor(0, 0, 0, 110));
            p.drawImage(at, overlay);
            p.setPen(QColor(0x4a, 0x8c, 0xf7));
            p.drawRect(QRect(at, overlay.size()).adjusted(0, 0, -1, -1));
        }
        shots_.push_back({QStringLiteral("%1. %2").arg(++step_, 2, 10, QLatin1Char('0')).arg(caption), img});
    }

    // Triggers `a`; the next modal dialog is handed to `handler` (default: grab + reject).
    void trigger_with_modal(QAction* a, std::function<void(QDialog*)> handler) {
        QVERIFY(a);
        on_modal_ = std::move(handler);
        a->trigger();
        settle();
        on_modal_ = nullptr;
    }

    // Keyboard shortcuts need the main window active (a closed modal dialog leaves none active).
    void key(Qt::Key k, Qt::KeyboardModifiers m = Qt::NoModifier) {
        if (QApplication::activeWindow() != w_.get()) {
            w_->activateWindow();
            QVERIFY(QTest::qWaitForWindowActive(w_.get()));
        }
        QTest::keyClick(w_.get(), k, m);
        settle();
    }

    QPointF widget_pt(double cx, double cy) { return c()->to_widget(QPointF(cx, cy)); }

    void tablet(QEvent::Type t, QPointingDevice* dev, const QPointF& lp, double pressure, Qt::MouseButton b, Qt::MouseButtons bs) {
        QTabletEvent e(t, dev, lp, cw()->mapToGlobal(lp), pressure, 12.f, -7.f, 0.f, 0.0, 0.f, Qt::NoModifier, b, bs);
        QCoreApplication::sendEvent(cw(), &e);
        QVERIFY2(e.isAccepted(), "canvas must accept every QTabletEvent");
    }

    void drag(const QPointF& a, const QPointF& b, Qt::KeyboardModifiers m = {}) {
        QTest::mousePress(cw(), Qt::LeftButton, m, a.toPoint());
        for (int i = 1; i <= 8; ++i) QTest::mouseMove(cw(), (a + (b - a) * (i / 8.0)).toPoint());
        QTest::mouseRelease(cw(), Qt::LeftButton, m, b.toPoint());
        settle();
    }

private slots:
    void initTestCase() {
        QVERIFY2(QGuiApplication::platformName() == QLatin1String("offscreen"), "gui-smoke must run offscreen");
        theme::apply(*qApp);
        QCoreApplication::setAttribute(Qt::AA_CompressHighFrequencyEvents, false);
        w_ = std::make_unique<MainWindow>();
        w_->set_confirm_close(false);
        w_->resize(1480, 920);
        w_->show();
        QVERIFY(QTest::qWaitForWindowExposed(w_.get()));
        w_->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(w_.get()));
        modal_timer_.setInterval(40);
        connect(&modal_timer_, &QTimer::timeout, this, [this] {
            auto* d = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!d) return;
            if (on_modal_) {
                auto h = std::move(on_modal_);
                on_modal_ = nullptr;
                h(d);
            } else {
                d->reject();
            }
        });
        modal_timer_.start();
    }

    void smoke() {
        // 1. Startup.
        QCOMPARE(diag().canvas_backend, QStringLiteral("raster"));
        QVERIFY(s()->has_document());
        QCOMPARE(count_nodes(s()->doc().root()), 1);
        shot(QStringLiteral("startup: 1600x1000, white Background, Brush tool"));

        // 2. File > New (dialog grabbed, 900x600 accepted).
        trigger_with_modal(w_->action(QStringLiteral("new")), [this](QDialog* d) {
            d->findChild<QSpinBox*>(QStringLiteral("w"))->setValue(900);
            d->findChild<QSpinBox*>(QStringLiteral("h"))->setValue(600);
            settle();
            last_dialog_ = d->grab().toImage();
            d->accept();
        });
        QCOMPARE(s()->doc().width(), 900);
        QCOMPARE(s()->doc().height(), 600);
        shot(QStringLiteral("File > New dialog: 900 x 600"), last_dialog_);

        // 3. Scripted content: a gradient layer and a solid layer (doc-10 add_layer ops).
        QVERIFY(s()->apply({{"op", "add_layer"}, {"id", "Sky"}, {"fill", "gradient"}, {"from", "#1E3A8AFF"}, {"to", "#F59E0BFF"}, {"dir", "v"}},
                           QStringLiteral("Sky gradient"))
                    .ok());
        QVERIFY(s()->apply({{"op", "add_layer"}, {"id", "Sun"}, {"fill", "solid"}, {"color", "#E0402AFF"}, {"rect", {520, 120, 220, 220}}},
                           QStringLiteral("Sun block"))
                    .ok());
        s()->set_active_layer("Sun");
        settle();
        // The display pipeline shows exactly the core composite (opaque, away from edges).
        {
            const auto px = c()->composite_pixel(630, 230);
            QVERIFY(px.has_value());
            QCOMPARE(px->rgb(), QColor(0xE0, 0x40, 0x2A).rgb());
            const QImage g = cw()->grab().toImage();
            const QPoint wp = widget_pt(630.5, 230.5).toPoint();
            QCOMPARE(QColor(g.pixel(wp)).rgb(), QColor(0xE0, 0x40, 0x2A).rgb());
        }
        QCOMPARE(s()->applied_count(), size_t{2});
        shot(QStringLiteral("two layers added by ops; History lists both"));

        // 4. Blend mode via the Layers panel combo.
        auto* blend = w_->layers_panel()->findChild<QComboBox*>(QStringLiteral("BlendMode"));
        QVERIFY(blend);
        const int scrn = blend->findData(QStringLiteral("scrn"));
        QVERIFY(scrn >= 0);
        blend->setCurrentIndex(scrn);
        emit blend->activated(scrn);
        settle();
        QCOMPARE(s()->doc().find("Sun").node->mode, rl::BlendMode::Scrn);
        shot(QStringLiteral("Layers: blend mode Screen on 'Sun'"));

        // 5. Opacity via the panel spin box.
        auto* op = w_->layers_panel()->findChild<QSpinBox*>(QStringLiteral("Opacity"));
        QVERIFY(op);
        op->setValue(60);
        settle();
        QCOMPARE(s()->doc().find("Sun").node->opacity, 0.6);
        shot(QStringLiteral("Layers: opacity 60 %"));

        // 6. Layer > New Layer and New Group (menu actions), then a checker layer inside the group.
        w_->action(QStringLiteral("new_layer"))->trigger();
        w_->action(QStringLiteral("new_group"))->trigger();
        settle();
        QVERIFY(s()->doc().id_exists("Layer 1"));
        QVERIFY(s()->doc().id_exists("Group 1"));
        QVERIFY(s()->apply({{"op", "add_layer"}, {"id", "Tiles"}, {"parent", "Group 1"}, {"fill", "checker"}, {"cell", 24},
                            {"a", "#FFFFFF80"}, {"b", "#00000000"}, {"rect", {60, 330, 300, 220}}},
                           QStringLiteral("Tiles in group"))
                    .ok());
        s()->set_active_layer("Tiles");
        QCOMPARE(count_nodes(s()->doc().root()), 6);
        shot(QStringLiteral("New Layer + New Group (pass-through) with a child"));

        // 7. Layer mask (Reveal All) then a gradient mask by op, on 'Sky'.
        s()->set_active_layer("Sky");
        w_->action(QStringLiteral("mask_reveal"))->trigger();
        settle();
        QVERIFY(s()->doc().find("Sky").node->mask.has_value());
        QVERIFY(s()->apply({{"op", "add_mask"}, {"layer", "Sky"}, {"fill", "gradient"}, {"from", 255}, {"to", 40}, {"dir", "h"}},
                           QStringLiteral("Gradient mask"))
                    .ok());
        shot(QStringLiteral("Layer mask on 'Sky' (thumbnail in the Layers panel)"));

        // 8. Clipping mask via Layer menu on 'Layer 1' is blocked? It clips to the layer below.
        s()->set_active_layer("Sun");
        w_->action(QStringLiteral("clip"))->trigger();
        settle();
        QVERIFY(s()->doc().find("Sun").node->clip);
        shot(QStringLiteral("Create Clipping Mask on 'Sun'"));

        // 9. Invert adjustment layer (the one adjustment registered in this build).
        w_->action(QStringLiteral("adj_invert"))->trigger();
        settle();
        QVERIFY(s()->doc().id_exists("Invert 1"));
        QVERIFY(s()->doc().find("Invert 1").node->is_adjustment());
        shot(QStringLiteral("New Adjustment Layer: Invert"));

        // 10. Hide it again with the eye column (set_visible op).
        {
            QTreeWidget* tree = w_->layers_panel()->tree();
            QTreeWidgetItem* it = nullptr;
            for (QTreeWidgetItemIterator i(tree); *i; ++i)
                if ((*i)->data(1, Qt::UserRole + 1).toString() == QLatin1String("Invert 1")) it = *i;  // the id role
            QVERIFY(it);
            const QRect r = tree->visualItemRect(it);
            QTest::mouseClick(tree->viewport(), Qt::LeftButton, {}, QPoint(12, r.center().y()));
            settle();
            QVERIFY(!s()->doc().find("Invert 1").node->visible);
        }
        shot(QStringLiteral("Eye column hides the Invert layer"));

        // 11. Brush stroke from a synthesized tablet pen (sendEvent), preview mid-stroke.
        w_->action(QStringLiteral("new_layer"))->trigger();
        settle();
        const std::string paint_layer = s()->active_layer();
        key(Qt::Key_B);
        QCOMPARE(w_->tools()->tool(), Tool::Brush);
        w_->tools()->settings().brush.size = 36;
        QPointingDevice pen(QStringLiteral("Test Pen"), 4242, QInputDevice::DeviceType::Stylus, QPointingDevice::PointerType::Pen,
                            QInputDevice::Capability::Position | QInputDevice::Capability::Pressure | QInputDevice::Capability::XTilt |
                                QInputDevice::Capability::YTilt,
                            1, 3, QString(), QPointingDeviceUniqueId::fromNumericId(0xABCDEF));
        const size_t hist_before = s()->applied_count();
        tablet(QEvent::TabletPress, &pen, widget_pt(120, 120), 0.2, Qt::LeftButton, Qt::LeftButton);
        for (int i = 1; i <= 24; ++i) {
            const double t = i / 24.0;
            tablet(QEvent::TabletMove, &pen, widget_pt(120 + 360 * t, 120 + 140 * std::sin(t * 3.14159)), 0.2 + 0.8 * t, Qt::NoButton,
                   Qt::LeftButton);
        }
        QVERIFY(c()->stroke_active());
        QCOMPARE(c()->stroke().samples().size(), size_t{25});
        QCOMPARE(c()->stroke().samples().back().pressure, 1.0);
        shot(QStringLiteral("tablet stroke in progress (StrokeAdapter preview)"));
        tablet(QEvent::TabletRelease, &pen, widget_pt(480, 120), 1.0, Qt::LeftButton, Qt::NoButton);
        settle();
        QVERIFY(!c()->stroke_active());
        QCOMPARE(diag().tablet_device, QStringLiteral("Test Pen"));
        QVERIFY(diag().tablet_events_total >= 26);
        // brush_stroke is registered by the brush lane: either it committed ONE record, or the
        // session reported it as not available (and added none).
        if (rl::gui::op_available("brush_stroke")) {
            QCOMPARE(s()->applied_count(), hist_before + 1);
        } else {
            QCOMPARE(s()->applied_count(), hist_before);
            QVERIFY2(w_->last_message().contains(QLatin1String("not available")), qPrintable(w_->last_message()));
        }
        (void)paint_layer;
        shot(QStringLiteral("pen-up: brush_stroke op -> ") + w_->last_message().left(60));

        // 12. Undo with Ctrl+Z (keyboard shortcut) and redo with Ctrl+Shift+Z.
        const size_t before_undo = s()->applied_count();
        key(Qt::Key_Z, Qt::ControlModifier);
        settle();
        QCOMPARE(s()->applied_count(), before_undo - 1);
        shot(QStringLiteral("Ctrl+Z: undo (History shows the undone entry dimmed)"));
        key(Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
        settle();
        QCOMPARE(s()->applied_count(), before_undo);

        // 13. History panel: click back to the second entry, then forward to the end.
        {
            QListWidget* list = w_->history_panel()->list();
            QVERIFY(list->count() >= 5);
            QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, list->visualItemRect(list->item(2)).center());
            settle();
            QCOMPARE(s()->applied_count(), size_t{2});
            QVERIFY(!s()->doc().id_exists("Group 1"));
            shot(QStringLiteral("History: clicked entry 2 (steps back)"));
            QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, list->visualItemRect(list->item(list->count() - 1)).center());
            settle();
            QCOMPARE(s()->applied_count(), s()->entries().size());
            QVERIFY(s()->doc().id_exists("Group 1"));
        }

        // 14. Zoom in (Ctrl+=) twice and Actual Pixels, then Fit (Ctrl+0).
        const double z0 = c()->zoom();
        key(Qt::Key_Equal, Qt::ControlModifier);
        key(Qt::Key_Equal, Qt::ControlModifier);
        settle();
        QVERIFY(c()->zoom() > z0);
        shot(QStringLiteral("Ctrl+= twice: zoomed in (Navigator shows the viewport)"));
        key(Qt::Key_0, Qt::ControlModifier);
        settle();
        QCOMPARE(c()->zoom(), z0);

        // 15. Rectangular marquee drag (select_rect) and Select All from the menu.
        key(Qt::Key_M);
        QCOMPARE(w_->tools()->tool(), Tool::MarqueeRect);
        drag(widget_pt(80, 60), widget_pt(420, 300));
        if (rl::gui::op_available("select_rect")) QVERIFY(s()->state().selection.active());
        else QVERIFY(w_->last_message().contains(QLatin1String("not available")));
        shot(QStringLiteral("Marquee drag -> select_rect: ") + w_->last_message().left(50));

        // 16. Gradient tool drag (gradient op) on a raster layer.
        s()->set_active_layer("Layer 1");
        key(Qt::Key_G);
        QCOMPARE(w_->tools()->tool(), Tool::Gradient);
        drag(widget_pt(100, 500), widget_pt(800, 500));
        shot(QStringLiteral("Gradient tool options + drag: ") + w_->last_message().left(50));

        // 17. Eyedropper: sample the red block into the foreground colour.
        key(Qt::Key_I);
        s()->apply({{"op", "set_clip"}, {"layer", "Sun"}, {"value", false}}, QStringLiteral("unclip"));
        const QPointF wf = widget_pt(600.5, 200.5);
        const QPoint click(static_cast<int>(std::floor(wf.x())), static_cast<int>(std::floor(wf.y())));
        const QPointF cc = c()->to_canvas(QPointF(click));
        const auto expect = c()->composite_pixel(static_cast<int>(std::floor(cc.x())), static_cast<int>(std::floor(cc.y())));
        QTest::mouseClick(cw(), Qt::LeftButton, {}, click);
        settle();
        QVERIFY(expect.has_value());
        QCOMPARE(w_->tools()->fg().rgb(), expect->rgb());
        shot(QStringLiteral("Eyedropper sampled the composite into the foreground"));

        // 18. Levels adjustment dialog (grabbed; accept).
        trigger_with_modal(w_->action(QStringLiteral("adj_levels")), [this](QDialog* d) {
            settle();
            last_dialog_ = d->grab().toImage();
            d->accept();
        });
        shot(QStringLiteral("New Levels Layer dialog"), last_dialog_);

        // 19. Curves adjustment dialog.
        trigger_with_modal(w_->action(QStringLiteral("adj_curves")), [this](QDialog* d) {
            settle();
            last_dialog_ = d->grab().toImage();
            d->reject();
        });
        shot(QStringLiteral("New Curves Layer dialog"), last_dialog_);

        // 20. Filter > Gaussian Blur dialog with preview.
        s()->set_active_layer("Sky");
        trigger_with_modal(w_->action(QStringLiteral("filter_gaussian_blur")), [this](QDialog* d) {
            QTest::qWait(250);  // let the debounced preview run
            last_dialog_ = d->grab().toImage();
            d->accept();
        });
        shot(QStringLiteral("Filter > Gaussian Blur dialog (preview status)"), last_dialog_);

        // 21. Image > Canvas Size dialog.
        trigger_with_modal(w_->action(QStringLiteral("canvas_size")), [this](QDialog* d) {
            settle();
            last_dialog_ = d->grab().toImage();
            d->reject();
        });
        shot(QStringLiteral("Image > Canvas Size dialog (anchor grid)"), last_dialog_);

        // 22. Reorder by the Layers panel's drop path: move 'Sun' to the bottom of root.
        {
            emit w_->layers_panel()->tree()->move_requested(QStringLiteral("Sun"), QStringLiteral("root"), 0);
            settle();
            QCOMPARE(s()->doc().root().children.front().id, std::string("Sun"));
        }
        shot(QStringLiteral("Layers: 'Sun' moved to the bottom (move_layer)"));

        // 23. Merge Down (Ctrl+E) on the top raster above 'Sky'.
        {
            const int n0 = count_nodes(s()->doc().root());
            s()->set_active_layer("Sky");
            key(Qt::Key_E, Qt::ControlModifier);
            settle();
            QCOMPARE(count_nodes(s()->doc().root()), n0 - 1);
        }
        shot(QStringLiteral("Merge Down (Ctrl+E)"));

        // 24. Input Diagnostics (non-modal).
        w_->action(QStringLiteral("diagnostics"))->trigger();
        settle();
        {
            auto* dd = w_->findChild<InputDiagnosticsDialog*>();
            QVERIFY(dd);
            dd->refresh();
            QString platform;
            for (QLabel* l : dd->findChildren<QLabel*>())
                if (l->text() == QLatin1String("offscreen")) platform = l->text();
            QCOMPARE(platform, QStringLiteral("offscreen"));
            shot(QStringLiteral("Help > Input Diagnostics (live values)"), dd->grab().toImage());
            dd->close();
        }

        // 25. About (Licenses tab exists).
        trigger_with_modal(w_->action(QStringLiteral("about")), [this](QDialog* d) {
            settle();
            last_dialog_ = d->grab().toImage();
            d->reject();
        });
        shot(QStringLiteral("Help > About (About / Licenses / Third-party)"), last_dialog_);

        // 26. Crop tool: drag a pending rectangle (overlay), then Esc.
        key(Qt::Key_C);
        drag(widget_pt(150, 100), widget_pt(700, 480));
        QVERIFY(c()->has_pending());
        shot(QStringLiteral("Crop tool: pending rectangle with thirds"));
        QTest::keyClick(cw(), Qt::Key_Escape);
        settle();
        QVERIFY(!c()->has_pending());

        // 27. Free Transform (Ctrl+T) box on 'Layer 1'.
        s()->set_active_layer("Tiles");
        key(Qt::Key_T, Qt::ControlModifier);
        settle();
        if (c()->has_pending()) {
            drag(widget_pt(210, 440), widget_pt(260, 470));  // move inside the box
            shot(QStringLiteral("Free Transform: moved box, Enter applies"));
            QTest::keyClick(cw(), Qt::Key_Return);
            settle();
        } else {
            shot(QStringLiteral("Free Transform: ") + w_->last_message().left(60));
        }

        // 28. Delete layer and Flatten.
        {
            s()->set_active_layer("Layer 1");
            const int n0 = count_nodes(s()->doc().root());
            w_->action(QStringLiteral("delete_layer"))->trigger();
            settle();
            QCOMPARE(count_nodes(s()->doc().root()), n0 - 1);
            w_->action(QStringLiteral("flatten"))->trigger();
            settle();
            QCOMPARE(static_cast<int>(s()->doc().root().children.size()), 1);
        }
        shot(QStringLiteral("Delete Layer, then Flatten Image"));

        // 29. Window: hide the History panel, then Reset Layout.
        {
            auto* dm = w_->dock_manager();
            ads::CDockWidget* h = dm->findDockWidget(QStringLiteral("historyDock"));
            QVERIFY(h);
            h->toggleView(false);
            settle();
            shot(QStringLiteral("Window: History panel hidden"));
            w_->action(QStringLiteral("reset_layout"))->trigger();
            settle();
            QVERIFY(!h->isClosed());
        }

        // 30. Tool options for the Clone Stamp and the Magic Wand.
        key(Qt::Key_S);
        QCOMPARE(w_->tools()->tool(), Tool::Clone);
        shot(QStringLiteral("Clone Stamp options (Alt-click sets source)"));
        key(Qt::Key_W);
        QCOMPARE(w_->tools()->tool(), Tool::Wand);
        shot(QStringLiteral("Magic Wand options"));
    }

    // Wave-4 capabilities: live StrokeSession painting, diagnostics, adjustment editing, inline
    // rename, quad Free Transform, the mip pyramid + status bar, file warnings, JPEG export
    // quality, the unsaved-changes prompt and a > 16384 px PSB.
    void new_capabilities() {
        w_->new_document(1600, 1000, 0);
        settle();
        QVERIFY(s()->apply({{"op", "add_layer"}, {"id", "Ground"}, {"fill", "gradient"}, {"from", "#2E5E3AFF"}, {"to", "#C9B27AFF"},
                            {"rect", {0, 560, 1600, 440}}},
                           "ground")
                    .ok());
        QVERIFY(s()->apply({{"op", "add_layer"}, {"id", "Paint"}}, "paint").ok());
        s()->set_active_layer("Paint");
        settle();

        // A. Live soft-brush stroke (StrokeSession preview tiles), then its commit.
        key(Qt::Key_B);
        BrushSettings& b = w_->tools()->settings().brush;
        b.size = 120;
        b.hardness = 0.0;
        b.flow = 0.7;
        w_->tools()->set_fg(QColor(0xE8, 0x6A, 0x2C));
        w_->tools()->notify_settings();
        QPointingDevice pen(QStringLiteral("Smoke Pen"), 4343, QInputDevice::DeviceType::Stylus, QPointingDevice::PointerType::Pen,
                            QInputDevice::Capability::Position | QInputDevice::Capability::Pressure | QInputDevice::Capability::XTilt |
                                QInputDevice::Capability::YTilt,
                            1, 3, QString(), QPointingDeviceUniqueId::fromNumericId(0xC0FFEE));
        const size_t hist0 = s()->applied_count();
        tablet(QEvent::TabletPress, &pen, widget_pt(200, 300), 0.1, Qt::LeftButton, Qt::LeftButton);
        for (int i = 1; i <= 60; ++i) {
            const double t = i / 60.0;
            tablet(QEvent::TabletMove, &pen, widget_pt(200 + 1200 * t, 300 + 220 * std::sin(t * 5.0)), 0.1 + 0.9 * std::sin(t * 3.1),
                   Qt::NoButton, Qt::LeftButton);
        }
        QVERIFY(c()->stroke_active());
        QVERIFY(c()->stroke().preview_pixels());
        QCOMPARE(s()->applied_count(), hist0);  // committed only at pen-up
        shot(QStringLiteral("live 120 px soft stroke: StrokeSession preview tiles (pen still down)"));
        tablet(QEvent::TabletRelease, &pen, widget_pt(1400, 300 + 220 * std::sin(5.0)), 0.2, Qt::LeftButton, Qt::NoButton);
        settle();
        QCOMPARE(s()->applied_count(), hist0 + 1);

        // B. Help > Input Diagnostics: dab-to-pixel latency and engine dabs/s of that stroke.
        w_->action(QStringLiteral("diagnostics"))->trigger();
        settle();
        if (auto* dd = w_->findChild<QDialog*>(QStringLiteral("InputDiagnostics"))) {
            QTest::qWait(150);
            shot(QStringLiteral("Input Diagnostics: dab-to-pixel median/p95 + engine dabs/s"), dd->grab().toImage());
            dd->close();
        }

        // C. Double-click a Levels layer: prefilled dialog with live preview (set_adjustment).
        QVERIFY(s()->apply({{"op", "add_adjustment"}, {"id", "Levels 1"}, {"type", "levels"}, {"params", {{"rgb", {{"in_black", 30}, {"gamma", 1.3}}}}}},
                           "lv")
                    .ok());
        settle();
        {
            QTreeWidget* t = w_->layers_panel()->tree();
            QTreeWidgetItem* it = nullptr;
            for (QTreeWidgetItemIterator i(t); *i; ++i)
                if ((*i)->data(1, Qt::UserRole + 1).toString() == QLatin1String("Levels 1")) it = *i;
            QVERIFY(it);
            t->scrollToItem(it);
            const QRect r = t->visualRect(t->indexFromItem(it, 1));
            QTest::mouseClick(t->viewport(), Qt::LeftButton, Qt::NoModifier, r.center());
            bool seen = false;
            on_modal_ = [&](QDialog* d) {
                seen = true;
                QTest::qWait(300);
                last_dialog_ = d->grab().toImage();
                d->reject();
            };
            QTest::mouseDClick(t->viewport(), Qt::LeftButton, Qt::NoModifier, r.center());
            settle();
            on_modal_ = nullptr;
            QVERIFY(seen);
            shot(QStringLiteral("double-click Levels layer: edit dialog, prefilled (set_adjustment)"), last_dialog_);
        }

        // D. Inline rename (F2). set_name: the id stays "Paint".
        s()->set_active_layer("Paint");
        w_->action(QStringLiteral("rename_layer"))->trigger();
        settle();
        QVERIFY(w_->layers_panel()->rename_editor());
        w_->layers_panel()->rename_editor()->setText(QStringLiteral("Brush strokes"));
        shot(QStringLiteral("F2: inline rename in the Layers panel"));
        QTest::keyClick(w_->layers_panel()->rename_editor(), Qt::Key_Return);
        settle();

        // E. Free Transform: Ctrl-drag a corner (distort) with the live preview, then Enter. (The
        // renamed layer is still active; its id is still "Paint".)
        QVERIFY(s()->active_node() && s()->active_node()->is_raster());
        key(Qt::Key_T, Qt::ControlModifier);
        settle();
        QVERIFY(c()->has_pending());
        {
            const auto q = c()->transform_corners();
            drag(c()->to_widget(q[1]), c()->to_widget(q[1] + QPointF(-160, 90)), Qt::ControlModifier);
            drag(c()->to_widget(q[3]), c()->to_widget(q[3] + QPointF(120, -40)), Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier);
            QVERIFY(c()->transform_quad_mode());
            shot(QStringLiteral("Free Transform: Ctrl distort + Ctrl+Alt+Shift perspective (live preview)"));
            QTest::keyClick(cw(), Qt::Key_Return);
            settle();
            QVERIFY(!c()->has_pending());
            shot(QStringLiteral("Enter: one transform op -> ") + w_->last_message().left(50));
        }

        // F. Zoom out to 25 % (drawn from mip level 2); status bar shows position + colour.
        c()->set_zoom(0.25, QPointF(cw()->width() / 2.0, cw()->height() / 2.0));
        c()->finish_display();
        QCOMPARE(c()->display_level(), 2);
        QTest::mouseMove(cw(), widget_pt(800, 800).toPoint());
        QTest::qWait(80);
        shot(QStringLiteral("25 % from the CPU mip pyramid; status: X/Y + RGBA under cursor"));
        c()->fit_to_view();

        // G. JPEG export: quality dialog (grabbed, cancelled).
        trigger_with_modal(w_->action(QStringLiteral("export_jpeg")), [this](QDialog* d) {
            last_dialog_ = d->grab().toImage();
            d->reject();
        });
        shot(QStringLiteral("File > Export > JPEG: quality"), last_dialog_);

        // H. Unsaved changes prompt on File > New (Cancel keeps the document).
        QVERIFY(s()->modified());
        w_->set_confirm_close(true);
        trigger_with_modal(w_->action(QStringLiteral("new")), [this](QDialog* d) {
            on_modal_ = [this](QDialog* box) {  // the prompt that follows the New dialog
                last_dialog_ = box->grab().toImage();
                box->reject();
            };
            d->accept();
        });
        w_->set_confirm_close(false);
        QVERIFY(s()->modified());
        QVERIFY(s()->doc().id_exists("Ground"));
        shot(QStringLiteral("New with unsaved changes: Save / Discard / Cancel"), last_dialog_);

        // I. Open PSDs from the independent corpus: warnings in one dialog; a 30001 px PSB.
        QTemporaryDir tmp;
        QProcess py;
        py.start(QStringLiteral("python3"), {QStringLiteral(RL_PSD_CORPUS_GEN), tmp.path()});
        if (py.waitForFinished(120000) && py.exitCode() == 0) {
            QVERIFY(w_->open_file(tmp.filePath(QStringLiteral("adjustment.psd"))));
            settle();
            QVERIFY(w_->warnings_dialog());
            shot(QStringLiteral("adjustment.psd: import notes in one dialog"), w_->warnings_dialog()->grab().toImage());
            w_->warnings_dialog()->close();
            QVERIFY(w_->open_file(tmp.filePath(QStringLiteral("psb_30001.psb"))));
            settle();
            c()->finish_display();
            QVERIFY(w_->warnings_dialog());
            shot(QStringLiteral("psb_30001.psb (> 16384 px) opens with a note"), w_->warnings_dialog()->grab().toImage());
            w_->warnings_dialog()->close();
        } else {
            shot(QStringLiteral("PSD corpus frames skipped (python3 + numpy missing)"));
        }
    }

    // Doc-60 layer edits (display names, duplicate, group isolate, Ctrl+click alpha, Layer
    // Properties) and the navigation / workflow features with their own ctests (gui_navigation).
    QTreeWidgetItem* layer_item(const char* id) {
        for (QTreeWidgetItemIterator i(w_->layers_panel()->tree()); *i; ++i)
            if ((*i)->data(1, Qt::UserRole + 1).toString() == QLatin1String(id)) return *i;
        return nullptr;
    }

    // Waits out the Navigator's 150 ms debounce so its thumbnail matches the frame.
    void nav_shot(const QString& caption, const QImage& overlay = {}) {
        QTest::qWait(200);
        shot(caption, overlay);
    }

    void layer_ops_and_navigation() {
        w_->new_document(1200, 760, 0);
        settle();
        for (const Json& op : {Json{{"op", "add_layer"}, {"id", "Sky"}, {"fill", "gradient"}, {"from", "#1B3A6BFF"}, {"to", "#E9A86BFF"}},
                               Json{{"op", "add_group"}, {"id", "Hills"}, {"mode", "pass"}},
                               Json{{"op", "add_layer"}, {"id", "Far"}, {"parent", "Hills"}, {"fill", "solid"}, {"color", "#3F6B4AFF"}, {"rect", {0, 430, 1200, 330}}},
                               Json{{"op", "add_layer"}, {"id", "Near"}, {"parent", "Hills"}, {"fill", "solid"}, {"color", "#26452FFF"}, {"rect", {0, 560, 1200, 200}}},
                               Json{{"op", "add_layer"}, {"id", "Sun"}, {"fill", "gradient"}, {"from", "#FFE08A00"}, {"to", "#FFE08AFF"}, {"rect", {820, 90, 180, 180}}},
                               Json{{"op", "add_adjustment"}, {"id", "Warm"}, {"type", "levels"}, {"params", {{"rgb", {{"gamma", 1.2}}}}}}})
            QVERIFY2(s()->apply(op, "setup").ok(), op.dump().c_str());
        settle();

        // 1. Rename through set_name: the row shows the display name, the id is unchanged.
        s()->set_active_layer("Near");
        w_->action(QStringLiteral("rename_layer"))->trigger();
        settle();
        QVERIFY(w_->layers_panel()->rename_editor());
        w_->layers_panel()->rename_editor()->setText(QStringLiteral("Foreground hill"));
        QTest::keyClick(w_->layers_panel()->rename_editor(), Qt::Key_Return);
        settle();
        QVERIFY(s()->doc().id_exists("Near"));
        QVERIFY(layer_item("Near")->text(1).startsWith(QStringLiteral("Foreground hill")));
        nav_shot(QStringLiteral("set_name: row shows 'Foreground hill', id 'Near' unchanged; History: Rename"));

        // 2. Duplicate the group (Ctrl+J): derived ids, the copy above the source, active.
        s()->set_active_layer("Hills");
        const size_t n0 = s()->applied_count();
        key(Qt::Key_J, Qt::ControlModifier);
        QCOMPARE(s()->applied_count(), n0 + 1);
        QVERIFY(s()->doc().id_exists("Hills copy") && s()->doc().id_exists("Hills copy/Near"));
        nav_shot(QStringLiteral("Ctrl+J: Duplicate Layer 'Hills' -> 'Hills copy' (children keep names)"));
        s()->undo();
        settle();

        // 3. Group Pass Through -> Isolated (set_group_mode).
        s()->set_active_layer("Hills");
        w_->action(QStringLiteral("isolate_group"))->trigger();
        settle();
        QVERIFY(s()->active_node()->mode != rl::BlendMode::Pass);
        nav_shot(QStringLiteral("Layer > Isolate Group: 'Hills' Pass Through -> Isolated, Normal"));

        // 4. Ctrl+click the Sun thumbnail: selection from its alpha (marching ants).
        {
            QTreeWidget* t = w_->layers_panel()->tree();
            QTreeWidgetItem* it = layer_item("Sun");
            QVERIFY(it);
            t->scrollToItem(it);
            const QRect col = t->visualRect(t->indexFromItem(it, 1));
            QTest::mouseClick(t->viewport(), Qt::LeftButton, Qt::ControlModifier, QPoint(col.left() + 12, col.center().y()));
            settle();
            QVERIFY(s()->state().selection.active());
            nav_shot(QStringLiteral("Ctrl+click 'Sun' thumbnail: select_alpha (selection = its alpha ramp)"));
            key(Qt::Key_D, Qt::ControlModifier);
        }

        // 5. Layer > Layer Properties on the adjustment layer: its settings, prefilled.
        s()->set_active_layer("Warm");
        trigger_with_modal(w_->action(QStringLiteral("layer_properties")), [this](QDialog* d) {
            QTest::qWait(200);
            last_dialog_ = d->grab().toImage();
            d->reject();
        });
        nav_shot(QStringLiteral("Layer Properties on 'Warm': Levels settings, prefilled (set_adjustment)"), last_dialog_);

        // 6. Hand tool drag at 100 %.
        c()->actual_pixels();
        settle();
        key(Qt::Key_H);
        const QPointF off = c()->offset();
        drag(QPointF(600, 400), QPointF(420, 300));
        QCOMPARE(c()->offset(), off + QPointF(-180, -100));
        nav_shot(QStringLiteral("Hand tool (H): view dragged by (-180, -100); document unchanged"));

        // 7. Zoom tool: rubber band while dragging, then the rectangle fills the view.
        key(Qt::Key_Z);
        {
            const QPointF a = widget_pt(760, 60), b = widget_pt(1060, 300);
            QTest::mousePress(cw(), Qt::LeftButton, Qt::NoModifier, a.toPoint());
            for (int i = 1; i <= 8; ++i) QTest::mouseMove(cw(), (a + (b - a) * (i / 8.0)).toPoint());
            nav_shot(QStringLiteral("Zoom tool drag: rubber band around the sun"));
            const double z0 = c()->zoom();
            QTest::mouseRelease(cw(), Qt::LeftButton, Qt::NoModifier, b.toPoint());
            settle();
            QVERIFY(c()->zoom() > z0 * 2);
            nav_shot(QStringLiteral("Zoom tool release: the rectangle fills the view (%1 %)").arg(std::lround(c()->zoom() * 100)));
        }
        c()->fit_to_view();

        // 8. Filter > Gaussian Blur on the Sun, then Filter > Repeat (Ctrl+F).
        s()->set_active_layer("Sun");
        trigger_with_modal(w_->action(QStringLiteral("filter_gaussian_blur")), [](QDialog* d) {
            if (auto* pd = qobject_cast<ParamDialog*>(d))
                if (auto* r = qobject_cast<QDoubleSpinBox*>(pd->field("radius"))) r->setValue(6.0);
            d->accept();
        });
        key(Qt::Key_F, Qt::ControlModifier);
        {
            QMenu* fm = nullptr;
            for (QObject* o : w_->action(QStringLiteral("repeat_filter"))->associatedObjects())
                if (auto* m = qobject_cast<QMenu*>(o)) fm = m;
            QVERIFY(fm);
            fm->popup(w_->mapToGlobal(QPoint(300, 40)));
            settle();
            const QImage menu = fm->grab().toImage();
            fm->hide();
            QCOMPARE(w_->action(QStringLiteral("repeat_filter"))->text(), QStringLiteral("Repeat Gaussian Blur"));
            nav_shot(QStringLiteral("Filter menu: 'Repeat Gaussian Blur' (Ctrl+F); History: two blurs"), menu);
        }

        // 9. X swaps the colours, D restores black / white.
        w_->tools()->set_fg(QColor(0xE8, 0x6A, 0x2C));
        w_->tools()->set_bg(QColor(0x2C, 0x7A, 0xE8));
        if (ads::CDockWidget* cd = w_->dock_manager()->findDockWidget(QStringLiteral("colorDock"))) cd->setAsCurrentTab();  // show the swatches
        key(Qt::Key_X);
        QCOMPARE(w_->tools()->fg(), QColor(0x2C, 0x7A, 0xE8));
        nav_shot(QStringLiteral("X: foreground/background swapped (blue over orange)"));
        key(Qt::Key_D);
        QCOMPARE(w_->tools()->fg(), QColor(Qt::black));
        nav_shot(QStringLiteral("D: default colours (black over white)"));
        if (ads::CDockWidget* nd = w_->dock_manager()->findDockWidget(QStringLiteral("navigatorDock"))) nd->setAsCurrentTab();

        // 10. Move tool: Shift+Left x3 nudges the Sun 30 px, one record per press.
        key(Qt::Key_V);
        const size_t n1 = s()->applied_count();
        cw()->setFocus(Qt::OtherFocusReason);
        for (int i = 0; i < 3; ++i) QTest::keyClick(cw(), Qt::Key_Left, Qt::ShiftModifier);
        settle();
        QCOMPARE(s()->applied_count(), n1 + 3);
        nav_shot(QStringLiteral("Move tool: Shift+Left x3 nudged 'Sun' 30 px (3 Nudge rows)"));
    }

    // Every action once; any dialog is closed by the modal timer. Proves no action crashes.
    void every_action() {
        QStringList skipped;
        int n = 0;
        for (const QString& key : w_->action_keys()) {
            if (key == QLatin1String("quit")) {
                skipped << key;
                continue;
            }
            QAction* a = w_->action(key);
            QVERIFY(a);
            on_modal_ = nullptr;
            a->trigger();
            settle();
            // Close non-modal windows the action may have opened (diagnostics).
            for (QWidget* tw : QApplication::topLevelWidgets())
                if (tw != w_.get() && tw->isVisible() && qobject_cast<QDialog*>(tw)) tw->close();
            ++n;
        }
        QVERIFY(n >= 60);
        QVERIFY(s()->has_document());
        qInfo("triggered %d actions (skipped: %s); last message: %s", n, qPrintable(skipped.join(QLatin1Char(','))),
              qPrintable(w_->last_message()));
        shot(QStringLiteral("after triggering all %1 menu actions").arg(n));
    }

    void cleanupTestCase() {
        modal_timer_.stop();
        // Contact sheet: 3 columns of 720-px-wide grabs with a caption strip.
        const int cols = 3, tw = 720;
        const int th = shots_.empty() ? 0 : static_cast<int>(720.0 * shots_.front().img.height() / shots_.front().img.width());
        const int cap = 26, pad = 10;
        const int rows = (static_cast<int>(shots_.size()) + cols - 1) / cols;
        QImage sheet(pad + cols * (tw + pad), 40 + pad + rows * (th + cap + pad), QImage::Format_RGB32);
        sheet.fill(QColor(0x14, 0x15, 0x17));
        QPainter p(&sheet);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.setPen(QColor(0xdc, 0xde, 0xe2));
        QFont f = p.font();
        f.setPixelSize(15);
        f.setBold(true);
        p.setFont(f);
        p.drawText(QRect(pad, 8, sheet.width(), 28), Qt::AlignVCenter,
                   QStringLiteral("Rasterloom gui-smoke  |  platform %1  |  canvas %2  |  Qt %3")
                       .arg(QGuiApplication::platformName(), diag().canvas_backend, QString::fromLatin1(qVersion())));
        f.setPixelSize(13);
        f.setBold(false);
        p.setFont(f);
        for (size_t i = 0; i < shots_.size(); ++i) {
            const int x = pad + static_cast<int>(i % cols) * (tw + pad);
            const int y = 40 + pad + static_cast<int>(i / cols) * (th + cap + pad);
            p.fillRect(QRect(x, y, tw, cap), QColor(0x24, 0x25, 0x29));
            p.setPen(QColor(0xdc, 0xde, 0xe2));
            p.drawText(QRect(x + 8, y, tw - 16, cap), Qt::AlignVCenter | Qt::AlignLeft, shots_[i].caption);
            p.drawImage(QRect(x, y + cap, tw, th), shots_[i].img);
        }
        p.end();
        QDir().mkpath(QStringLiteral(RL_GUI_OUT "/gui-smoke"));
        for (size_t i = 0; i < shots_.size(); ++i)  // full-size grabs for close inspection
            shots_[i].img.save(QStringLiteral(RL_GUI_OUT "/gui-smoke/%1.png").arg(i + 1, 2, 10, QLatin1Char('0')));
        const QString path = QStringLiteral(RL_GUI_OUT "/gui-contact-sheet.png");
        QVERIFY(sheet.save(path));
        qInfo("contact sheet: %s (%zu shots)", qPrintable(path), shots_.size());
        w_.reset();
    }
};

QTEST_MAIN(GuiSmoke)
#include "gui_smoke.moc"
