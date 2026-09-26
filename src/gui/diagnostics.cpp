// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/diagnostics.hpp"

#include <algorithm>
#include <vector>

#include <QApplication>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGuiApplication>
#include <QLabel>
#include <QScreen>
#include <QTimer>
#include <QVBoxLayout>

namespace rl::gui {

Diagnostics& diag() {
    static Diagnostics d;
    return d;
}

qint64 Diagnostics::now_ms() {
    if (!clock.isValid()) clock.start();
    return clock.elapsed();
}

qint64 Diagnostics::now_ns() {
    if (!clock.isValid()) clock.start();
    return clock.nsecsElapsed();
}

namespace {
void trim(std::deque<qint64>& q, qint64 now) {
    while (!q.empty() && now - q.front() > 1000) q.pop_front();
}
}  // namespace

void Diagnostics::note_tablet_event() {
    const qint64 t = now_ms();
    tablet_times.push_back(t);
    trim(tablet_times, t);
    ++tablet_events_total;
}

void Diagnostics::note_mouse_event() {
    const qint64 t = now_ms();
    mouse_times.push_back(t);
    trim(mouse_times, t);
    ++mouse_events_total;
}

int Diagnostics::tablet_rate() {
    trim(tablet_times, now_ms());
    return static_cast<int>(tablet_times.size());
}

int Diagnostics::mouse_rate() {
    trim(mouse_times, now_ms());
    return static_cast<int>(mouse_times.size());
}

namespace {
void push_window(std::deque<double>& d, double v) {
    d.push_back(v);
    while (d.size() > 240) d.pop_front();
}
}  // namespace

void Diagnostics::note_dab_latency(double ms) { push_window(dab_latency_ms, ms); }
void Diagnostics::note_frame(double ms) { push_window(frame_ms, ms); }

void Diagnostics::reset_stroke_stats() {
    stroke_dabs = 0;
    stroke_engine_ms = 0.0;
    stroke_wall_ms = 0.0;
    stroke_events = 0;
}

double Diagnostics::percentile(const std::deque<double>& v, double q) {
    if (v.empty()) return 0.0;
    std::vector<double> s(v.begin(), v.end());
    std::sort(s.begin(), s.end());
    const size_t i = std::min(s.size() - 1, static_cast<size_t>(q * static_cast<double>(s.size() - 1) + 0.5));
    return s[i];
}

double Diagnostics::cache_hit_rate() const {
    const qint64 n = cache_hits + cache_misses;
    return n > 0 ? static_cast<double>(cache_hits) / static_cast<double>(n) : 0.0;
}

InputDiagnosticsDialog::InputDiagnosticsDialog(QWidget* parent) : QDialog(parent) {
    setObjectName(QStringLiteral("InputDiagnostics"));
    setWindowTitle(tr("Input Diagnostics"));
    auto* lay = new QVBoxLayout(this);
    auto* intro = new QLabel(tr("Move or press a pen over the canvas; these values update live. "
                                "Screenshot this window when reporting a tablet problem."));
    intro->setWordWrap(true);
    intro->setStyleSheet(QStringLiteral("color:#8d9199;"));
    lay->addWidget(intro);
    auto* f = new QFormLayout;
    f->setLabelAlignment(Qt::AlignRight);
    f->setHorizontalSpacing(14);
    qt_ = add_row(f, tr("Qt runtime"));
    platform_ = add_row(f, tr("Platform (QPA)"));
    screen_ = add_row(f, tr("Screen"));
    backend_ = add_row(f, tr("Canvas backend"));
    gl_vendor_ = add_row(f, QStringLiteral("GL_VENDOR"));
    gl_renderer_ = add_row(f, QStringLiteral("GL_RENDERER"));
    gl_version_ = add_row(f, QStringLiteral("GL_VERSION"));
    device_ = add_row(f, tr("Tablet device"));
    pointer_ = add_row(f, tr("Pointer type"));
    pressure_ = add_row(f, tr("Pressure"));
    tilt_ = add_row(f, tr("Tilt (x, y)"));
    rate_ = add_row(f, tr("Event rate"));
    dab_ = add_row(f, tr("Dab-to-pixel"));
    dab_->setToolTip(tr("Time from receiving a pen/mouse sample to the end of the frame that shows it (ms)"));
    engine_ = add_row(f, tr("Brush engine"));
    engine_->setToolTip(tr("Throughput of rl::brush::StrokeSession over the current or last stroke"));
    frame_ = add_row(f, tr("Last frame"));
    tiles_ = add_row(f, tr("Tiles / frame"));
    cache_ = add_row(f, tr("Tile cache hit rate"));
    lay->addLayout(f);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::close);
    lay->addWidget(bb);
    timer_ = new QTimer(this);
    timer_->setInterval(100);
    connect(timer_, &QTimer::timeout, this, &InputDiagnosticsDialog::refresh);
    timer_->start();
    refresh();
    resize(560, sizeHint().height());
}

