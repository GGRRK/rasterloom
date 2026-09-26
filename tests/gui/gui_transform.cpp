// SPDX-License-Identifier: GPL-3.0-or-later
//
// Free Transform (Ctrl+T) with live preview on a layer copy: the params form (move / scale), then
// the quad form (Ctrl-drag a corner = distort, Ctrl+Shift = skew, Ctrl+Alt+Shift = perspective).
// The document is untouched until Enter; the preview after the release is byte-identical to the
// layer after Enter applies ONE transform op; the op replays through the CLI; Esc cancels and an
// edit arriving mid-transform (undo) abandons it.
#include <QApplication>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

#include <fstream>

#include "core/io/export_png.hpp"
#include "core/io/png.hpp"
#include "gui/canvas.hpp"
#include "gui/canvas_widgets.hpp"
#include "gui/session.hpp"
#include "gui/tools.hpp"

using namespace rl::gui;

namespace {

std::vector<uint8_t> bytes_of(const rl::RgbaImage& im) {
    std::vector<uint8_t> v;
    v.reserve(static_cast<size_t>(im.width()) * static_cast<size_t>(im.height()) * 4);
    for (int y = 0; y < im.height(); ++y)
        for (int x = 0; x < im.width(); ++x) {
            const rl::Rgba8 p = im.get(x, y);
            v.insert(v.end(), {p.r, p.g, p.b, p.a});
        }
    return v;
}

std::vector<uint8_t> png_bytes(const QString& p) {
    const rl::io::RgbaBuffer b = rl::io::read_png(p.toStdString());
    std::vector<uint8_t> v;
    for (int y = 0; y < b.h; ++y)
        for (int x = 0; x < b.w; ++x) {
            const rl::Rgba8 q = b.at(x, y);
            v.insert(v.end(), {q.r, q.g, q.b, q.a});
        }
    return v;
}

}  // namespace

class GuiTransform : public QObject {
    Q_OBJECT
    EditorSession s_;
    ToolState tools_;
    CanvasController* c_ = nullptr;
    CanvasRasterWidget* w_ = nullptr;
    QTemporaryDir tmp_;

    const rl::RgbaImage& layer() { return s_.doc().require_raster("L", "t").pixels; }
    QPoint wp(double x, double y) { return c_->to_widget(QPointF(x, y)).toPoint(); }
    void drag(QPointF a, QPointF b, Qt::KeyboardModifiers m) {
        QTest::mousePress(w_, Qt::LeftButton, m, wp(a.x(), a.y()));
        for (int i = 1; i <= 6; ++i) {
            const QPointF p = a + (b - a) * (i / 6.0);
            QMouseEvent e(QEvent::MouseMove, QPointF(wp(p.x(), p.y())), w_->mapToGlobal(QPointF(wp(p.x(), p.y()))), Qt::NoButton,
                          Qt::LeftButton, m);
            QCoreApplication::sendEvent(w_, &e);
        }
        QTest::mouseRelease(w_, Qt::LeftButton, m, wp(b.x(), b.y()));
        QCoreApplication::processEvents();
    }
    void key(Qt::Key k) {
        QKeyEvent e(QEvent::KeyPress, k, Qt::NoModifier);
        QVERIFY(c_->key_press(&e));
    }
    void check_cli_replay() {
        const QString sp = tmp_.filePath(QStringLiteral("s.json")), out = tmp_.filePath(QStringLiteral("cli.png")),
                      gui = tmp_.filePath(QStringLiteral("gui.png"));
        {
            std::ofstream f(sp.toStdString());
            f << s_.session_script()->dump(1);
        }
        QProcess p;
        p.start(QStringLiteral(RL_CLI), {QStringLiteral("--render-script"), sp, QStringLiteral("--out"), out});
        QVERIFY(p.waitForFinished(60000));
        QVERIFY2(p.exitCode() == 0, qPrintable(QString::fromLocal8Bit(p.readAllStandardError())));
        rl::io::write_document_png(s_.state(), gui.toStdString());
        QVERIFY(png_bytes(out) == png_bytes(gui));
    }

private slots:
    void initTestCase() {
        QVERIFY(tmp_.isValid());
        s_.new_document(240, 180, 1);
        QVERIFY(s_.apply({{"op", "add_layer"}, {"id", "L"}, {"fill", "gradient"}, {"from", "#FF4020FF"}, {"to", "#2040FFFF"},
                          {"rect", {60, 50, 100, 70}}},
                         "grad")
                    .ok());
        s_.set_active_layer("L");
        c_ = new CanvasController(&s_, &tools_, this);
        w_ = new CanvasRasterWidget(c_);
        c_->set_widget(w_);
        w_->resize(360, 280);
        w_->show();
        QVERIFY(QTest::qWaitForWindowExposed(w_));
        c_->sync_document();
        c_->set_zoom(1.0, QPointF(0, 0));
        c_->center_on(QPointF(120, 90));
        tools_.settings().transform_bicubic = true;
    }

