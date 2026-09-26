// SPDX-License-Identifier: GPL-3.0-or-later
//
// The canvas as the application hosts it: the real MainWindow, its ADS canvas dock and, for the GL
// backend, CanvasGLWidget::paintGL including the QPainter overlay pass. Regression tests for
// 5c8e9df: ADS wrapped the canvas in a QScrollArea, QScrollArea::setWidget() turned
// autoFillBackground on, and on a QOpenGLWidget every QPainter::begin() then cleared the frame the
// GL pass had just drawn, so every real GPU showed an empty workspace. gui_gl_canvas.cpp draws into
// its own framebuffer object through test hooks and never saw it.
//
//  - canvas_hosting / late_gl_failure_hosting: structural, any platform (offscreen without a
//    display too): the canvas widget of either backend, and the raster widget swapped in after a
//    late GL failure, has no scroll-area ancestor and autoFillBackground() false.
//  - gl_widget_frame: RASTERLOOM_CANVAS=gl, a document with known colours, the canvas settled
//    (display_complete), QOpenGLWidget::grabFramebuffer() (paintGL plus the overlay painter)
//    compared with the known colours and with the raster path of the same controller. On the xcb
//    platform the window as the X server shows it (GetImage) is compared too, where the server can
//    read a GL window back (Xvfb, rootful Xwayland; rootless Xwayland returns black and the log
//    says the check did not run). QSKIPs without a GL 3.3 core context.
//
// The xcb platform opens a real (if throwaway) window, so main() allows it only with RL_TEST_XCB=1
// (CI's Xvfb step, headless gamescope); otherwise it falls back to offscreen and every test QSKIPs.
#include <QAbstractScrollArea>
#include <QApplication>
#include <QDialog>
#include <QPainter>
#include <QPointer>
#include <QScreen>
#include <QSurfaceFormat>
#include <QTest>

#include <DockManager.h>
#include <DockWidget.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "gui/canvas.hpp"
#include "gui/canvas_gl.hpp"
#include "gui/canvas_widgets.hpp"
#include "gui/main_window.hpp"
#include "gui/session.hpp"
#include "gui/theme.hpp"

using namespace rl::gui;

namespace {

QString g_platform_skip;  // set by main() when the requested platform is not allowed here

// A 480 x 320 document: red everywhere, the right half blue, the bottom-left quarter green (three
// opaque solid layers, so the composite is exactly these colours and a flipped or shifted frame
// shows up).
constexpr int kW = 480, kH = 320;
const QRgb kRed = qRgb(0xD0, 0x28, 0x18), kBlue = qRgb(0x18, 0x68, 0xE0), kGreen = qRgb(0x30, 0xC0, 0x40);

QRgb expected_at(int x, int y) {
    if (x >= kW / 2) return kBlue;
    return y >= kH / 2 ? kGreen : kRed;
}

// Distance of a canvas pixel to the nearest colour boundary or canvas edge (in canvas pixels).
int boundary_distance(int x, int y) {
    const int dx = x < kW / 2 ? std::min(x, kW / 2 - 1 - x) : std::min(x - kW / 2, kW - 1 - x);
    const int dy = x < kW / 2 ? (y < kH / 2 ? std::min(y, kH / 2 - 1 - y) : std::min(y - kH / 2, kH - 1 - y))
                              : std::min(y, kH - 1 - y);
    return std::min(dx, dy);
}

void make_doc(MainWindow& w) {
    w.new_document(kW, kH, 1);
    EditorSession* s = w.session();
    QVERIFY(s->apply({{"op", "add_layer"}, {"id", "R"}, {"fill", "solid"}, {"color", "#D02818FF"}}, "red").ok());
    QVERIFY(s->apply({{"op", "add_layer"}, {"id", "B"}, {"fill", "solid"}, {"color", "#1868E0FF"},
                      {"rect", {kW / 2, 0, kW / 2, kH}}},
                     "blue")
                .ok());
    QVERIFY(s->apply({{"op", "add_layer"}, {"id", "G"}, {"fill", "solid"}, {"color", "#30C040FF"},
                      {"rect", {0, kH / 2, kW / 2, kH / 2}}},
                     "green")
                .ok());
}

// Every scroll area between the canvas widget and its window, as "Class(objectName) < ...".
QString scroll_area_ancestors(const QWidget* w) {
    QStringList found;
    for (const QWidget* p = w->parentWidget(); p; p = p->parentWidget())
        if (qobject_cast<const QAbstractScrollArea*>(p))
            found << QStringLiteral("%1(%2)").arg(QString::fromLatin1(p->metaObject()->className()), p->objectName());
    return found.join(QStringLiteral(" < "));
}

void verify_hosting(MainWindow& w, QWidget* cw) {
    QVERIFY(cw);
    QCOMPARE(w.canvas()->widget(), cw);
    ads::CDockWidget* dock = w.dock_manager()->findDockWidget(QStringLiteral("CanvasDock"));
    QVERIFY(dock);
    QCOMPARE(dock->widget(), cw);
    const QString scrolls = scroll_area_ancestors(cw);
    QVERIFY2(scrolls.isEmpty(), qPrintable(QStringLiteral("the canvas is wrapped in a scroll area: ") + scrolls));
    // On a QOpenGLWidget this makes every QPainter::begin() clear the GL frame.
    QVERIFY2(!cw->autoFillBackground(), "the canvas widget has autoFillBackground on");
    QVERIFY(cw->window() == &w);
}

struct FrameCheck {
    long checked = 0, wrong = 0;
    QString first;  // the first mismatch, for the failure message
};

// Compares the canvas interior of a widget-sized frame with the known colours (100 % zoom:
// nearest sampling, exact), skipping 2 canvas pixels around every colour boundary.
FrameCheck check_frame(const QImage& frame, const CanvasController& c) {
    FrameCheck r;
    const QImage f = frame.convertToFormat(QImage::Format_RGB32);
    for (int y = 0; y < f.height(); ++y)
        for (int x = 0; x < f.width(); ++x) {
            const QPointF cp = c.to_canvas(QPointF(x + 0.5, y + 0.5));
            const int cx = static_cast<int>(std::floor(cp.x())), cy = static_cast<int>(std::floor(cp.y()));
            if (cx < 0 || cy < 0 || cx >= kW || cy >= kH || boundary_distance(cx, cy) < 2) continue;
            ++r.checked;
            const QRgb got = f.pixel(x, y) & 0xffffffu, want = expected_at(cx, cy) & 0xffffffu;
            if (got != want && r.wrong++ == 0)
                r.first = QStringLiteral("widget (%1, %2) = canvas (%3, %4): got #%5, want #%6")
                              .arg(x)
                              .arg(y)
                              .arg(cx)
                              .arg(cy)
                              .arg(got, 6, 16, QLatin1Char('0'))
                              .arg(want, 6, 16, QLatin1Char('0'));
        }
    return r;
}

void dump(const QImage& img, const char* name) {
    if (const char* dir = std::getenv("RL_GL_DUMP"))
        img.save(QStringLiteral("%1/%2.png").arg(QString::fromLocal8Bit(dir), QString::fromLatin1(name)));
}

}  // namespace

