// SPDX-License-Identifier: GPL-3.0-or-later
//
// Evidence for view/navigation and small workflow features, one QtTest function per PARITY row
// (tests/gui/CMakeLists.txt registers each function as its own ctest):
//   hand_tool_pan            TOOL-068  Hand tool (H) drag and Space-drag pan the view, never the document
//   zoom_tool_click_drag     TOOL-070  Zoom tool click / Alt+click steps at the pointer; a drag zooms to the rectangle
//   open_recent              FILE-004  File > Open Recent lists saved/opened files newest first and opens them
//   repeat_last_filter       FLT-075   Filter > Repeat <filter> (Ctrl+F) re-applies the last filter's settings
//   default_and_swap_colors  TOOL-071/072  D resets black/white, X swaps foreground and background
//   move_tool_arrow_nudge    LAYER-043 Move tool + arrow keys nudge the active layer 1 px (Shift: 10 px)
//   brush_options_bar        the Brush options bar shows hardness / spacing / opacity / flow with tooltips
#include <QApplication>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QLabel>
#include <QMenu>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <functional>

#include "gui/canvas.hpp"
#include "gui/dialogs.hpp"
#include "gui/main_window.hpp"
#include "gui/session.hpp"
#include "gui/tools.hpp"

using namespace rl::gui;

class GuiNavigation : public QObject {
    Q_OBJECT
    std::unique_ptr<MainWindow> w_;
    std::function<void(QDialog*)> on_modal_;
    QTimer modal_timer_;
    QTemporaryDir tmp_;

    EditorSession* s() { return w_->session(); }
    CanvasController* c() { return w_->canvas(); }
    QWidget* cw() { return w_->canvas_widget(); }
    void settle() {
        for (int i = 0; i < 5; ++i) QCoreApplication::processEvents();
    }
    // Window shortcuts (D, X, H, Z, Ctrl+F) need the main window active.
    void key(Qt::Key k, Qt::KeyboardModifiers m = Qt::NoModifier) {
        if (QApplication::activeWindow() != w_.get()) {
            w_->activateWindow();
            QVERIFY(QTest::qWaitForWindowActive(w_.get()));
        }
        QTest::keyClick(w_.get(), k, m);
        settle();
    }
    // Keys the canvas handles itself (arrows, Space) go to the focused canvas widget.
    void canvas_key(Qt::Key k, Qt::KeyboardModifiers m = Qt::NoModifier) {
        cw()->setFocus(Qt::OtherFocusReason);
        QTest::keyClick(cw(), k, m);
        settle();
    }
    void drag(const QPointF& a, const QPointF& b, Qt::KeyboardModifiers m = {}) {
        QTest::mousePress(cw(), Qt::LeftButton, m, a.toPoint());
        for (int i = 1; i <= 8; ++i) QTest::mouseMove(cw(), (a + (b - a) * (i / 8.0)).toPoint());
        QTest::mouseRelease(cw(), Qt::LeftButton, m, b.toPoint());
        settle();
    }
    QString path(const QString& name) const { return tmp_.filePath(name); }
    void fresh_doc(int w = 400, int h = 300) {
        w_->new_document(w, h, 1);
        settle();
    }

private slots:
    void initTestCase() {
        QVERIFY2(QGuiApplication::platformName() == QLatin1String("offscreen"), "must run offscreen");
        QVERIFY(tmp_.isValid());
        QCoreApplication::setAttribute(Qt::AA_CompressHighFrequencyEvents, false);
        w_ = std::make_unique<MainWindow>();
        w_->set_confirm_close(false);
        w_->resize(1200, 800);
        w_->show();
        QVERIFY(QTest::qWaitForWindowExposed(w_.get()));
        w_->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(w_.get()));
        modal_timer_.setInterval(30);
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

