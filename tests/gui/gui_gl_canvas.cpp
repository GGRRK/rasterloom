// SPDX-License-Identifier: GPL-3.0-or-later
//
// GL canvas vs raster canvas (docs/research/qt-platform.md section 2): the same document and view
// drawn by CanvasGLWidget (texture-array tiles, BGRA words per pyramid-level slot) and by
// CanvasRasterWidget (QPainter) must agree within a small tolerance (display only, not
// byte-exact). The offscreen QPA has no GL widgets, so the GL path renders into a framebuffer
// object of a headless context through CanvasGLWidget's test hooks; the test QSKIPs when no GL 3.3
// core context can be created at all.
//
// Also: the CPU mip pyramid is a premultiplied 2x2 box filter of the level below, and zoom < 100 %
// draws from it on both paths.
#include <QApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QTest>

#include <cstdlib>

#include "gui/canvas.hpp"
#include "gui/canvas_gl.hpp"
#include "gui/canvas_widgets.hpp"
#include "gui/diagnostics.hpp"
#include "gui/session.hpp"
#include "gui/theme.hpp"
#include "gui/tools.hpp"

using namespace rl::gui;

namespace {

void make_doc(EditorSession& s, int w, int h) {
    s.new_document(w, h, 1);
    QVERIFY(s.apply({{"op", "add_layer"}, {"id", "G"}, {"fill", "gradient"}, {"from", "#FF000080"}, {"to", "#0000FFFF"}}, "g").ok());
    QVERIFY(s.apply({{"op", "add_layer"}, {"id", "C"}, {"fill", "checker"}, {"cell", 7}, {"a", "#20C040FF"}, {"b", "#00000000"},
                     {"rect", {40, 30, 150, 100}}},
                    "c")
                .ok());
}

}  // namespace

class GuiGlCanvas : public QObject {
    Q_OBJECT
private slots:
    void pyramid_is_premultiplied_box_filter() {
        EditorSession s;
        ToolState tools;
        make_doc(s, 300, 200);
        CanvasController c(&s, &tools);
        CanvasRasterWidget w(&c);
        c.set_widget(&w);
        w.resize(420, 300);
        c.sync_document();
        QVERIFY(c.level_count() >= 3);
        QCOMPARE(CanvasController::level_for_zoom(1.0), 0);
        QCOMPARE(CanvasController::level_for_zoom(0.5), 1);
        QCOMPARE(CanvasController::level_for_zoom(0.3), 1);
        QCOMPARE(CanvasController::level_for_zoom(0.25), 2);
        QCOMPARE(CanvasController::level_for_zoom(0.02), 5);
        // Level 1 tile (1, 0) covers canvas x 128..255 = level-0 tiles (2, 0), (3, 0), (2, 1), (3, 1).
        const QImage* l1 = c.level_tile(1, 1, 0);
        QVERIFY(l1);
        for (int y = 0; y < 64; y += 5)
            for (int x = 0; x < 64; x += 3) {
                const int cx = 128 + 2 * x, cy = 2 * y;
                const auto px = [&](int X, int Y) {
                    return reinterpret_cast<const QRgb*>(c.display_tile(X / 64, Y / 64).constScanLine(Y % 64))[X % 64];
                };
                const QRgb a = px(cx, cy), b = px(cx + 1, cy), d = px(cx, cy + 1), e = px(cx + 1, cy + 1);
                for (int sh : {0, 8, 16, 24}) {
                    const unsigned want = (((a >> sh) & 0xff) + ((b >> sh) & 0xff) + ((d >> sh) & 0xff) + ((e >> sh) & 0xff) + 2) >> 2;
                    const unsigned got = (reinterpret_cast<const QRgb*>(l1->constScanLine(y))[x] >> sh) & 0xff;
                    QCOMPARE(got, want);
                }
            }
        // An edit invalidates the level-0 tile and every ancestor.
        const quint64 g1 = c.tile_generation(1, 1, 0);
        QVERIFY(s.apply({{"op", "select_rect"}, {"x", 140}, {"y", 10}, {"w", 20}, {"h", 20}, {"mode", "new"}}, "sel").ok());
        QVERIFY(s.apply({{"op", "fill_selection"}, {"layer", "C"}, {"color", "#FFFFFFFF"}, {"opacity", 1.0}}, "fill").ok());
        c.level_tile(1, 1, 0);
        QVERIFY(c.tile_generation(1, 1, 0) != g1);
    }

