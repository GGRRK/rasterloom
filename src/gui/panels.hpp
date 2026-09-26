// SPDX-License-Identifier: GPL-3.0-or-later
//
// History, Color and Navigator panels.
#pragma once

#include <QImage>
#include <QListWidget>
#include <QTimer>
#include <QWidget>

class QLabel;
class QLineEdit;
class QSlider;
class QSpinBox;

namespace rl::gui {

class CanvasController;
class EditorSession;
class ToolState;

// One row per history entry (plus the base state). Clicking a row steps back (or forward) to it;
// entries after the current one are shown dimmed until a new edit discards them.
class HistoryPanel : public QWidget {
    Q_OBJECT
public:
    explicit HistoryPanel(EditorSession* s, QWidget* parent = nullptr);
    QListWidget* list() const { return list_; }

public slots:
    void rebuild();

private:
    EditorSession* s_;
    QListWidget* list_;
    QLabel* footer_;
};

// Saturation/value square with a hue strip (the canvas colour model is RGB bytes; HSV is only an
// input method here).
class HsvPicker : public QWidget {
    Q_OBJECT
public:
    explicit HsvPicker(QWidget* parent = nullptr);
    QColor color() const { return QColor::fromHsvF(h_, s_, v_); }
    void set_color(const QColor& c);
    QSize sizeHint() const override { return {220, 170}; }

signals:
    void color_edited(const QColor& c);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void resizeEvent(QResizeEvent*) override;

private:
    QRect sv_rect() const;
    QRect hue_rect() const;
    void pick(const QPoint& p, bool start);
    float h_ = 0.6f, s_ = 0.7f, v_ = 0.5f;
    int drag_ = 0;  // 1 = SV, 2 = hue
    QImage sv_cache_;
    float sv_cache_h_ = -1.0f;
};

class FgBgSwatch : public QWidget {
    Q_OBJECT
public:
    explicit FgBgSwatch(ToolState* t, QWidget* parent = nullptr);
    QSize sizeHint() const override { return {64, 60}; }

signals:
    void editing_target_changed(bool background);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;

private:
    ToolState* t_;
    bool target_bg_ = false;
};

class ColorPanel : public QWidget {
    Q_OBJECT
public:
    explicit ColorPanel(ToolState* t, QWidget* parent = nullptr);

private:
    void sync();
    void set_edited(const QColor& c);
    ToolState* t_;
    FgBgSwatch* swatch_;
    HsvPicker* picker_;
    QSpinBox *h_, *s_, *v_, *r_, *g_, *b_;
    QLineEdit* hex_;
    bool target_bg_ = false;
    bool syncing_ = false;
};

// Downscaled view of the whole composite (from the canvas' cached display tiles) with the visible
// viewport outlined; click or drag to pan, slider to zoom.
class NavigatorPanel : public QWidget {
    Q_OBJECT
public:
    NavigatorPanel(EditorSession* s, CanvasController* c, QWidget* parent = nullptr);

public slots:
    void schedule_refresh();
    void sync_zoom();

protected:
    bool eventFilter(QObject* o, QEvent* e) override;

private:
    void refresh();
    QRectF image_rect() const;
    EditorSession* s_;
    CanvasController* c_;
    QWidget* view_;
    QSlider* zoom_;
    QLabel* zoom_label_;
    QImage thumb_;
    QTimer timer_;
    bool syncing_ = false;
};

}  // namespace rl::gui