    void hand_tool_pan() {
        fresh_doc(1600, 1200);  // larger than the view: every pan below stays inside the clamp
        c()->actual_pixels();
        settle();
        key(Qt::Key_H);
        QCOMPARE(w_->tools()->tool(), Tool::Hand);
        const size_t records = s()->applied_count();
        QPointF off = c()->offset();
        drag(QPointF(300, 300), QPointF(360, 340));
        QCOMPARE(c()->offset(), off + QPointF(60, 40));
        QCOMPARE(c()->zoom(), 1.0);
        // Space held with another tool is a temporary Hand: the Brush paints nothing.
        key(Qt::Key_B);
        QCOMPARE(w_->tools()->tool(), Tool::Brush);
        off = c()->offset();
        cw()->setFocus(Qt::OtherFocusReason);
        QTest::keyPress(cw(), Qt::Key_Space);
        drag(QPointF(400, 300), QPointF(330, 280));
        QTest::keyRelease(cw(), Qt::Key_Space);
        settle();
        QCOMPARE(c()->offset(), off + QPointF(-70, -20));
        QCOMPARE(w_->tools()->tool(), Tool::Brush);
        QCOMPARE(s()->applied_count(), records);  // panning never edits the document
        QVERIFY(!s()->modified());
    }

    void zoom_tool_click_drag() {
        fresh_doc(400, 300);
        c()->actual_pixels();
        settle();
        key(Qt::Key_Z);
        QCOMPARE(w_->tools()->tool(), Tool::Zoom);
        // Click: the next preset step, the canvas point under the pointer stays under it.
        const QPointF p(500, 350);
        const QPointF under = c()->to_canvas(p);
        QTest::mouseClick(cw(), Qt::LeftButton, Qt::NoModifier, p.toPoint());
        settle();
        const double z1 = c()->zoom();
        QVERIFY2(z1 > 1.0 && z1 <= 2.0, qPrintable(QString::number(z1)));
        QVERIFY((c()->to_widget(under) - p).manhattanLength() <= 1.0);
        // Alt+click: back one step.
        QTest::mouseClick(cw(), Qt::LeftButton, Qt::AltModifier, p.toPoint());
        settle();
        QCOMPARE(c()->zoom(), 1.0);
        // A 4 px jitter is still a click, not a rectangle.
        drag(p, p + QPointF(3, 2));
        QCOMPARE(c()->zoom(), z1);
        c()->actual_pixels();
        settle();
        // Drag a rectangle: it fills the view (the limiting side), centred.
        const QPointF a(420, 300), b(520, 350);  // 100 x 50 widget px at zoom 1
        const QPointF centre_c = c()->to_canvas((a + b) / 2);
        const double expect = std::min(cw()->width() / 100.0, cw()->height() / 50.0);
        drag(a, b);
        QVERIFY2(std::abs(c()->zoom() - expect) < 1e-9, qPrintable(QStringLiteral("%1 vs %2").arg(c()->zoom()).arg(expect)));
        const QPointF view_centre(cw()->width() / 2.0, cw()->height() / 2.0);
        QVERIFY2((c()->to_widget(centre_c) - view_centre).manhattanLength() <= 2.0, "the rectangle's centre is the view's centre");
        QVERIFY(!s()->modified());
    }

    void open_recent() {
        fresh_doc(64, 48);
        QVERIFY(s()->apply({{"op", "add_layer"}, {"id", "A"}, {"fill", "solid"}, {"color", "#FF0000"}}, "a").ok());
        QVERIFY(w_->save_to(path(QStringLiteral("first.orp"))));
        fresh_doc(32, 24);
        QVERIFY(w_->save_to(path(QStringLiteral("second.orp"))));
        fresh_doc(10, 10);
        auto* menu = w_->findChild<QMenu*>(QStringLiteral("RecentMenu"));
        QVERIFY(menu);
        emit menu->aboutToShow();  // the menu is rebuilt each time it opens
        QList<QAction*> acts = menu->actions();
        QVERIFY(acts.size() >= 3);  // second, first, separator, Clear Recent
        QVERIFY2(acts[0]->text().endsWith(QStringLiteral("second.orp")), qPrintable(acts[0]->text()));
        QVERIFY2(acts[1]->text().endsWith(QStringLiteral("first.orp")), qPrintable(acts[1]->text()));
        QCOMPARE(acts[1]->toolTip(), QFileInfo(path(QStringLiteral("first.orp"))).absoluteFilePath());
        acts[1]->trigger();
        settle();
        QCOMPARE(s()->file_path(), QFileInfo(path(QStringLiteral("first.orp"))).absoluteFilePath());
        QCOMPARE(s()->doc().width(), 64);
        QVERIFY(s()->doc().id_exists("A"));
        // Opening moved it to the top; a file that disappeared is listed but disabled.
        QVERIFY(QFile::remove(path(QStringLiteral("second.orp"))));
        emit menu->aboutToShow();
        acts = menu->actions();
        QVERIFY(acts[0]->text().endsWith(QStringLiteral("first.orp")));
        QVERIFY(acts[0]->isEnabled());
        QVERIFY(acts[1]->text().endsWith(QStringLiteral("second.orp")));
        QVERIFY(!acts[1]->isEnabled());
        // Clear Recent empties it.
        menu->actions().back()->trigger();
        emit menu->aboutToShow();
        QVERIFY(!menu->actions().front()->isEnabled());
        QCOMPARE(menu->actions().front()->text(), QStringLiteral("(empty)"));
    }

