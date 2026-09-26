// SPDX-License-Identifier: GPL-3.0-or-later
//
// GUI performance measurements (not a ctest; run by hand, numbers go into the lane report):
//
//   gui_bench brush            4000 x 3000 canvas, 200 px soft brush: engine dabs/s, end-to-end
//                              dabs/s with a synchronous repaint per event, dab-to-pixel latency.
//   gui_bench photo FILE       open a large photo in the real MainWindow (raster canvas): open time,
//                              fit-to-window render (first frame + until complete), a pan frame and
//                              a zoom step; plus the GL path through an FBO when a context exists.
//
// Always offscreen: QT_QPA_PLATFORM=offscreen RASTERLOOM_CANVAS=raster.
#include <QApplication>
#include <QElapsedTimer>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QTabletEvent>
#include <QTest>

#include <cmath>
#include <cstdio>

#include "gui/canvas.hpp"
#include "gui/canvas_gl.hpp"
#include "gui/canvas_widgets.hpp"
#include "gui/diagnostics.hpp"
#include "gui/main_window.hpp"
#include "gui/session.hpp"
#include "gui/tools.hpp"

using namespace rl::gui;

namespace {

double ms_since(const QElapsedTimer& t) { return static_cast<double>(t.nsecsElapsed()) / 1e6; }

void bench_brush(double spacing, double zoom) {
    EditorSession s;
    ToolState tools;
    s.new_document(4000, 3000, 0);  // white Background
    s.apply({{"op", "add_layer"}, {"id", "Paint"}}, "paint");
    s.set_active_layer("Paint");
    CanvasController c(&s, &tools);
    CanvasRasterWidget w(&c);
    c.set_widget(&w);
    w.resize(1400, 900);
    w.show();
    (void)QTest::qWaitForWindowExposed(&w);
    c.sync_document();
    if (zoom > 0) c.set_zoom(zoom, QPointF(0, 0));
    c.center_on(QPointF(2000, 1500));
    c.finish_display();
    w.repaint();
    tools.set_tool(Tool::Brush);
    BrushSettings& b = tools.settings().brush;
    b.size = 200;
    b.hardness = 0.0;
    b.spacing = spacing;
    b.flow = 1.0;
    b.opacity = 1.0;
    b.pressure_size = true;
    QPointingDevice pen(QStringLiteral("Bench Pen"), 77, QInputDevice::DeviceType::Stylus, QPointingDevice::PointerType::Pen,
                        QInputDevice::Capability::Position | QInputDevice::Capability::Pressure, 1, 3);
    const auto send = [&](QEvent::Type t, QPointF cpt, double pr, quint64 ts) {
        const QPointF lp = c.to_widget(cpt);
        QTabletEvent e(t, &pen, lp, w.mapToGlobal(lp), pr, 0.f, 0.f, 0.f, 0.0, 0.f, Qt::NoModifier,
                       t == QEvent::TabletMove ? Qt::NoButton : Qt::LeftButton, t == QEvent::TabletRelease ? Qt::NoButton : Qt::LeftButton);
        e.setTimestamp(ts);
        QCoreApplication::sendEvent(&w, &e);
    };
    // A zigzag across the visible canvas: 600 events, 4 ms apart (a 250 Hz pen).
    const int n = 600;
    const QRectF v = c.visible_canvas_rect().adjusted(150, 150, -150, -150);
    QElapsedTimer wall;
    wall.start();
    double paint_ms = 0;
    send(QEvent::TabletPress, v.topLeft(), 0.8, 1000);
    for (int i = 1; i <= n; ++i) {
        const double t = double(i) / n;
        const double x = v.left() + v.width() * t;
        const double y = v.top() + v.height() * (0.5 + 0.45 * std::sin(t * 18.0));
        send(QEvent::TabletMove, QPointF(x, y), 0.6 + 0.4 * std::sin(t * 7.0), 1000 + 4 * i);
        QElapsedTimer pt;
        pt.start();
        w.repaint();  // synchronous frame: the preview tiles are recomposited and drawn now
        paint_ms += ms_since(pt);
    }
    const double total = ms_since(wall);
    Diagnostics& d = diag();
    const qint64 dabs = d.stroke_dabs;
    std::printf("brush 200 px soft, spacing %.0f %%, zoom %.0f %% (level %d): %lld dabs over %d events\n", spacing * 100,
                c.zoom() * 100, c.display_level(), static_cast<long long>(dabs), n);
    std::printf("  engine only      : %.1f ms  -> %.0f dabs/s\n", d.stroke_engine_ms, dabs * 1000.0 / d.stroke_engine_ms);
    std::printf("  end to end       : %.1f ms  -> %.0f dabs/s (%.0f events/s, input + engine + tiles + paint)\n", total,
                dabs * 1000.0 / total, n * 1000.0 / total);
    std::printf("  paint per event  : %.2f ms mean\n", paint_ms / n);
    std::printf("  dab-to-pixel     : median %.2f ms, p95 %.2f ms, max %.2f ms\n", Diagnostics::percentile(d.dab_latency_ms, 0.5),
                Diagnostics::percentile(d.dab_latency_ms, 0.95), Diagnostics::percentile(d.dab_latency_ms, 1.0));
    QElapsedTimer ct;
    ct.start();
    send(QEvent::TabletRelease, QPointF(v.right(), v.center().y()), 0.6, 1000 + 4 * n + 4);
    std::printf("  pen-up commit    : %.1f ms (engine end + history + re-dirty)\n", ms_since(ct));
    d.dab_latency_ms.clear();
}

// Repaints until the display is complete; returns {first frame ms, total ms, frames}.
struct Frames {
    double first = 0, total = 0, worst = 0;
    int frames = 0;
};
Frames paint_until_complete(QWidget* w, CanvasController* c) {
    Frames f;
    QElapsedTimer t;
    t.start();
    do {
        QElapsedTimer ft;
        ft.start();
        w->repaint();
        const double ms = ms_since(ft);
        if (f.frames == 0) f.first = ms;
        f.worst = std::max(f.worst, ms);
        ++f.frames;
    } while (!c->display_complete() && f.frames < 10000);
    f.total = ms_since(t);
    return f;
}

void bench_photo(const QString& path) {
    MainWindow mw;
    mw.set_confirm_close(false);
    mw.resize(1480, 920);
    mw.show();
    (void)QTest::qWaitForWindowExposed(&mw);
    QCoreApplication::processEvents();
    QElapsedTimer t;
    t.start();
    if (!mw.open_file(path)) {
        std::printf("open failed: %s\n", qPrintable(mw.last_message()));
        return;
    }
    const double open_ms = ms_since(t);
    CanvasController* c = mw.canvas();
    QWidget* w = mw.canvas_widget();
    std::printf("photo %s: %d x %d\n", qPrintable(path), mw.session()->doc().width(), mw.session()->doc().height());
    std::printf("  open (decode + adopt)   : %.0f ms (reported %.0f ms)\n", open_ms, mw.last_open_ms());
    const Frames fit = paint_until_complete(w, c);
    std::printf("  fit to window %.1f %% (level %d): first frame %.1f ms, complete after %d frames / %.0f ms, worst frame %.1f ms\n",
                c->zoom() * 100, c->display_level(), fit.first, fit.frames, fit.total, fit.worst);
    // Pan frames (cached tiles).
    double pan_worst = 0, pan_sum = 0;
    for (int i = 0; i < 20; ++i) {
        c->pan_by(QPointF(i % 2 ? -40 : 40, 25 * (i % 3 - 1)));
        const Frames f = paint_until_complete(w, c);
        pan_worst = std::max(pan_worst, f.worst);
        pan_sum += f.total;
    }
    std::printf("  pan frame at fit        : mean %.2f ms, worst %.2f ms\n", pan_sum / 20, pan_worst);
    // Zoom steps (new pyramid level each time; at 100 % a level-0 render of the visible tiles).
    for (int k = 0; k < 4; ++k) {
        c->zoom_in();
        const Frames f = paint_until_complete(w, c);
        std::printf("  zoom step -> %.1f %% (level %d): first frame %.1f ms, complete %d frames / %.1f ms, worst %.1f ms\n", c->zoom() * 100,
                    c->display_level(), f.first, f.frames, f.total, f.worst);
    }
    double pan100_worst = 0, pan100_sum = 0;
    for (int i = 0; i < 20; ++i) {
        c->pan_by(QPointF(-120, 0));
        const Frames f = paint_until_complete(w, c);
        pan100_worst = std::max(pan100_worst, f.worst);
        pan100_sum += f.total;
    }
    std::printf("  pan frame at %.0f %% (120 px into unrendered tiles): mean %.2f ms, worst frame %.2f ms\n", c->zoom() * 100,
                pan100_sum / 20, pan100_worst);
    c->fit_to_view();
    paint_until_complete(w, c);

    // GL path through an FBO (the widget cannot be shown offscreen).
    const GlProbe p = probe_gl();
    if (!p.ok) {
        std::printf("  GL: skipped (%s)\n", qPrintable(p.reason));
        return;
    }
    CanvasController gc(mw.session(), mw.tools());
    CanvasGLWidget glw(&gc);
    gc.set_widget(&glw);
    glw.resize(w->size());
    gc.sync_document();
    QOpenGLContext ctx;
    QSurfaceFormat f;
    f.setVersion(3, 3);
    f.setProfile(QSurfaceFormat::CoreProfile);
    ctx.setFormat(f);
    ctx.create();
    QOffscreenSurface surf;
    surf.setFormat(ctx.format());
    surf.create();
    ctx.makeCurrent(&surf);
    QOpenGLFramebufferObject fbo(w->size());
    fbo.bind();
    glw.init_for_test(&ctx);
    ctx.functions()->glViewport(0, 0, w->width(), w->height());
    const auto gl_frames = [&] {
        Frames fr;
        QElapsedTimer tt;
        tt.start();
        do {
            QElapsedTimer ft;
            ft.start();
            glw.render_for_test(w->width(), w->height());
            ctx.functions()->glFinish();
            const double ms = ms_since(ft);
            if (fr.frames == 0) fr.first = ms;
            fr.worst = std::max(fr.worst, ms);
            ++fr.frames;
        } while (!gc.display_complete() && fr.frames < 10000);
        fr.total = ms_since(tt);
        return fr;
    };
    const Frames g0 = gl_frames();
    std::printf("  GL (%s) fit: first frame %.1f ms, complete %d frames / %.0f ms, worst %.1f ms\n", qPrintable(p.renderer), g0.first,
                g0.frames, g0.total, g0.worst);
    double gp = 0, gw = 0;
    for (int i = 0; i < 20; ++i) {
        gc.pan_by(QPointF(i % 2 ? -40 : 40, 0));
        const Frames fr = gl_frames();
        gp += fr.total;
        gw = std::max(gw, fr.worst);
    }
    std::printf("  GL pan frame at fit     : mean %.2f ms, worst %.2f ms\n", gp / 20, gw);
    for (int k = 0; k < 4; ++k) {
        gc.zoom_in();
        const Frames fr = gl_frames();
        std::printf("  GL zoom step -> %.1f %%: first %.1f ms, complete %d frames / %.1f ms\n", gc.zoom() * 100, fr.first, fr.frames,
                    fr.total);
    }
    glw.cleanup_for_test();
    fbo.release();
    ctx.doneCurrent();
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QCoreApplication::setAttribute(Qt::AA_CompressHighFrequencyEvents, false);
    const QStringList a = app.arguments();
    if (a.size() >= 2 && a[1] == QLatin1String("brush")) {
        bench_brush(0.25, 1.0);
        bench_brush(0.25, 0.25);
        bench_brush(0.05, 1.0);
        return 0;
    }
    if (a.size() >= 3 && a[1] == QLatin1String("photo")) {
        bench_photo(a[2]);
        return 0;
    }
    std::fprintf(stderr, "usage: gui_bench brush | gui_bench photo FILE\n");
    return 2;
}