class GuiGlWidget : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        if (!g_platform_skip.isEmpty()) QSKIP(qPrintable(g_platform_skip));
        qInfo("platform %s", qPrintable(QGuiApplication::platformName()));
    }

    void cleanup() { qunsetenv("RASTERLOOM_CANVAS"); }

    void canvas_hosting_data() {
        QTest::addColumn<QByteArray>("backend");
        QTest::newRow("raster") << QByteArray("raster");
        QTest::newRow("gl") << QByteArray("gl");
    }

    // Both backends (the GL widget is constructed even where no GL exists; it is never shown here).
    void canvas_hosting() {
        QFETCH(QByteArray, backend);
        qputenv("RASTERLOOM_CANVAS", backend);
        MainWindow w;
        w.set_confirm_close(false);
        QWidget* cw = w.canvas_widget();
        if (backend == "gl") QVERIFY2(qobject_cast<CanvasGLWidget*>(cw), "RASTERLOOM_CANVAS=gl must host CanvasGLWidget");
        else QVERIFY2(qobject_cast<CanvasRasterWidget*>(cw), "RASTERLOOM_CANVAS=raster must host CanvasRasterWidget");
        verify_hosting(w, cw);
    }

    // The raster widget that replaces a GL canvas whose initializeGL failed is docked the same way.
    void late_gl_failure_hosting() {
        qputenv("RASTERLOOM_CANVAS", "gl");
        MainWindow w;
        w.set_confirm_close(false);
        QPointer<CanvasGLWidget> gl = qobject_cast<CanvasGLWidget*>(w.canvas_widget());
        QVERIFY(gl);
        emit gl->gl_failed(QStringLiteral("injected by gui_gl_widget"));
        QTRY_VERIFY(qobject_cast<CanvasRasterWidget*>(w.canvas_widget()));
        verify_hosting(w, w.canvas_widget());
        QTRY_VERIFY(gl.isNull());
    }

    void gl_widget_frame() {
        const GlProbe p = probe_gl();
        if (!p.ok) QSKIP(qPrintable(QStringLiteral("no usable GL 3.3 core context: ") + p.reason));
        qInfo("GL: %s | %s | %s", qPrintable(p.vendor), qPrintable(p.renderer), qPrintable(p.version));

        qputenv("RASTERLOOM_CANVAS", "gl");
        MainWindow w;
        w.set_confirm_close(false);
        w.resize(1200, 800);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        auto* gl = qobject_cast<CanvasGLWidget*>(w.canvas_widget());
        QVERIFY2(gl, "RASTERLOOM_CANVAS=gl must host CanvasGLWidget");
        make_doc(w);
        CanvasController* c = w.canvas();
        QVERIFY2(gl->width() > kW + 20 && gl->height() > kH + 20, "the canvas dock must fit the document at 100 %");
        c->set_zoom(1.0, QPointF(0, 0));
        c->center_on(QPointF(kW / 2.0, kH / 2.0));

        // Let the canvas settle: each grab runs paintGL (render_gl + the overlay painter) and
        // reports through frame_end() whether every visible tile was current.
        QImage frame;
        for (int i = 0; i < 1000; ++i) {
            QCoreApplication::processEvents();
            frame = gl->grabFramebuffer();
            if (c->display_complete()) break;
            QTest::qWait(5);
        }
        QVERIFY2(c->display_complete(), "the canvas never finished displaying");
        QVERIFY2(gl->gl_ok(), "CanvasGLWidget failed to initialise in its own context");
        QCOMPARE(qobject_cast<CanvasGLWidget*>(w.canvas_widget()), gl);  // no late fallback to raster
        QCOMPARE(frame.size(), gl->size() * gl->devicePixelRatio());
        dump(frame, "gl-widget-frame");

        // The raster path of the same controller and view (CanvasRasterWidget::paintEvent).
        QImage ref(gl->size(), QImage::Format_ARGB32_Premultiplied);
        ref.fill(Qt::black);
        {
            QPainter rp(&ref);
            c->frame_begin();
            c->paint_raster(rp, ref.rect());
            c->paint_overlays(rp);
            c->frame_end();
        }
        dump(ref, "gl-widget-raster-ref");

        const FrameCheck fr = check_frame(frame, *c);
        const FrameCheck rr = check_frame(ref, *c);
        qInfo("grabFramebuffer: %ld of %ld canvas pixels wrong; raster reference: %ld of %ld", fr.wrong, fr.checked, rr.wrong,
              rr.checked);
        QVERIFY2(fr.checked > (kW - 8) * (kH - 8) / 2, "too few canvas pixels in view");
        QCOMPARE(rr.wrong, 0L);  // the reference itself
        QVERIFY2(fr.wrong == 0, qPrintable(QStringLiteral("the GL widget frame does not show the document: %1 of %2 pixels wrong, first %3")
                                               .arg(fr.wrong)
                                               .arg(fr.checked)
                                               .arg(fr.first)));
        // Workspace around the canvas, as the raster path paints it.
        const QPoint corner(2, 2);
        QVERIFY(!c->canvas_rect_widget().contains(QPointF(corner) + QPointF(0.5, 0.5)));
        QCOMPARE(frame.convertToFormat(QImage::Format_RGB32).pixel(corner), ref.convertToFormat(QImage::Format_RGB32).pixel(corner));
        QCOMPARE(QColor(frame.pixel(corner)), theme::kWorkspace);

        // xcb: what the window system shows (the backing store's composition of the GL texture),
        // read back with GetImage. Servers that cannot read back a GL-presented window (Xwayland
        // with a DRI3 driver) return all black for the whole window; the check needs a readable one
        // (Xvfb, swrast) and says in the log which case it met.
        if (QGuiApplication::platformName() == QLatin1String("xcb")) {
            const QImage whole = w.screen()->grabWindow(w.winId()).toImage().convertToFormat(QImage::Format_RGB32);
            bool readable = false;
            for (int y = 0; y < whole.height() && !readable; ++y)
                for (int x = 0; x < whole.width() && !readable; ++x) readable = (whole.pixel(x, y) & 0xffffffu) != 0;
            if (!readable) {
                qInfo("on screen: not checked, this X server reads the GL window back as all black (%dx%d)", whole.width(),
                      whole.height());
                return;
            }
            const QPoint at = gl->mapTo(&w, QPoint(0, 0));
            FrameCheck sr;
            QImage shown;
            for (int i = 0; i < 100; ++i) {
                gl->update();
                QTest::qWait(50);
                shown = w.screen()->grabWindow(w.winId(), at.x(), at.y(), gl->width(), gl->height()).toImage();
                sr = check_frame(shown, *c);
                if (sr.wrong == 0 && sr.checked == fr.checked) break;
            }
            dump(shown, "gl-widget-on-screen");
            qInfo("on screen: %ld of %ld canvas pixels wrong", sr.wrong, sr.checked);
            QCOMPARE(sr.checked, fr.checked);
            QVERIFY2(sr.wrong == 0, qPrintable(QStringLiteral("the window does not show the document: %1 of %2 pixels wrong, first %3")
                                                   .arg(sr.wrong)
                                                   .arg(sr.checked)
                                                   .arg(sr.first)));
        }
    }
};

int main(int argc, char** argv) {
    // As src/gui/main.cpp: GL 3.3 core as the default format, set before QApplication.
    QSurfaceFormat fmt;
    fmt.setVersion(3, 3);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    QSurfaceFormat::setDefaultFormat(fmt);
    if (qgetenv("QT_QPA_PLATFORM") == "xcb" && qgetenv("RL_TEST_XCB") != "1") {
        g_platform_skip = QStringLiteral("xcb needs RL_TEST_XCB=1 and a throwaway X server (Xvfb, headless gamescope)");
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication app(argc, argv);
    QCoreApplication::setAttribute(Qt::AA_CompressHighFrequencyEvents, false);
    theme::apply(app);
    GuiGlWidget t;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&t, argc, argv);
}

#include "gui_gl_widget.moc"