    void repeat_last_filter() {
        fresh_doc(96, 64);
        QVERIFY(s()->apply({{"op", "add_layer"}, {"id", "L1"}, {"fill", "solid"}, {"color", "#2060C0FF"}, {"rect", {20, 16, 40, 30}}}, "l1").ok());
        QVERIFY(s()->apply({{"op", "add_layer"}, {"id", "L2"}, {"fill", "solid"}, {"color", "#C02020FF"}, {"rect", {10, 10, 30, 20}}}, "l2").ok());
        QAction* rep = w_->action(QStringLiteral("repeat_filter"));
        QVERIFY(rep);
        QVERIFY(!rep->isEnabled());  // nothing to repeat yet
        s()->set_active_layer("L1");
        on_modal_ = [](QDialog* d) {
            auto* pd = qobject_cast<ParamDialog*>(d);
            QVERIFY(pd);
            auto* radius = qobject_cast<QDoubleSpinBox*>(pd->field("radius"));
            QVERIFY(radius);
            radius->setValue(2.5);
            d->accept();
        };
        w_->action(QStringLiteral("filter_gaussian_blur"))->trigger();
        QTRY_VERIFY(!on_modal_);
        settle();
        size_t n = s()->applied_count();
        const Json first = s()->entries()[n - 1].op;
        QCOMPARE(first["op"].get<std::string>(), std::string("filter_gaussian_blur"));
        QCOMPARE(first["radius"].get<double>(), 2.5);
        QVERIFY(rep->isEnabled());
        QCOMPARE(rep->text(), QStringLiteral("Repeat Gaussian Blur"));
        // Ctrl+F: the same filter and settings again, one record, no dialog.
        key(Qt::Key_F, Qt::ControlModifier);
        QCOMPARE(s()->applied_count(), n + 1);
        QCOMPARE(s()->entries()[n].op, first);
        QCOMPARE(s()->entries()[n].label, QStringLiteral("Gaussian Blur"));
        // On another layer with a selection: it targets the active layer and the new selection.
        s()->set_active_layer("L2");
        QVERIFY(s()->apply({{"op", "select_rect"}, {"x", 0}, {"y", 0}, {"w", 24}, {"h", 24}}, "sel").ok());
        n = s()->applied_count();
        key(Qt::Key_F, Qt::ControlModifier);
        QCOMPARE(s()->applied_count(), n + 1);
        const Json again = s()->entries()[n].op;
        QCOMPARE(again["layer"].get<std::string>(), std::string("L2"));
        QCOMPARE(again["radius"].get<double>(), 2.5);
        QVERIFY(!first.contains("coverage"));
        QCOMPARE(again["coverage"], (Json{{"src", "selection"}}));
    }

    void default_and_swap_colors() {
        fresh_doc(32, 32);
        ToolState* t = w_->tools();
        t->set_fg(QColor(200, 40, 30));
        t->set_bg(QColor(20, 60, 220));
        key(Qt::Key_X);
        QCOMPARE(t->fg(), QColor(20, 60, 220));
        QCOMPARE(t->bg(), QColor(200, 40, 30));
        key(Qt::Key_X);
        QCOMPARE(t->fg(), QColor(200, 40, 30));
        key(Qt::Key_D);
        QCOMPARE(t->fg(), QColor(Qt::black));
        QCOMPARE(t->bg(), QColor(Qt::white));
        key(Qt::Key_X);
        QCOMPARE(t->fg(), QColor(Qt::white));
        QCOMPARE(t->bg(), QColor(Qt::black));
        QCOMPARE(s()->applied_count(), size_t{0});  // colours are tool state, not document edits
    }

