// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/panels.hpp"

#include <QColorDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QRegularExpressionValidator>
#include <QSlider>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>

#include "core/composite/render.hpp"
#include "gui/canvas.hpp"
#include "gui/icons.hpp"
#include "gui/session.hpp"
#include "gui/theme.hpp"
#include "gui/tools.hpp"

namespace rl::gui {

// ---- History ------------------------------------------------------------------------------------------

HistoryPanel::HistoryPanel(EditorSession* s, QWidget* parent) : QWidget(parent), s_(s) {
    setObjectName(QStringLiteral("HistoryPanel"));
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    list_ = new QListWidget;
    list_->setObjectName(QStringLiteral("HistoryList"));
    list_->setIconSize(QSize(16, 16));
    v->addWidget(list_, 1);
    auto* foot = new QWidget;
    foot->setObjectName(QStringLiteral("PanelFooterRow"));
    auto* fl = new QHBoxLayout(foot);
    fl->setContentsMargins(8, 4, 8, 4);
    footer_ = new QLabel;
    footer_->setStyleSheet(QStringLiteral("color:#8d9199;"));
    fl->addWidget(footer_);
    v->addWidget(foot);
    connect(s_, &EditorSession::history_changed, this, &HistoryPanel::rebuild);
    connect(s_, &EditorSession::document_reset, this, &HistoryPanel::rebuild);
    connect(list_, &QListWidget::itemClicked, this, [this](QListWidgetItem* it) {
        s_->jump_to(static_cast<size_t>(list_->row(it)));
    });
}

void HistoryPanel::rebuild() {
    list_->blockSignals(true);
    list_->clear();
    if (!s_->has_document()) {
        list_->blockSignals(false);
        return;
    }
    auto* base = new QListWidgetItem(icon(QStringLiteral("new_doc")), s_->base_label());
    list_->addItem(base);
    const auto& e = s_->entries();
    for (size_t i = 0; i < e.size(); ++i) {
        auto* it = new QListWidgetItem(icon(QStringLiteral("history")), e[i].label);
        it->setToolTip(QString::fromStdString(e[i].op.dump()).left(600));
        if (i >= s_->applied_count()) {
            it->setForeground(QColor(0x6a, 0x6e, 0x75));
            QFont f = it->font();
            f.setItalic(true);
            it->setFont(f);
        }
        list_->addItem(it);
    }
    list_->setCurrentRow(static_cast<int>(s_->applied_count()));
    list_->scrollToItem(list_->currentItem());
    list_->blockSignals(false);
    footer_->setText(tr("%1 of %2 states, depth %3").arg(s_->applied_count()).arg(e.size()).arg(s_->doc().history().depth()));
}

// ---- HSV picker -----------------------------------------------------------------------------------------

HsvPicker::HsvPicker(QWidget* parent) : QWidget(parent) {
    setMinimumSize(160, 110);
    setCursor(Qt::CrossCursor);
}

void HsvPicker::set_color(const QColor& c) {
    float h, s, v;
    c.getHsvF(&h, &s, &v);
    if (h >= 0) h_ = h;  // achromatic colours keep the current hue
    if (v > 0) s_ = s;
    v_ = v;
    update();
}

QRect HsvPicker::sv_rect() const { return QRect(4, 4, width() - 36, height() - 8); }
QRect HsvPicker::hue_rect() const { return QRect(width() - 24, 4, 18, height() - 8); }

void HsvPicker::resizeEvent(QResizeEvent*) { sv_cache_h_ = -1.0f; }

void HsvPicker::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRect sv = sv_rect(), hr = hue_rect();
    if (sv_cache_h_ != h_ || sv_cache_.size() != sv.size()) {
        sv_cache_ = QImage(sv.size(), QImage::Format_RGB32);
        QPainter g(&sv_cache_);
        QLinearGradient hor(0, 0, sv.width(), 0);
        hor.setColorAt(0, Qt::white);
        hor.setColorAt(1, QColor::fromHsvF(h_, 1, 1));
        g.fillRect(sv_cache_.rect(), hor);
        QLinearGradient ver(0, 0, 0, sv.height());
        ver.setColorAt(0, QColor(0, 0, 0, 0));
        ver.setColorAt(1, Qt::black);
        g.fillRect(sv_cache_.rect(), ver);
        sv_cache_h_ = h_;
    }
    p.drawImage(sv.topLeft(), sv_cache_);
    QLinearGradient hue(0, hr.top(), 0, hr.bottom());
    for (int i = 0; i <= 6; ++i) hue.setColorAt(i / 6.0, QColor::fromHsvF(1.0f - i / 6.0f > 0.9999f ? 0.0f : 1.0f - i / 6.0f, 1, 1));
    p.fillRect(hr, hue);
    p.setPen(QColor(0, 0, 0, 160));
    p.setBrush(Qt::NoBrush);
    p.drawRect(sv.adjusted(0, 0, -1, -1));
    p.drawRect(hr.adjusted(0, 0, -1, -1));
    const QPointF m(sv.left() + s_ * (sv.width() - 1), sv.top() + (1 - v_) * (sv.height() - 1));
    p.setPen(QPen(Qt::black, 2));
    p.drawEllipse(m, 5, 5);
    p.setPen(QPen(Qt::white, 1.2));
    p.drawEllipse(m, 5, 5);
    const double hy = hr.top() + (1 - h_) * (hr.height() - 1);
    p.setPen(QPen(Qt::white, 2));
    p.drawRect(QRectF(hr.left() - 2, hy - 2, hr.width() + 3, 4));
}