    void gl_matches_raster() {
        const GlProbe p = probe_gl();
        if (!p.ok) QSKIP(qPrintable(QStringLiteral("no usable GL 3.3 core context: ") + p.reason));
        qInfo("GL: %s | %s | %s", qPrintable(p.vendor), qPrintable(p.renderer), qPrintable(p.version));

        EditorSession s;
        ToolState tools;
        make_doc(s, 900, 640);
        CanvasController gc(&s, &tools), rc(&s, &tools);
        CanvasGLWidget glw(&gc);
        gc.set_widget(&glw);
        CanvasRasterWidget rw(&rc);
        rc.set_widget(&rw);
        const QSize view(420, 300);
        glw.resize(view);
        rw.resize(view);
        gc.sync_document();
        rc.sync_document();

        QOpenGLContext ctx;
        QSurfaceFormat f;
        f.setVersion(3, 3);
        f.setProfile(QSurfaceFormat::CoreProfile);
        ctx.setFormat(f);
        QVERIFY(ctx.create());
        QOffscreenSurface surf;
        surf.setFormat(ctx.format());
        surf.create();
        QVERIFY(ctx.makeCurrent(&surf));
        QOpenGLFramebufferObject fbo(view);
        QVERIFY(fbo.bind());
        QVERIFY(glw.init_for_test(&ctx));
        ctx.functions()->glViewport(0, 0, view.width(), view.height());

        struct Case {
            double zoom;
            int tolerance;         // per-pixel max channel difference ...
            double max_fraction;   // ... allowed on at most this fraction of the pixels
        };
        // 100 % and 50 % map pixels 1:1 (nearest): identical. 30 % / 13 % minify a level
        // bilinearly, where QPainter and GL sample at slightly different phases: the 7-px checker
        // (a worst case: about 2 screen pixels per cell at 30 %) differs on its edges.
        for (const Case cs : {Case{1.0, 0, 0.0}, Case{0.5, 0, 0.0}, Case{0.3, 40, 0.02}, Case{0.13, 40, 0.02}}) {
            for (CanvasController* c : {&gc, &rc}) c->set_zoom(cs.zoom, QPointF(0, 0)), c->center_on(QPointF(450, 320));
            gc.finish_display();
            rc.finish_display();
            glw.render_for_test(view.width(), view.height());
            const QImage g = fbo.toImage().convertToFormat(QImage::Format_RGB32);
            const QImage r = rw.grab().toImage().convertToFormat(QImage::Format_RGB32);
            QCOMPARE(g.size(), r.size());
            if (const char* dump = std::getenv("RL_GL_DUMP")) {
                g.save(QStringLiteral("%1/gl-%2.png").arg(QString::fromLocal8Bit(dump)).arg(cs.zoom));
                r.save(QStringLiteral("%1/raster-%2.png").arg(QString::fromLocal8Bit(dump)).arg(cs.zoom));
            }
            // Compare inside the canvas rect, where both draw checker + tiles.
            const QRect cr = gc.canvas_rect_widget().toAlignedRect().adjusted(2, 2, -2, -2).intersected(QRect(QPoint(0, 0), view));
            int worst = 0;
            long over = 0, total = 0;
            double sum = 0;
            for (int y = cr.top(); y <= cr.bottom(); ++y)
                for (int x = cr.left(); x <= cr.right(); ++x) {
                    const QRgb a = g.pixel(x, y), b = r.pixel(x, y);
                    const int d = std::max({std::abs(qRed(a) - qRed(b)), std::abs(qGreen(a) - qGreen(b)), std::abs(qBlue(a) - qBlue(b))});
                    worst = std::max(worst, d);
                    sum += d;
                    ++total;
                    if (d > cs.tolerance) ++over;
                }
            qInfo("zoom %.2f (level %d): worst channel difference %d, mean %.3f, pixels over %d: %ld of %ld", cs.zoom,
                  gc.display_level(), worst, sum / double(total), cs.tolerance, over, total);
            QVERIFY2(total > 1000, "the canvas must cover the view");
            QVERIFY2(double(over) <= cs.max_fraction * double(total), "GL canvas deviates from the raster canvas");
            QVERIFY2(sum / double(total) < 1.5, "GL canvas deviates from the raster canvas on average");
        }
        glw.cleanup_for_test();
        fbo.release();
        ctx.doneCurrent();
    }
};

QTEST_MAIN(GuiGlCanvas)
#include "gui_gl_canvas.moc"