    void distort_skew_perspective_commit() {
        const std::vector<uint8_t> before = bytes_of(layer());
        const size_t hist = s_.applied_count();
        c_->begin_free_transform();
        QVERIFY(c_->has_pending());
        QVERIFY(!c_->transform_quad_mode());
        // Params form first: move by (+10, +6).
        drag(QPointF(110, 85), QPointF(120, 91), Qt::NoModifier);
        QVERIFY(bytes_of(layer()) == before);  // the document is untouched
        QVERIFY(c_->transform_preview_pixels());
        QVERIFY(bytes_of(*c_->transform_preview_pixels()) != before);
        // Ctrl-drag the top-right corner (box 60,50..160,120 moved by 10,6): distort.
        drag(QPointF(170, 56), QPointF(185, 40), Qt::ControlModifier);
        QVERIFY(c_->transform_quad_mode());
        // Ctrl+Shift on the bottom edge midpoint: skew (the edge slides along itself).
        drag(QPointF(120, 126), QPointF(135, 140), Qt::ControlModifier | Qt::ShiftModifier);
        // Ctrl+Alt+Shift on the bottom-left corner: perspective (its partner mirrors).
        drag(QPointF(85, 126), QPointF(75, 126), Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier);
        QVERIFY(bytes_of(layer()) == before);
        const std::vector<uint8_t> preview = bytes_of(*c_->transform_preview_pixels());
        qInfo("final preview rendered in %.2f ms", c_->last_transform_preview_ms());
        key(Qt::Key_Return);
        QVERIFY(!c_->has_pending());
        QCOMPARE(s_.applied_count(), hist + 1);
        const Json op = s_.entries().back().op;
        QCOMPARE(QString::fromStdString(op["op"].get<std::string>()), QStringLiteral("transform"));
        QVERIFY(op.contains("matrix"));
        QVERIFY2(bytes_of(layer()) == preview, "the committed transform must equal the live preview");
        QVERIFY(&c_->display_state() == &s_.state());  // the override is gone
        check_cli_replay();
    }

    void skew_and_perspective_geometry() {
        c_->begin_free_transform();
        const auto q0 = c_->transform_corners();
        const QPointF tl = q0[0], tr = q0[1], br = q0[2], bl = q0[3];
        QVERIFY(std::abs(tr.y() - tl.y()) < 1e-9);  // an axis-aligned box to start from
        const auto near = [](QPointF a, QPointF b) { return std::abs(a.x() - b.x()) < 1.01 && std::abs(a.y() - b.y()) < 1.01; };
        // Perspective on the top-left corner, dragged right along the top edge: TR mirrors it.
        drag(tl, tl + QPointF(20, 3), Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier);
        QVERIFY(c_->transform_quad_mode());
        auto q = c_->transform_corners();
        QVERIFY2(near(q[0], tl + QPointF(20, 0)), "corner slides along its edge");
        QVERIFY2(near(q[1], tr - QPointF(20, 0)), "the partner corner mirrors it (perspective)");
        QVERIFY(near(q[2], br) && near(q[3], bl));
        // Skew on the bottom edge midpoint: both bottom corners slide along the bottom edge.
        const QPointF mid = (q[2] + q[3]) / 2.0;
        drag(mid, mid + QPointF(15, 9), Qt::ControlModifier | Qt::ShiftModifier);
        const auto q2 = c_->transform_corners();
        QVERIFY(near(q2[2], q[2] + QPointF(15, 0)) && near(q2[3], q[3] + QPointF(15, 0)));
        QVERIFY(near(q2[0], q[0]) && near(q2[1], q[1]));
        // Distort: one corner moves freely.
        drag(q2[2], q2[2] + QPointF(-7, 11), Qt::ControlModifier);
        const auto q3 = c_->transform_corners();
        QVERIFY(near(q3[2], q2[2] + QPointF(-7, 11)));
        // A drag that would fold the quad stops at the last convex shape.
        drag(q3[0], q3[2] + QPointF(10, 10), Qt::ControlModifier);
        const auto f = c_->transform_corners();
        double sign = 0;
        for (size_t i = 0; i < 4; ++i) {
            const QPointF u = f[(i + 1) % 4] - f[i], v = f[(i + 2) % 4] - f[(i + 1) % 4];
            const double cr = u.x() * v.y() - u.y() * v.x();
            QVERIFY(cr != 0.0 && (sign == 0.0 || (cr > 0) == (sign > 0)));
            sign = cr;
        }
        QVERIFY(!near(f[0], q3[2] + QPointF(10, 10)));
        key(Qt::Key_Escape);
        QVERIFY(!c_->has_pending());
    }

    void escape_and_undo_cancel() {
        const std::vector<uint8_t> before = bytes_of(layer());
        const size_t hist = s_.applied_count();
        c_->begin_free_transform();
        drag(QPointF(120, 90), QPointF(140, 100), Qt::NoModifier);
        QVERIFY(c_->transform_preview_pixels());
        key(Qt::Key_Escape);
        QVERIFY(!c_->has_pending());
        QCOMPARE(s_.applied_count(), hist);
        QVERIFY(bytes_of(layer()) == before);
        QVERIFY(&c_->display_state() == &s_.state());
        // Undo while transforming abandons the transform first.
        c_->begin_free_transform();
        drag(QPointF(120, 90), QPointF(100, 80), Qt::NoModifier);
        s_.undo();
        QVERIFY(!c_->has_pending());
        QCOMPARE(s_.applied_count(), hist - 1);
    }
};

QTEST_MAIN(GuiTransform)
#include "gui_transform.moc"
