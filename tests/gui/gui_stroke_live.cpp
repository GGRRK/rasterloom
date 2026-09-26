// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live strokes through rl::brush::StrokeSession (src/gui/stroke_adapter.*): synthetic QTabletEvent
// strokes (pressure, tilt, timestamps) sent to the real canvas widget. For each stroke:
//   - the live preview before pointer-up is byte-identical to the layer after commit;
//   - the canvas displays exactly that preview while the pen is down (display state = preview);
//   - the committed op replays through `rasterloom-cli --render-script` to the same composite;
//   - exactly one history record per stroke; undo restores the pre-stroke pixels.
// Brush (pixels), brush on a layer mask, eraser, and clone stamp (Alt-click source) are covered.
#include <QApplication>
#include <QDir>
#include <QProcess>
#include <QTabletEvent>
#include <QTemporaryDir>
#include <QTest>
#include <QWheelEvent>

#include <cmath>
#include <fstream>
#include <vector>

#include "core/composite/render.hpp"
#include "core/io/export_png.hpp"
#include "core/io/png.hpp"
#include "gui/canvas.hpp"
#include "gui/canvas_widgets.hpp"
#include "gui/session.hpp"
#include "gui/tools.hpp"

using namespace rl::gui;

namespace {

std::vector<uint8_t> bytes_of(const rl::RgbaImage& im) {
    std::vector<uint8_t> v(static_cast<size_t>(im.width()) * static_cast<size_t>(im.height()) * 4);
    for (int y = 0; y < im.height(); ++y)
        for (int x = 0; x < im.width(); ++x) {
            const rl::Rgba8 p = im.get(x, y);
            uint8_t* d = &v[(static_cast<size_t>(y) * static_cast<size_t>(im.width()) + static_cast<size_t>(x)) * 4];
            d[0] = p.r, d[1] = p.g, d[2] = p.b, d[3] = p.a;
        }
    return v;
}

std::vector<uint8_t> bytes_of(const rl::GrayImage& im) {
    std::vector<uint8_t> v(static_cast<size_t>(im.width()) * static_cast<size_t>(im.height()));
    for (int y = 0; y < im.height(); ++y)
        for (int x = 0; x < im.width(); ++x) v[static_cast<size_t>(y) * static_cast<size_t>(im.width()) + static_cast<size_t>(x)] = im.get(x, y);
    return v;
}

// The document composite as decoded PNG bytes (the same writer the CLI uses).
std::vector<uint8_t> composite_png_bytes(const rl::DocState& s, const QString& path) {
    rl::io::write_document_png(s, path.toStdString());
    const rl::io::RgbaBuffer b = rl::io::read_png(path.toStdString());
    std::vector<uint8_t> v;
    for (int y = 0; y < b.h; ++y)
        for (int x = 0; x < b.w; ++x) {
            const rl::Rgba8 p = b.at(x, y);
            v.insert(v.end(), {p.r, p.g, p.b, p.a});
        }
    return v;
}

const rl::Node* find_node(const rl::Node& c, const std::string& id) {
    for (const rl::Node& n : c.children) {
        if (n.id == id) return &n;
        if (n.is_group())
            if (const rl::Node* f = find_node(n, id)) return f;
    }
    return nullptr;
}

}  // namespace

class GuiStrokeLive : public QObject {
    Q_OBJECT

    EditorSession s_;
    ToolState tools_;
    CanvasController* c_ = nullptr;
    CanvasRasterWidget* w_ = nullptr;
    QPointingDevice* pen_ = nullptr;
    QTemporaryDir tmp_;
    quint64 t_ = 1000;

    void tablet(QEvent::Type type, const QPointF& canvas_pt, double pressure, double tx, double ty, quint64 ts,
                Qt::KeyboardModifiers mods = Qt::NoModifier) {
        const QPointF lp = c_->to_widget(canvas_pt);
        const Qt::MouseButton b = type == QEvent::TabletMove ? Qt::NoButton : Qt::LeftButton;
        const Qt::MouseButtons bs = type == QEvent::TabletRelease ? Qt::NoButton : Qt::LeftButton;
        QTabletEvent e(type, pen_, lp, w_->mapToGlobal(lp), pressure, float(tx), float(ty), 0.f, 0.0, 0.f, mods, b, bs);
        e.setTimestamp(ts);
        QCoreApplication::sendEvent(w_, &e);
        QVERIFY2(e.isAccepted(), "canvas must accept every QTabletEvent");
    }

