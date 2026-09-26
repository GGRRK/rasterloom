// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/canvas_widgets.hpp"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPointingDevice>
#include <QResizeEvent>
#include <QTabletEvent>
#include <QWheelEvent>

#include <cstdio>

#include "gui/canvas.hpp"
#include "gui/canvas_gl.hpp"
#include "gui/diagnostics.hpp"

namespace rl::gui {

namespace canvas_input {

namespace {

QString pointer_type_name(QPointingDevice::PointerType t) {
    switch (t) {
        case QPointingDevice::PointerType::Pen: return QStringLiteral("Pen");
        case QPointingDevice::PointerType::Eraser: return QStringLiteral("Eraser");
        case QPointingDevice::PointerType::Cursor: return QStringLiteral("Cursor");
        case QPointingDevice::PointerType::Finger: return QStringLiteral("Finger");
        case QPointingDevice::PointerType::Generic: return QStringLiteral("Generic");
        default: return QStringLiteral("Unknown");
    }
}

bool handle_tablet(QWidget* w, CanvasController* c, QTabletEvent* te) {
    te->accept();  // unconditionally (BUILD-SPEC req 5)
    Diagnostics& d = diag();
    d.note_tablet_event();
    d.last_source = QStringLiteral("tablet");
    if (const QPointingDevice* dev = te->pointingDevice()) d.tablet_device = dev->name();
    d.pointer_type = pointer_type_name(te->pointerType());
    d.pressure = te->pressure();
    d.tilt_x = te->xTilt();
    d.tilt_y = te->yTilt();

    PointerInput in;
    in.pos = te->position();
    in.pressure = te->pressure();
    in.tilt_x = te->xTilt();
    in.tilt_y = te->yTilt();
    in.t_ms = static_cast<double>(te->timestamp());
    in.button = te->button();
    in.buttons = te->buttons();
    in.mods = te->modifiers();
    in.tablet = true;
    in.eraser_end = te->pointerType() == QPointingDevice::PointerType::Eraser;
    switch (te->type()) {
        case QEvent::TabletPress:
            w->setFocus(Qt::MouseFocusReason);
            c->set_tablet_down(true);
            if (in.button == Qt::NoButton) in.button = Qt::LeftButton;
            c->pointer_press(in);
            break;
        case QEvent::TabletMove:
            if (c->tablet_down()) c->pointer_move(in);
            else c->hover(in);
            break;
        case QEvent::TabletRelease:
            if (in.button == Qt::NoButton) in.button = Qt::LeftButton;
            c->pointer_release(in);
            c->set_tablet_down(false);
            break;
        default: break;
    }
    return true;
}

bool handle_mouse(QWidget* w, CanvasController* c, QMouseEvent* me) {
    // Defensive rule (qt-platform.md section 1): while the pen is down, drop every mouse event
    // that is synthesized or does not come from a real mouse.
    const bool from_mouse = me->source() == Qt::MouseEventNotSynthesized &&
                            (!me->pointingDevice() || me->pointingDevice()->type() == QInputDevice::DeviceType::Mouse);
    if (c->tablet_down() && !from_mouse) {
        me->accept();
        return true;
    }
    Diagnostics& d = diag();
    d.note_mouse_event();
    d.last_source = QStringLiteral("mouse");
    PointerInput in;
    in.pos = me->position();
    in.t_ms = static_cast<double>(me->timestamp());
    in.button = me->button();
    in.buttons = me->buttons();
    in.mods = me->modifiers();
    switch (me->type()) {
        case QEvent::MouseButtonPress:
            w->setFocus(Qt::MouseFocusReason);
            c->pointer_press(in);
            break;
        case QEvent::MouseButtonDblClick:
            in.double_click = true;
            c->pointer_press(in);
            break;
        case QEvent::MouseMove:
            if (me->buttons() != Qt::NoButton) c->pointer_move(in);
            else c->hover(in);
            break;
        case QEvent::MouseButtonRelease: c->pointer_release(in); break;
        default: break;
    }
    me->accept();
    return true;
}

}  // namespace

void setup(QWidget* w) {
    w->setFocusPolicy(Qt::StrongFocus);
    w->setMouseTracking(true);
    w->setTabletTracking(true);
    w->setAttribute(Qt::WA_OpaquePaintEvent);
    w->setMinimumSize(200, 150);
    w->setObjectName(QStringLiteral("Canvas"));
}

bool handle(QWidget* w, CanvasController* c, QEvent* e) {
    switch (e->type()) {
        case QEvent::TabletPress:
        case QEvent::TabletMove:
        case QEvent::TabletRelease: return handle_tablet(w, c, static_cast<QTabletEvent*>(e));
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonDblClick:
        case QEvent::MouseMove:
        case QEvent::MouseButtonRelease: return handle_mouse(w, c, static_cast<QMouseEvent*>(e));
        case QEvent::Wheel: c->wheel(static_cast<QWheelEvent*>(e)); return true;
        case QEvent::KeyPress:
            if (c->key_press(static_cast<QKeyEvent*>(e))) {
                e->accept();
                return true;
            }
            return false;
        case QEvent::KeyRelease:
            if (c->key_release(static_cast<QKeyEvent*>(e))) {
                e->accept();
                return true;
            }
            return false;
        case QEvent::FocusOut: c->focus_lost(); return false;
        case QEvent::Leave: c->pointer_left(); return false;
        default: return false;
    }
}

}  // namespace canvas_input

CanvasRasterWidget::CanvasRasterWidget(CanvasController* c, QWidget* parent) : QWidget(parent), c_(c) {
    canvas_input::setup(this);
}

bool CanvasRasterWidget::event(QEvent* e) {
    if (canvas_input::handle(this, c_, e)) return true;
    return QWidget::event(e);
}

void CanvasRasterWidget::paintEvent(QPaintEvent* e) {
    QPainter p(this);
    c_->frame_begin();
    c_->paint_raster(p, e->rect());
    c_->paint_overlays(p);
    c_->frame_end();
}

void CanvasRasterWidget::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    c_->viewport_resized();
}