void HsvPicker::pick(const QPoint& pt, bool start) {
    const QRect sv = sv_rect(), hr = hue_rect();
    if (start) drag_ = sv.adjusted(-3, -3, 3, 3).contains(pt) ? 1 : hr.adjusted(-4, -3, 4, 3).contains(pt) ? 2 : 0;
    if (drag_ == 1) {
        s_ = std::clamp(float(pt.x() - sv.left()) / float(sv.width() - 1), 0.0f, 1.0f);
        v_ = 1.0f - std::clamp(float(pt.y() - sv.top()) / float(sv.height() - 1), 0.0f, 1.0f);
    } else if (drag_ == 2) {
        h_ = 1.0f - std::clamp(float(pt.y() - hr.top()) / float(hr.height() - 1), 0.0f, 1.0f);
        if (h_ >= 1.0f) h_ = 0.0f;
    } else {
        return;
    }
    update();
    emit color_edited(color());
}

void HsvPicker::mousePressEvent(QMouseEvent* e) { pick(e->position().toPoint(), true); }
void HsvPicker::mouseMoveEvent(QMouseEvent* e) {
    if (e->buttons() & Qt::LeftButton) pick(e->position().toPoint(), false);
}

// ---- FG/BG swatch -----------------------------------------------------------------------------------------

FgBgSwatch::FgBgSwatch(ToolState* t, QWidget* parent) : QWidget(parent), t_(t) {
    setFixedSize(64, 60);
    setToolTip(tr("Foreground / background colour. Click to choose which one the picker edits, double-click for a dialog. X swaps, D resets."));
    connect(t_, &ToolState::colors_changed, this, qOverload<>(&QWidget::update));
}

void FgBgSwatch::paintEvent(QPaintEvent*) {
    QPainter p(this);
    const QRect fg(2, 2, 36, 36), bg(24, 20, 36, 36);
    const auto sw = [&](const QRect& r, const QColor& c, bool active) {
        p.fillRect(r, Qt::white);
        p.fillRect(r.adjusted(1, 1, -1, -1), Qt::black);
        p.fillRect(r.adjusted(2, 2, -2, -2), c);
        if (active) {
            p.setPen(QPen(theme::kAccent, 2));
            p.drawRect(r.adjusted(-1, -1, 0, 0));
        }
    };
    sw(bg, t_->bg(), target_bg_);
    sw(fg, t_->fg(), !target_bg_);
}

void FgBgSwatch::mousePressEvent(QMouseEvent* e) {
    const QRect fg(2, 2, 36, 36);
    target_bg_ = !fg.contains(e->position().toPoint());
    emit editing_target_changed(target_bg_);
    update();
}

