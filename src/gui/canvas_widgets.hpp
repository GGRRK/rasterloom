// SPDX-License-Identifier: GPL-3.0-or-later
//
// The canvas display widgets and their shared input path.
//
// Input rules (BUILD-SPEC req 5, doc 40 §5, docs/research/qt-platform.md section 1):
// - every QTabletEvent is accepted unconditionally (an ignored one would be re-sent as a
//   synthesized mouse event, and Qt compresses those);
// - tablet tracking is on so hover samples reach Input Diagnostics and the brush outline;
// - while the pen is down, synthesized / non-mouse mouse events are dropped so a stroke never
//   receives two sample streams.
#pragma once

#include <QString>
#include <QWidget>

class QEvent;

namespace rl::gui {

class CanvasController;

namespace canvas_input {
// Routes one event of a canvas widget to the controller. Returns true when the event was consumed.
bool handle(QWidget* w, CanvasController* c, QEvent* e);
// Common widget setup (focus, tracking, attributes).
void setup(QWidget* w);
}  // namespace canvas_input

// The mandatory QPainter raster fallback (and the backend offscreen tests use).
class CanvasRasterWidget : public QWidget {
    Q_OBJECT
public:
    explicit CanvasRasterWidget(CanvasController* c, QWidget* parent = nullptr);

protected:
    bool event(QEvent* e) override;
    void paintEvent(class QPaintEvent* e) override;
    void resizeEvent(class QResizeEvent* e) override;

private:
    CanvasController* c_;
};

struct CanvasChoice {
    QString backend;  // "gl" or "raster"
    QString reason;
};

// Chooses the backend (RASTERLOOM_CANVAS=gl|raster override; otherwise a GL 3.3 core probe with a
// software-renderer throughput check), records it in Diagnostics and creates the widget.
QWidget* create_canvas_widget(CanvasController* c, QWidget* parent, CanvasChoice* choice);

}  // namespace rl::gui