    void move_tool_arrow_nudge() {
        fresh_doc(80, 60);
        QVERIFY(s()->apply({{"op", "add_layer"}, {"id", "P"}, {"fill", "solid"}, {"color", "#10A040FF"}, {"rect", {20, 20, 10, 10}}}, "p").ok());
        s()->set_active_layer("P");
        key(Qt::Key_V);
        QCOMPARE(w_->tools()->tool(), Tool::Move);
        const auto alpha = [this](int x, int y) {
            const auto px = c()->composite_pixel(x, y);
            return px ? px->alpha() : -1;
        };
        const auto op_translate = [this] {
            const Json& op = s()->entries()[s()->applied_count() - 1].op;
            return std::make_pair(op["translate"][0].get<int>(), op["translate"][1].get<int>());
        };
        QCOMPARE(alpha(20, 20), 255);
        size_t n = s()->applied_count();
        canvas_key(Qt::Key_Right);
        QCOMPARE(s()->applied_count(), n + 1);
        QCOMPARE(s()->entries()[n].label, QStringLiteral("Nudge"));
        QCOMPARE(op_translate(), std::make_pair(1, 0));
        QCOMPARE(alpha(20, 20), 0);   // left column vacated
        QCOMPARE(alpha(30, 20), 255); // right edge moved in
        canvas_key(Qt::Key_Down, Qt::ShiftModifier);
        QCOMPARE(s()->applied_count(), n + 2);
        QCOMPARE(op_translate(), std::make_pair(0, 10));
        QCOMPARE(alpha(21, 25), 0);
        QCOMPARE(alpha(21, 35), 255);
        canvas_key(Qt::Key_Left);
        canvas_key(Qt::Key_Up);
        QCOMPARE(s()->applied_count(), n + 4);
        QCOMPARE(op_translate(), std::make_pair(0, -1));
        // Undo walks back one nudge at a time.
        s()->undo(4);
        QCOMPARE(alpha(20, 20), 255);
        // With another tool the arrows do not move the layer.
        key(Qt::Key_B);
        n = s()->applied_count();
        canvas_key(Qt::Key_Right);
        QCOMPARE(s()->applied_count(), n);
    }

    void brush_options_bar() {
        fresh_doc(64, 64);
        key(Qt::Key_B);
        QCOMPARE(w_->tools()->tool(), Tool::Brush);
        BrushSettings* b = w_->tools()->brush_settings_for(Tool::Brush);
        QVERIFY(b);
        const struct {
            const char* name;
            const char* caption;
            const char* must_explain;
            double BrushSettings::*field;
        } fields[] = {{"BrushHardness", "Hardness", "edge", &BrushSettings::hardness},
                      {"BrushSpacing", "Spacing", "diameter", &BrushSettings::spacing},
                      {"BrushOpacity", "Opacity", "stroke", &BrushSettings::opacity},
                      {"BrushFlow", "Flow", "dab", &BrushSettings::flow}};
        for (const auto& f : fields) {
            auto* sb = w_->findChild<QSpinBox*>(QLatin1String(f.name));
            QVERIFY2(sb && sb->isVisible(), f.name);
            QCOMPARE(sb->suffix(), QStringLiteral(" %"));
            QVERIFY2(sb->toolTip().contains(QLatin1String(f.must_explain), Qt::CaseInsensitive), qPrintable(sb->toolTip()));
            // The caption shows the same explanation.
            QLabel* cap = nullptr;
            for (QLabel* l : w_->findChildren<QLabel*>())
                if (l->buddy() == sb) cap = l;
            QVERIFY2(cap && cap->text() == QLatin1String(f.caption), f.name);
            QCOMPARE(cap->toolTip(), sb->toolTip());
            // Field -> settings, and settings -> field.
            sb->setValue(37);
            QCOMPARE(b->*(f.field), 0.37);
            b->*(f.field) = 0.5;
            w_->tools()->notify_settings();
            QCOMPARE(sb->value(), 50);
        }
        auto* mode = w_->findChild<QWidget*>(QStringLiteral("BrushMode"));
        QVERIFY(mode && mode->toolTip().contains(QLatin1String("Wash")) && mode->toolTip().contains(QLatin1String("Build-up")));
    }

    void cleanupTestCase() { w_.reset(); }
};

QTEST_MAIN(GuiNavigation)
#include "gui_navigation.moc"