void FgBgSwatch::mouseDoubleClickEvent(QMouseEvent*) {
    const QColor c = QColorDialog::getColor(target_bg_ ? t_->bg() : t_->fg(), this,
                                            target_bg_ ? tr("Background Colour") : tr("Foreground Colour"));
    if (!c.isValid()) return;
    if (target_bg_) t_->set_bg(c);
    else t_->set_fg(c);
}

// ---- Color panel -------------------------------------------------------------------------------------------

ColorPanel::ColorPanel(ToolState* t, QWidget* parent) : QWidget(parent), t_(t) {
    setObjectName(QStringLiteral("ColorPanel"));
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(8, 8, 8, 8);
    v->setSpacing(6);
    auto* top = new QHBoxLayout;
    swatch_ = new FgBgSwatch(t);
    top->addWidget(swatch_);
    auto* btns = new QVBoxLayout;
    auto* swap = new QToolButton;
    swap->setIcon(icon(QStringLiteral("swap")));
    swap->setToolTip(tr("Swap colours (X)"));
    swap->setAutoRaise(true);
    auto* reset = new QToolButton;
    reset->setText(QStringLiteral("D"));
    reset->setToolTip(tr("Default colours: black / white (D)"));
    reset->setAutoRaise(true);
    btns->addWidget(swap);
    btns->addWidget(reset);
    top->addLayout(btns);
    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(4);
    grid->setVerticalSpacing(2);
    const auto mk = [&](const QString& name, int max, int row, int col) {
        auto* l = new QLabel(name);
        l->setStyleSheet(QStringLiteral("color:#8d9199;"));
        auto* sb = new QSpinBox;
        sb->setRange(0, max);
        sb->setKeyboardTracking(false);
        sb->setButtonSymbols(QAbstractSpinBox::NoButtons);
        sb->setFixedWidth(42);
        grid->addWidget(l, row, col * 2);
        grid->addWidget(sb, row, col * 2 + 1);
        return sb;
    };
    h_ = mk(QStringLiteral("H"), 359, 0, 0);
    s_ = mk(QStringLiteral("S"), 100, 1, 0);
    v_ = mk(QStringLiteral("V"), 100, 2, 0);
    r_ = mk(QStringLiteral("R"), 255, 0, 1);
    g_ = mk(QStringLiteral("G"), 255, 1, 1);
    b_ = mk(QStringLiteral("B"), 255, 2, 1);
    top->addStretch(1);
    top->addLayout(grid);
    v->addLayout(top);
    picker_ = new HsvPicker;
    v->addWidget(picker_, 1);
    auto* hexrow = new QHBoxLayout;
    auto* hl = new QLabel(QStringLiteral("#"));
    hl->setStyleSheet(QStringLiteral("color:#8d9199;"));
    hex_ = new QLineEdit;
    hex_->setObjectName(QStringLiteral("HexColor"));
    hex_->setMaxLength(6);
    hex_->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("[0-9A-Fa-f]{0,6}")), hex_));
    hexrow->addWidget(hl);
    hexrow->addWidget(hex_);
    v->addLayout(hexrow);

    connect(swap, &QToolButton::clicked, t_, &ToolState::swap_colors);
    connect(reset, &QToolButton::clicked, t_, &ToolState::reset_colors);
    connect(t_, &ToolState::colors_changed, this, &ColorPanel::sync);
    connect(swatch_, &FgBgSwatch::editing_target_changed, this, [this](bool bg) {
        target_bg_ = bg;
        sync();
    });
    connect(picker_, &HsvPicker::color_edited, this, &ColorPanel::set_edited);
    for (QSpinBox* sb : {h_, s_, v_}) {
        connect(sb, qOverload<int>(&QSpinBox::valueChanged), this, [this] {
            if (syncing_) return;
            set_edited(QColor::fromHsv(h_->value(), s_->value() * 255 / 100, v_->value() * 255 / 100));
        });
    }
    for (QSpinBox* sb : {r_, g_, b_}) {
        connect(sb, qOverload<int>(&QSpinBox::valueChanged), this, [this] {
            if (syncing_) return;
            set_edited(QColor(r_->value(), g_->value(), b_->value()));
        });
    }
    connect(hex_, &QLineEdit::editingFinished, this, [this] {
        if (hex_->text().size() == 6) set_edited(QColor(QStringLiteral("#") + hex_->text()));
    });
    sync();
}