    // Presses, moves through `n` samples along a wave, and returns without releasing.
    void stroke_down(double x0, double y0, double x1, double y1, int n) {
        t_ += 50;
        tablet(QEvent::TabletPress, QPointF(x0, y0), 0.15, -20, 10, t_);
        for (int i = 1; i <= n; ++i) {
            const double t = double(i) / n;
            t_ += 7;
            tablet(QEvent::TabletMove, QPointF(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t + 18.0 * std::sin(t * 6.0)),
                   0.15 + 0.8 * t, -20 + 40 * t, 10 - 25 * t, t_);
        }
    }

    // Replays the session script with the CLI and compares its composite to the GUI document's.
    void check_cli_replay(const char* what) {
        const std::optional<Json> script = s_.session_script();
        QVERIFY(script.has_value());
        const QString sp = tmp_.filePath(QStringLiteral("session.json")), out = tmp_.filePath(QStringLiteral("cli.png"));
        {
            std::ofstream f(sp.toStdString());
            f << script->dump(1);
        }
        QProcess p;
        p.start(QStringLiteral(RL_CLI), {QStringLiteral("--render-script"), sp, QStringLiteral("--out"), out});
        QVERIFY(p.waitForFinished(60000));
        QVERIFY2(p.exitCode() == 0, qPrintable(QString::fromLocal8Bit(p.readAllStandardError())));
        const rl::io::RgbaBuffer b = rl::io::read_png(out.toStdString());
        std::vector<uint8_t> cli;
        for (int y = 0; y < b.h; ++y)
            for (int x = 0; x < b.w; ++x) {
                const rl::Rgba8 px = b.at(x, y);
                cli.insert(cli.end(), {px.r, px.g, px.b, px.a});
            }
        const std::vector<uint8_t> gui = composite_png_bytes(s_.state(), tmp_.filePath(QStringLiteral("gui.png")));
        QVERIFY2(cli == gui, qPrintable(QStringLiteral("CLI replay differs from the GUI document after ") + QLatin1String(what)));
    }

private slots:
    void initTestCase() {
        QVERIFY(tmp_.isValid());
        QCoreApplication::setAttribute(Qt::AA_CompressHighFrequencyEvents, false);
        pen_ = new QPointingDevice(QStringLiteral("Test Pen"), 4243, QInputDevice::DeviceType::Stylus, QPointingDevice::PointerType::Pen,
                                   QInputDevice::Capability::Position | QInputDevice::Capability::Pressure |
                                       QInputDevice::Capability::XTilt | QInputDevice::Capability::YTilt,
                                   1, 3, QString(), QPointingDeviceUniqueId::fromNumericId(0xBEEF), this);
        s_.new_document(320, 200, 1);  // transparent, one layer "Layer 1"
        c_ = new CanvasController(&s_, &tools_, this);
        w_ = new CanvasRasterWidget(c_);
        c_->set_widget(w_);
        w_->resize(400, 280);
        w_->show();
        QVERIFY(QTest::qWaitForWindowExposed(w_));
        c_->sync_document();
        c_->set_zoom(1.0, QPointF(0, 0));
        c_->center_on(QPointF(160, 100));
    }