QWidget* create_canvas_widget(CanvasController* c, QWidget* parent, CanvasChoice* choice) {
    Diagnostics& d = diag();
    const QByteArray forced = qgetenv("RASTERLOOM_CANVAS").trimmed().toLower();
    CanvasChoice ch;
    if (forced == "raster") {
        ch = {QStringLiteral("raster"), QStringLiteral("RASTERLOOM_CANVAS=raster")};
    } else {
        GlProbe probe = probe_gl();
        d.gl_vendor = probe.vendor;
        d.gl_renderer = probe.renderer;
        d.gl_version = probe.version;
        if (!probe.vendor.isEmpty())
            std::fprintf(stderr, "rasterloom: GL_VENDOR=%s GL_RENDERER=%s GL_VERSION=%s\n", qPrintable(probe.vendor),
                         qPrintable(probe.renderer), qPrintable(probe.version));
        if (forced == "gl") {
            ch = {QStringLiteral("gl"), probe.ok ? QStringLiteral("RASTERLOOM_CANVAS=gl")
                                                 : QStringLiteral("RASTERLOOM_CANVAS=gl (probe failed: %1)").arg(probe.reason)};
        } else if (probe.ok) {
            ch = {QStringLiteral("gl"), probe.reason};
        } else {
            ch = {QStringLiteral("raster"), QStringLiteral("fallback: %1").arg(probe.reason)};
        }
    }
    d.canvas_backend = ch.backend;
    d.canvas_reason = ch.reason;
    std::fprintf(stderr, "rasterloom: canvas backend %s (%s)\n", qPrintable(ch.backend), qPrintable(ch.reason));
    if (choice) *choice = ch;
    QWidget* w = nullptr;
    if (ch.backend == QLatin1String("gl")) w = new CanvasGLWidget(c, parent);
    else w = new CanvasRasterWidget(c, parent);
    c->set_widget(w);
    return w;
}

}  // namespace rl::gui