void ColorPanel::set_edited(const QColor& c) {
    if (target_bg_) t_->set_bg(c);
    else t_->set_fg(c);
    sync();
}

void ColorPanel::sync() {
    syncing_ = true;
    const QColor c = target_bg_ ? t_->bg() : t_->fg();
    if (!(picker_->color().rgb() == c.rgb())) picker_->set_color(c);
    int h, s, v;
    c.getHsv(&h, &s, &v);
    h_->setValue(std::max(0, h));
    s_->setValue(static_cast<int>(std::lround(s * 100.0 / 255)));
    v_->setValue(static_cast<int>(std::lround(v * 100.0 / 255)));
    r_->setValue(c.red());
    g_->setValue(c.green());
    b_->setValue(c.blue());
    hex_->setText(c.name().mid(1).toUpper());
    syncing_ = false;
}

// ---- Navigator ------------------------------------------------------------------------------------------------

NavigatorPanel::NavigatorPanel(EditorSession* s, CanvasController* c, QWidget* parent)
    : QWidget(parent), s_(s), c_(c) {
    setObjectName(QStringLiteral("NavigatorPanel"));
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    view_ = new QWidget;
    view_->setMinimumHeight(120);
    view_->installEventFilter(this);
    view_->setCursor(Qt::PointingHandCursor);
    v->addWidget(view_, 1);
    auto* foot = new QWidget;
    foot->setObjectName(QStringLiteral("PanelFooterRow"));
    auto* fl = new QHBoxLayout(foot);
    fl->setContentsMargins(6, 3, 6, 3);
    auto* fit = new QToolButton;
    fit->setIcon(icon(QStringLiteral("fit")));
    fit->setToolTip(tr("Fit on screen (Ctrl+0)"));
    fit->setAutoRaise(true);
    zoom_ = new QSlider(Qt::Horizontal);
    zoom_->setRange(0, 1000);  // log scale over 2 % .. 6400 %
    zoom_label_ = new QLabel;
    zoom_label_->setMinimumWidth(48);
    zoom_label_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    auto* actual = new QToolButton;
    actual->setIcon(icon(QStringLiteral("actual")));
    actual->setToolTip(tr("100 % (Ctrl+1)"));
    actual->setAutoRaise(true);
    fl->addWidget(fit);
    fl->addWidget(zoom_, 1);
    fl->addWidget(actual);
    fl->addWidget(zoom_label_);
    v->addWidget(foot);

    timer_.setSingleShot(true);
    timer_.setInterval(150);
    connect(&timer_, &QTimer::timeout, this, &NavigatorPanel::refresh);
    connect(s_, &EditorSession::tiles_dirty, this, &NavigatorPanel::schedule_refresh);
    // A new or opened document shows its thumbnail right away (edits stay debounced).
    connect(s_, &EditorSession::document_reset, this, [this] { QTimer::singleShot(0, this, &NavigatorPanel::refresh); });
    connect(c_, &CanvasController::view_changed, this, [this] {
        sync_zoom();
        view_->update();
    });
    connect(fit, &QToolButton::clicked, c_, &CanvasController::fit_to_view);
    connect(actual, &QToolButton::clicked, c_, &CanvasController::actual_pixels);
    connect(zoom_, &QSlider::valueChanged, this, [this](int v) {
        if (syncing_ || !c_->widget()) return;
        const double z = std::exp(std::log(0.02) + (std::log(64.0) - std::log(0.02)) * v / 1000.0);
        c_->set_zoom(z, QPointF(c_->widget()->width() / 2.0, c_->widget()->height() / 2.0));
    });
    sync_zoom();
}

void NavigatorPanel::schedule_refresh() { timer_.start(); }

void NavigatorPanel::sync_zoom() {
    syncing_ = true;
    const double z = c_->zoom();
    zoom_->setValue(static_cast<int>(std::lround((std::log(z) - std::log(0.02)) / (std::log(64.0) - std::log(0.02)) * 1000.0)));
    zoom_label_->setText(QStringLiteral("%1 %").arg(z * 100.0, 0, 'f', z < 0.1 ? 1 : 0));
    syncing_ = false;
}