    void brush_preview_equals_commit_equals_replay() {
        tools_.set_tool(Tool::Brush);
        tools_.set_fg(QColor(0x2a, 0x6f, 0xdb));
        BrushSettings& b = tools_.settings().brush;
        b.size = 28;
        b.hardness = 0.3;
        b.flow = 0.6;
        b.pressure_size = true;
        b.pressure_opacity = true;
        const std::string layer = s_.active_layer();
        const size_t hist = s_.applied_count();
        const std::vector<uint8_t> before = bytes_of(s_.doc().require_raster(layer, "t").pixels);

        stroke_down(30, 60, 290, 140, 40);
        QVERIFY(c_->stroke_active());
        QCOMPARE(c_->stroke().samples().size(), size_t{41});
        // Live: the document is untouched, the canvas displays the preview.
        QVERIFY(bytes_of(s_.doc().require_raster(layer, "t").pixels) == before);
        const rl::RgbaImage* live = c_->stroke().preview_pixels();
        QVERIFY(live);
        const std::vector<uint8_t> preview = bytes_of(*live);
        QVERIFY(preview != before);
        const rl::Node* shown = find_node(c_->display_state().root, layer);
        QVERIFY(shown);
        QVERIFY2(bytes_of(shown->pixels) == preview, "the canvas must display the engine's preview");
        // The displayed tile really is recomposited from the preview.
        rl::RgbaTile expect;
        rl::composite::render_tile(c_->display_state(), 2, 1, true, expect);
        const QImage& tile = c_->display_tile(2, 1);
        int painted = 0;
        for (int y = 0; y < rl::kTileSize; ++y)
            for (int x = 0; x < rl::kTileSize; ++x) {
                const rl::Rgba8 e = expect.at(x, y);
                const unsigned a = e.a;
                const QRgb want = a == 0 ? 0u : qRgba((e.r * a + 127) / 255, (e.g * a + 127) / 255, (e.b * a + 127) / 255, int(a));
                QCOMPARE(reinterpret_cast<const QRgb*>(tile.constScanLine(y))[x], want);
                painted += e.a != 0;
            }
        QVERIFY(painted > 0);

        // Release at the last position and time: no extra sample (doc 40 §5).
        tablet(QEvent::TabletRelease, QPointF(c_->stroke().samples().back().x, c_->stroke().samples().back().y), 0.95, 20, -15, t_);
        QVERIFY(!c_->stroke_active());
        QCOMPARE(s_.applied_count(), hist + 1);
        const Json op = s_.entries().back().op;
        QCOMPARE(QString::fromStdString(op["op"].get<std::string>()), QStringLiteral("brush_stroke"));
        QCOMPARE(op["samples"].size(), size_t{41});
        QCOMPARE(op["samples"][5]["tilt_x"].get<double>(), -20 + 40 * 5.0 / 40);
        const std::vector<uint8_t> committed = bytes_of(s_.doc().require_raster(layer, "t").pixels);
        QVERIFY2(committed == preview, "committed layer must equal the live preview byte for byte");
        check_cli_replay("the brush stroke");
        // Undo restores the pre-stroke pixels exactly.
        s_.undo();
        QVERIFY(bytes_of(s_.doc().require_raster(layer, "t").pixels) == before);
        s_.redo();
        QVERIFY(bytes_of(s_.doc().require_raster(layer, "t").pixels) == committed);
    }

    void mask_eraser_clone() {
        const std::string layer = s_.active_layer();
        // Mask stroke: add a reveal-all mask and paint black into it.
        QVERIFY(s_.apply({{"op", "add_mask"}, {"layer", layer}, {"fill", "solid"}, {"value", 255}}, "mask").ok());
        s_.set_edit_mask(true);
        QVERIFY(s_.edit_mask());
        tools_.set_fg(QColor(0, 0, 0));
        stroke_down(40, 150, 200, 170, 20);
        QVERIFY(c_->stroke().targets_mask());
        const std::vector<uint8_t> mprev = bytes_of(*c_->stroke().preview_mask());
        const rl::Node* shown = find_node(c_->display_state().root, layer);
        QVERIFY(shown && shown->mask);
        QVERIFY(bytes_of(shown->mask->plane) == mprev);
        tablet(QEvent::TabletRelease, QPointF(c_->stroke().samples().back().x, c_->stroke().samples().back().y), 0.95, 0, 0, t_);
        QVERIFY(bytes_of(s_.doc().require_raster(layer, "t").mask->plane) == mprev);
        s_.set_edit_mask(false);
        check_cli_replay("the mask stroke");

        // Eraser.
        tools_.set_tool(Tool::Eraser);
        tools_.settings().eraser.size = 40;
        stroke_down(60, 40, 260, 120, 25);
        const std::vector<uint8_t> eprev = bytes_of(*c_->stroke().preview_pixels());
        tablet(QEvent::TabletRelease, QPointF(c_->stroke().samples().back().x, c_->stroke().samples().back().y), 0.95, 0, 0, t_);
        QCOMPARE(QString::fromStdString(s_.entries().back().op["op"].get<std::string>()), QStringLiteral("eraser_stroke"));
        QVERIFY(bytes_of(s_.doc().require_raster(layer, "t").pixels) == eprev);
        check_cli_replay("the eraser stroke");

        // Clone: Alt-click sets the source, then two strokes (aligned offset reused).
        tools_.set_tool(Tool::Clone);
        tablet(QEvent::TabletPress, QPointF(100, 90), 1.0, 0, 0, t_ += 30, Qt::AltModifier);
        tablet(QEvent::TabletRelease, QPointF(100, 90), 1.0, 0, 0, t_ += 5, Qt::AltModifier);
        QVERIFY(c_->clone_source().has_value());
        for (int k = 0; k < 2; ++k) {
            stroke_down(180 + 20 * k, 50, 300, 90 + 30 * k, 15);
            const std::vector<uint8_t> cprev = bytes_of(*c_->stroke().preview_pixels());
            tablet(QEvent::TabletRelease, QPointF(c_->stroke().samples().back().x, c_->stroke().samples().back().y), 0.9, 0, 0, t_);
            QCOMPARE(QString::fromStdString(s_.entries().back().op["op"].get<std::string>()), QStringLiteral("clone_stroke"));
            QVERIFY(bytes_of(s_.doc().require_raster(layer, "t").pixels) == cprev);
        }
        check_cli_replay("two clone strokes");
    }