QLabel* InputDiagnosticsDialog::add_row(QFormLayout* f, const QString& name) {
    auto* v = new QLabel(QStringLiteral("-"));
    v->setTextInteractionFlags(Qt::TextSelectableByMouse);
    v->setMinimumWidth(240);
    auto* n = new QLabel(name);
    n->setStyleSheet(QStringLiteral("color:#8d9199;"));
    f->addRow(n, v);
    return v;
}

void InputDiagnosticsDialog::refresh() {
    Diagnostics& d = diag();
    const auto or_dash = [](const QString& s) { return s.isEmpty() ? QStringLiteral("-") : s; };
    qt_->setText(QString::fromLatin1(qVersion()));
    platform_->setText(QGuiApplication::platformName());
    if (QScreen* s = QGuiApplication::primaryScreen())
        screen_->setText(QStringLiteral("%1x%2 @ %3 Hz, dpr %4")
                             .arg(s->size().width())
                             .arg(s->size().height())
                             .arg(s->refreshRate(), 0, 'f', 0)
                             .arg(s->devicePixelRatio()));
    backend_->setText(d.canvas_reason.isEmpty() ? d.canvas_backend
                                                : QStringLiteral("%1 (%2)").arg(d.canvas_backend, d.canvas_reason));
    gl_vendor_->setText(or_dash(d.gl_vendor));
    gl_renderer_->setText(or_dash(d.gl_renderer));
    gl_version_->setText(or_dash(d.gl_version));
    device_->setText(d.tablet_device.isEmpty() ? tr("none seen yet") : d.tablet_device);
    pointer_->setText(or_dash(d.pointer_type) +
                      (d.last_source.isEmpty() ? QString() : tr("   (last event from: %1)").arg(d.last_source)));
    pressure_->setText(QString::number(d.pressure, 'f', 3));
    tilt_->setText(QStringLiteral("%1°, %2°").arg(d.tilt_x, 0, 'f', 1).arg(d.tilt_y, 0, 'f', 1));
    rate_->setText(tr("tablet %1/s, mouse %2/s (totals %3 / %4)")
                       .arg(d.tablet_rate())
                       .arg(d.mouse_rate())
                       .arg(d.tablet_events_total)
                       .arg(d.mouse_events_total));
    dab_->setText(tr("%1 ms last · median %2 · p95 %3")
                      .arg(d.dab_to_pixel_ms, 0, 'f', 2)
                      .arg(Diagnostics::percentile(d.dab_latency_ms, 0.5), 0, 'f', 2)
                      .arg(Diagnostics::percentile(d.dab_latency_ms, 0.95), 0, 'f', 2));
    if (d.stroke_dabs > 0 && d.stroke_engine_ms > 0.0)
        engine_->setText(tr("%3 dabs/s  (%1 dabs in %2 ms, %4 events)")
                             .arg(d.stroke_dabs)
                             .arg(d.stroke_engine_ms, 0, 'f', 1)
                             .arg(static_cast<double>(d.stroke_dabs) * 1000.0 / d.stroke_engine_ms, 0, 'f', 0)
                             .arg(d.stroke_events));
    else
        engine_->setText(tr("no stroke yet"));
    frame_->setText(tr("%1 ms last, median %2 ms, max %3 ms")
                        .arg(d.last_frame_ms, 0, 'f', 2)
                        .arg(Diagnostics::percentile(d.frame_ms, 0.5), 0, 'f', 2)
                        .arg(Diagnostics::percentile(d.frame_ms, 1.0), 0, 'f', 2));
    tiles_->setText(tr("%1 (total %2)").arg(d.tiles_last_frame).arg(d.tiles_rendered_total));
    cache_->setText(QStringLiteral("%1 %  (%2 hits, %3 misses)")
                        .arg(d.cache_hit_rate() * 100.0, 0, 'f', 1)
                        .arg(d.cache_hits)
                        .arg(d.cache_misses));
}

}  // namespace rl::gui