QRectF NavigatorPanel::image_rect() const {
    if (thumb_.isNull()) return {};
    const QSizeF s = QSizeF(thumb_.size()).scaled(QSizeF(view_->size()) - QSizeF(16, 16), Qt::KeepAspectRatio);
    return QRectF(QPointF((view_->width() - s.width()) / 2.0, (view_->height() - s.height()) / 2.0), s);
}

void NavigatorPanel::refresh() {
    if (!s_->has_document() || c_->tiles_x() == 0) {
        thumb_ = QImage();
        view_->update();
        return;
    }
    // Build from the canvas' display pyramid at the level nearest the thumbnail scale (cached;
    // level-0 renders are budgeted so a huge document never stalls the UI; an incomplete
    // thumbnail refreshes again when the canvas has rendered more).
    const int W = s_->doc().width(), H = s_->doc().height();
    const double sc = std::min(1.0, 256.0 / std::max(W, H));
    const int tw = std::max(1, static_cast<int>(std::lround(W * sc))), th = std::max(1, static_cast<int>(std::lround(H * sc)));
    const int l = std::min(CanvasController::level_for_zoom(sc), c_->level_count() - 1);
    const double ls = static_cast<double>(1 << l);
    QImage img(tw, th, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    c_->set_render_budget_ms(8.0);
    {
        QPainter p(&img);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        for (int ty = 0; ty < c_->level_tiles_y(l); ++ty)
            for (int tx = 0; tx < c_->level_tiles_x(l); ++tx) {
                const QRectF cr = c_->level_tile_canvas_rect(l, tx, ty);
                const QImage* t = c_->level_tile(l, tx, ty);
                if (!t) continue;
                p.drawImage(QRectF(cr.x() * sc, cr.y() * sc, cr.width() * sc, cr.height() * sc), *t,
                            QRectF(0, 0, cr.width() / ls, cr.height() / ls));
            }
    }
    const bool complete = c_->display_complete();
    c_->set_render_budget_ms(0.0);
    if (!complete) timer_.start();
    thumb_ = img;
    view_->update();
}

bool NavigatorPanel::eventFilter(QObject* o, QEvent* e) {
    if (o != view_) return QWidget::eventFilter(o, e);
    if (e->type() == QEvent::Paint) {
        QPainter p(view_);
        p.fillRect(view_->rect(), theme::kWorkspace);
        const QRectF ir = image_rect();
        if (!ir.isEmpty()) {
            const int c = 6;
            p.save();
            p.setClipRect(ir);
            for (int y = 0; y < ir.height(); y += c)
                for (int x = 0; x < ir.width(); x += c)
                    p.fillRect(QRectF(ir.left() + x, ir.top() + y, c, c), ((x + y) / c) % 2 ? theme::kChecker1 : theme::kChecker2);
            p.restore();
            p.setRenderHint(QPainter::SmoothPixmapTransform);
            p.drawImage(ir, thumb_);
            const QRectF vis = c_->visible_canvas_rect();
            if (!vis.isEmpty() && s_->has_document()) {
                const double k = ir.width() / s_->doc().width();
                const QRectF vr(ir.left() + vis.left() * k, ir.top() + vis.top() * k, vis.width() * k, vis.height() * k);
                p.setPen(QPen(QColor(0xe0, 0x4a, 0x4a), 1.5));
                p.setBrush(Qt::NoBrush);
                p.drawRect(vr);
            }
        }
        return true;
    }
    if ((e->type() == QEvent::MouseButtonPress || e->type() == QEvent::MouseMove) && s_->has_document()) {
        auto* me = static_cast<QMouseEvent*>(e);
        if (e->type() == QEvent::MouseMove && !(me->buttons() & Qt::LeftButton)) return false;
        const QRectF ir = image_rect();
        if (ir.isEmpty()) return true;
        const double k = s_->doc().width() / ir.width();
        c_->center_on(QPointF((me->position().x() - ir.left()) * k, (me->position().y() - ir.top()) * k));
        return true;
    }
    if (e->type() == QEvent::Resize) view_->update();
    return QWidget::eventFilter(o, e);
}

}  // namespace rl::gui
