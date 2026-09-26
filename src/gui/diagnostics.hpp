// SPDX-License-Identifier: GPL-3.0-or-later
//
// Input Diagnostics (BUILD-SPEC req 14): the tester's only channel back, so every number here is
// measured where it happens (tabletEvent, paint, tile cache), never estimated.
#pragma once

#include <QDialog>
#include <QElapsedTimer>
#include <QString>

#include <deque>

class QFormLayout;
class QLabel;
class QTimer;

namespace rl::gui {

struct Diagnostics {
    // Set once at startup / canvas creation.
    QString canvas_backend = QStringLiteral("raster");
    QString canvas_reason;  // why this backend was chosen
    QString gl_vendor, gl_renderer, gl_version;

    // Live input.
    QString tablet_device;
    QString pointer_type;
    QString last_source;  // "tablet" / "mouse"
    double pressure = 0.0;
    double tilt_x = 0.0;
    double tilt_y = 0.0;
    std::deque<qint64> tablet_times;  // ms timestamps of the last second of tablet events
    std::deque<qint64> mouse_times;
    qint64 tablet_events_total = 0;
    qint64 mouse_events_total = 0;

    // Rendering.
    double dab_to_pixel_ms = 0.0;  // sample received -> the frame showing it finished painting
    qint64 pending_sample_ns = -1;
    int tiles_last_frame = 0;      // tiles recomposited in the last painted frame
    qint64 tiles_rendered_total = 0;
    qint64 cache_hits = 0;
    qint64 cache_misses = 0;
    double last_frame_ms = 0.0;

    // Rolling windows (last 240 values) for the dialog: latency and frame-time statistics.
    std::deque<double> dab_latency_ms;
    std::deque<double> frame_ms;
    void note_dab_latency(double ms);
    void note_frame(double ms);
    // Brush engine throughput over the current / last stroke.
    qint64 stroke_dabs = 0;         // dabs placed
    double stroke_engine_ms = 0.0;  // time inside StrokeSession::add_samples
    double stroke_wall_ms = 0.0;    // first sample -> last update
    qint64 stroke_events = 0;
    void reset_stroke_stats();
    static double percentile(const std::deque<double>& v, double q);

    QElapsedTimer clock;  // started on first use

    qint64 now_ms();
    qint64 now_ns();
    void note_tablet_event();
    void note_mouse_event();
    int tablet_rate();  // events in the last second
    int mouse_rate();
    double cache_hit_rate() const;
};

Diagnostics& diag();

class InputDiagnosticsDialog : public QDialog {
    Q_OBJECT
public:
    explicit InputDiagnosticsDialog(QWidget* parent = nullptr);
    void refresh();

private:
    QLabel* add_row(QFormLayout* f, const QString& name);
    QTimer* timer_ = nullptr;
    QLabel *platform_ = nullptr, *backend_ = nullptr, *gl_vendor_ = nullptr, *gl_renderer_ = nullptr,
           *gl_version_ = nullptr, *device_ = nullptr, *pointer_ = nullptr, *pressure_ = nullptr,
           *tilt_ = nullptr, *rate_ = nullptr, *dab_ = nullptr, *tiles_ = nullptr, *cache_ = nullptr,
           *frame_ = nullptr, *qt_ = nullptr, *screen_ = nullptr, *engine_ = nullptr;
};

}  // namespace rl::gui