    void navigation_space_hand_and_wheel_zoom() {
        tools_.set_tool(Tool::Brush);
        c_->set_zoom(1.0, QPointF(0, 0));
        c_->center_on(QPointF(160, 100));
        const size_t hist = s_.applied_count();
        // Space held: a drag pans (Hand), and paints nothing.
        QKeyEvent sp(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
        QCoreApplication::sendEvent(w_, &sp);
        const QPointF off0 = c_->offset();
        QTest::mousePress(w_, Qt::LeftButton, Qt::NoModifier, QPoint(200, 140));
        QTest::mouseMove(w_, QPoint(230, 150));
        QTest::mouseRelease(w_, Qt::LeftButton, Qt::NoModifier, QPoint(230, 150));
        QKeyEvent spr(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier);
        QCoreApplication::sendEvent(w_, &spr);
        QCOMPARE(c_->offset() - off0, QPointF(30, 10));
        QCOMPARE(s_.applied_count(), hist);
        // Ctrl+wheel and Alt+wheel (either axis) zoom about the cursor: the canvas point under it stays put.
        for (const auto& [mods, delta] : std::vector<std::pair<Qt::KeyboardModifiers, QPoint>>{
                 {Qt::ControlModifier, QPoint(0, 240)}, {Qt::AltModifier, QPoint(240, 0)}, {Qt::AltModifier, QPoint(0, -120)}}) {
            const QPointF at(250, 90);
            const QPointF before = c_->to_canvas(at);
            const double z0 = c_->zoom();
            QWheelEvent we(at, w_->mapToGlobal(at), QPoint(), delta, Qt::NoButton, mods, Qt::NoScrollPhase, false);
            QCoreApplication::sendEvent(w_, &we);
            QVERIFY(c_->zoom() != z0);
            QVERIFY(std::abs(c_->to_canvas(at).x() - before.x()) <= 1.0 / c_->zoom() + 1e-9);
            QVERIFY(std::abs(c_->to_canvas(at).y() - before.y()) <= 1.0 / c_->zoom() + 1e-9);
        }
    }

    void edits_during_a_stroke_commit_it_first() {
        tools_.set_tool(Tool::Brush);
        const size_t hist = s_.applied_count();
        stroke_down(20, 20, 120, 60, 10);
        QVERIFY(c_->stroke_active());
        // Another edit arrives mid-stroke (a menu shortcut): the stroke commits first.
        QVERIFY(s_.apply({{"op", "select_all"}}, "Select All").ok());
        QVERIFY(!c_->stroke_active());
        QCOMPARE(s_.applied_count(), hist + 2);
        QCOMPARE(QString::fromStdString(s_.entries()[hist].op["op"].get<std::string>()), QStringLiteral("brush_stroke"));
        tablet(QEvent::TabletRelease, QPointF(120, 60), 1.0, 0, 0, t_ += 5);  // stray release: ignored
        QCOMPARE(s_.applied_count(), hist + 2);
        check_cli_replay("a stroke committed by another edit");
    }
};

QTEST_MAIN(GuiStrokeLive)
#include "gui_stroke_live.moc"
