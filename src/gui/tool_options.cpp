// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/tool_options.hpp"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QMenu>
#include <QWidgetAction>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QToolButton>

#include "gui/canvas.hpp"
#include "gui/icons.hpp"
#include "gui/session.hpp"
#include "gui/tools.hpp"

namespace rl::gui {

ToolOptionsBar::ToolOptionsBar(ToolState* t, CanvasController* c, EditorSession* s, QWidget* parent)
    : QToolBar(tr("Tool Options"), parent), t_(t), c_(c), s_(s) {
    setObjectName(QStringLiteral("ToolOptions"));
    setMovable(false);
    setFloatable(false);
    setIconSize(QSize(18, 18));
    setFixedHeight(38);
    connect(t_, &ToolState::tool_changed, this, &ToolOptionsBar::rebuild);
    connect(c_, &CanvasController::pending_changed, this, &ToolOptionsBar::rebuild);
    connect(s_, &EditorSession::active_layer_changed, this, [this] {
        if (t_->tool() == Tool::Brush) rebuild();
    });
    rebuild();
}

QLabel* ToolOptionsBar::add_label(const QString& text) {
    auto* l = new QLabel(text);
    addWidget(l);
    return l;
}

QDoubleSpinBox* ToolOptionsBar::add_double(const QString& label, double min, double max, double value, int decimals,
                                           const QString& suffix, const QString& tip) {
    QLabel* l = label.isEmpty() ? nullptr : add_label(label);
    auto* sb = new QDoubleSpinBox;
    if (l) {
        l->setToolTip(tip);  // hovering the caption explains the field too
        l->setBuddy(sb);
    }
    sb->setRange(min, max);
    sb->setDecimals(decimals);
    sb->setValue(value);
    sb->setSuffix(suffix);
    sb->setKeyboardTracking(false);
    sb->setToolTip(tip);
    sb->setAccelerated(true);
    sb->setMaximumWidth(84);
    addWidget(sb);
    return sb;
}

QSpinBox* ToolOptionsBar::add_int(const QString& label, int min, int max, int value, const QString& suffix, const QString& tip) {
    QLabel* l = label.isEmpty() ? nullptr : add_label(label);
    auto* sb = new QSpinBox;
    if (l) {
        l->setToolTip(tip);
        l->setBuddy(sb);
    }
    sb->setRange(min, max);
    sb->setValue(value);
    sb->setSuffix(suffix);
    sb->setKeyboardTracking(false);
    sb->setToolTip(tip);
    sb->setMaximumWidth(72);
    addWidget(sb);
    return sb;
}

QCheckBox* ToolOptionsBar::add_check(const QString& text, bool value, const QString& tip) {
    auto* cb = new QCheckBox(text);
    cb->setChecked(value);
    cb->setToolTip(tip);
    addWidget(cb);
    return cb;
}

void ToolOptionsBar::add_sel_modes() {
    auto* group = new QButtonGroup(this);
    const struct {
        SelMode m;
        const char* text;
        const char* tip;
    } modes[] = {{SelMode::New, "New", "New selection"},
                 {SelMode::Add, "Add", "Add to selection (Shift)"},
                 {SelMode::Subtract, "Sub", "Subtract from selection (Alt)"},
                 {SelMode::Intersect, "Int", "Intersect with selection (Shift+Alt)"}};
    for (const auto& m : modes) {
        auto* b = new QToolButton;
        b->setText(tr(m.text));
        b->setToolTip(tr(m.tip));
        b->setCheckable(true);
        b->setChecked(t_->settings().sel_mode == m.m);
        b->setObjectName(QStringLiteral("SelMode_%1").arg(QString::fromLatin1(m.text)));
        group->addButton(b);
        const SelMode mode = m.m;
        connect(b, &QToolButton::toggled, this, [this, mode](bool on) {
            if (on) {
                t_->settings().sel_mode = mode;
                t_->notify_settings();
            }
        });
        addWidget(b);
    }
    addSeparator();
}

void ToolOptionsBar::add_brush(bool clone) {
    const Tool tool = t_->tool();
    BrushSettings* b = t_->brush_settings_for(tool);
    if (!b) return;
    auto* size = add_double(tr("Size"), 1, 5000, b->size, 1, QStringLiteral(" px"), tr("<b>Size</b>: dab diameter in pixels ([ and ] change it)"));
    size->setObjectName(QStringLiteral("BrushSize"));
    connect(size, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this, b](double v) {
        b->size = v;
        t_->notify_settings();
    });
    connect(t_, &ToolState::settings_changed, size, [size, b] {
        if (size->value() != b->size) {
            const QSignalBlocker blk(size);
            size->setValue(b->size);
        }
    });
    // Tip shape: hardness and spacing sit next to size; then how paint is laid down: opacity, flow
    // and the Wash / Build-up mode. Every caption carries the same tooltip as its field.
    auto* hard = add_int(tr("Hardness"), 0, 100, static_cast<int>(std::lround(b->hardness * 100)), QStringLiteral(" %"),
                         tr("<b>Hardness</b>: how crisp the dab's edge is. 100 % = a hard edge; 0 % = fully soft, "
                            "fading from the centre to the rim."));
    hard->setObjectName(QStringLiteral("BrushHardness"));
    connect(hard, qOverload<int>(&QSpinBox::valueChanged), this, [b](int v) { b->hardness = v / 100.0; });
    auto* sp = add_int(tr("Spacing"), 1, 1000, static_cast<int>(std::lround(b->spacing * 100)), QStringLiteral(" %"),
                       tr("<b>Spacing</b>: distance between dabs as a percentage of the diameter. Lower = smoother "
                          "strokes (more dabs); 25 % is the usual default."));
    sp->setObjectName(QStringLiteral("BrushSpacing"));
    connect(sp, qOverload<int>(&QSpinBox::valueChanged), this, [b](int v) { b->spacing = v / 100.0; });
    addSeparator();
    auto* op = add_int(tr("Opacity"), 0, 100, static_cast<int>(std::lround(b->opacity * 100)), QStringLiteral(" %"),
                       tr("<b>Opacity</b>: the most one stroke can cover. In Wash mode a stroke never exceeds it, "
                          "however often it crosses itself."));
    op->setObjectName(QStringLiteral("BrushOpacity"));
    connect(op, qOverload<int>(&QSpinBox::valueChanged), this, [b](int v) { b->opacity = v / 100.0; });
    auto* flow = add_int(tr("Flow"), 0, 100, static_cast<int>(std::lround(b->flow * 100)), QStringLiteral(" %"),
                         tr("<b>Flow</b>: how much paint each dab adds. Low flow builds up gradually towards the "
                            "opacity as dabs overlap; 100 % reaches it at once."));
    flow->setObjectName(QStringLiteral("BrushFlow"));
    connect(flow, qOverload<int>(&QSpinBox::valueChanged), this, [b](int v) { b->flow = v / 100.0; });
    // Settings changed elsewhere (presets, tests, another tool's shortcut) show up in the bar.
    for (const auto& [box, field] : {std::pair<QSpinBox*, double BrushSettings::*>{hard, &BrushSettings::hardness},
                                     {sp, &BrushSettings::spacing}, {op, &BrushSettings::opacity}, {flow, &BrushSettings::flow}}) {
        QSpinBox* w = box;
        const auto f = field;
        connect(t_, &ToolState::settings_changed, w, [w, b, f] {
            const int v = static_cast<int>(std::lround(b->*f * 100));
            if (w->value() != v) {
                const QSignalBlocker blk(w);
                w->setValue(v);
            }
        });
    }
    auto* mode = new QComboBox;
    mode->addItem(tr("Wash"));
    mode->addItem(tr("Build-up"));
    mode->setCurrentIndex(b->buildup ? 1 : 0);
    mode->setObjectName(QStringLiteral("BrushMode"));
    mode->setToolTip(tr("<b>Wash</b>: a stroke never exceeds its opacity. <b>Build-up</b>: overlapping dabs keep "
                        "accumulating past it (airbrush-like)."));
    connect(mode, qOverload<int>(&QComboBox::currentIndexChanged), this, [b](int i) { b->buildup = i == 1; });
    addWidget(mode);
    // Secondary tip / dynamics settings live in a compact popup so the bar fits 1280 px.
    auto* more = new QToolButton;
    more->setObjectName(QStringLiteral("BrushTipMenu"));
    more->setText(tr("Tip"));
    more->setToolTip(tr("Angle, roundness, smoothing and airbrush"));
    more->setPopupMode(QToolButton::InstantPopup);
    auto* menu = new QMenu(more);
    auto* box = new QWidget;
    auto* form = new QFormLayout(box);
    form->setContentsMargins(10, 8, 10, 8);
    auto* ang = new QDoubleSpinBox;
    ang->setRange(-360, 360);
    ang->setDecimals(0);
    ang->setSuffix(QStringLiteral("°"));
    ang->setValue(b->angle);
    auto* rnd = new QSpinBox;
    rnd->setRange(1, 100);
    rnd->setSuffix(QStringLiteral(" %"));
    rnd->setValue(static_cast<int>(std::lround(b->roundness * 100)));
    auto* sm = new QSpinBox;
    sm->setRange(0, 99);
    sm->setSuffix(QStringLiteral(" %"));
    sm->setValue(static_cast<int>(std::lround(b->smoothing * 100)));
    sm->setToolTip(tr("Stabilizer strength"));
    auto* air = new QDoubleSpinBox;
    air->setRange(0, 1000);
    air->setDecimals(0);
    air->setSuffix(tr(" dabs/s"));
    air->setValue(b->dabs_per_second);
    air->setToolTip(tr("Airbrush: dabs per second while the pointer is held still (0 = off)"));
    for (QAbstractSpinBox* w : {static_cast<QAbstractSpinBox*>(ang), static_cast<QAbstractSpinBox*>(rnd),
                                static_cast<QAbstractSpinBox*>(sm), static_cast<QAbstractSpinBox*>(air)})
        w->setKeyboardTracking(false);
    form->addRow(tr("Angle"), ang);
    form->addRow(tr("Roundness"), rnd);
    form->addRow(tr("Smoothing"), sm);
    form->addRow(tr("Airbrush"), air);
    auto* wa = new QWidgetAction(menu);
    wa->setDefaultWidget(box);
    menu->addAction(wa);
    more->setMenu(menu);
    addWidget(more);
    connect(ang, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this, b](double v) {
        b->angle = v;
        t_->notify_settings();
    });
    connect(rnd, qOverload<int>(&QSpinBox::valueChanged), this, [this, b](int v) {
        b->roundness = v / 100.0;
        t_->notify_settings();
    });
    connect(sm, qOverload<int>(&QSpinBox::valueChanged), this, [b](int v) { b->smoothing = v / 100.0; });
    connect(air, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [b](double v) { b->dabs_per_second = v; });
    addSeparator();
    add_label(tr("Pressure:"));
    auto* ps = add_check(tr("Size"), b->pressure_size, tr("Pen pressure controls size"));
    connect(ps, &QCheckBox::toggled, this, [b](bool on) { b->pressure_size = on; });
    auto* po = add_check(tr("Opacity"), b->pressure_opacity, tr("Pen pressure controls opacity"));
    connect(po, &QCheckBox::toggled, this, [b](bool on) { b->pressure_opacity = on; });
    if (clone) {
        addSeparator();
        auto* al = add_check(tr("Aligned"), t_->settings().clone_aligned,
                             tr("Aligned: the source moves with the brush across strokes"));
        connect(al, &QCheckBox::toggled, this, [this](bool on) { t_->settings().clone_aligned = on; });
        add_label(c_->clone_source() ? tr("Alt-click sets a new source") : tr("Alt-click to set the source"));
    }
    if (tool == Tool::Brush && s_->edit_mask()) {
        addSeparator();
        auto* l = new QLabel(tr("Painting the layer mask"));
        l->setObjectName(QStringLiteral("MessageBadge"));
        addWidget(l);
    }
}

void ToolOptionsBar::rebuild() {
    clear();
    const Tool tool = t_->tool();
    auto* title = new QLabel(QStringLiteral("<b>%1</b>").arg(tr(tool_info(tool).name)));
    title->setStyleSheet(QStringLiteral("color:#dcdee2; padding-right:8px;"));
    addWidget(title);
    ToolSettings& s = t_->settings();
    switch (tool) {
        case Tool::Brush:
        case Tool::Eraser: add_brush(false); break;
        case Tool::Clone: add_brush(true); break;
        case Tool::MarqueeRect:
        case Tool::MarqueeEllipse: {
            add_sel_modes();
            const bool ell = tool == Tool::MarqueeEllipse;
            auto* aa = add_check(tr("Anti-alias"), ell ? s.ellipse_antialias : s.marquee_antialias, tr("Soft edge coverage"));
            connect(aa, &QCheckBox::toggled, this, [this, ell](bool on) {
                (ell ? t_->settings().ellipse_antialias : t_->settings().marquee_antialias) = on;
            });
            add_label(tr("   Drag to select; click to deselect"));
            break;
        }
        case Tool::Lasso:
        case Tool::PolygonLasso: {
            add_sel_modes();
            auto* aa = add_check(tr("Anti-alias"), s.lasso_antialias, tr("Soft edge coverage"));
            connect(aa, &QCheckBox::toggled, this, [this](bool on) { t_->settings().lasso_antialias = on; });
            add_label(tool == Tool::Lasso ? tr("   Drag around an area")
                                          : tr("   Click to add points; double-click, Enter or click the first point to close"));
            break;
        }
        case Tool::Wand: {
            add_sel_modes();
            auto* tol = add_int(tr("Tolerance"), 0, 255, s.wand_tolerance, QString(), tr("Colour distance that still matches"));
            connect(tol, qOverload<int>(&QSpinBox::valueChanged), this, [this](int v) { t_->settings().wand_tolerance = v; });
            auto* ct = add_check(tr("Contiguous"), s.wand_contiguous, tr("Only connected pixels"));
            connect(ct, &QCheckBox::toggled, this, [this](bool on) { t_->settings().wand_contiguous = on; });
            auto* aa = add_check(tr("Anti-alias"), s.wand_antialias, tr("Soft edge coverage"));
            connect(aa, &QCheckBox::toggled, this, [this](bool on) { t_->settings().wand_antialias = on; });
            break;
        }
        case Tool::Bucket: {
            auto* tol = add_int(tr("Tolerance"), 0, 255, s.bucket_tolerance, QString(), tr("Colour distance that still matches"));
            connect(tol, qOverload<int>(&QSpinBox::valueChanged), this, [this](int v) { t_->settings().bucket_tolerance = v; });
            auto* op = add_int(tr("Opacity"), 0, 100, static_cast<int>(std::lround(s.bucket_opacity * 100)), QStringLiteral(" %"), QString());
            connect(op, qOverload<int>(&QSpinBox::valueChanged), this, [this](int v) { t_->settings().bucket_opacity = v / 100.0; });
            auto* ct = add_check(tr("Contiguous"), s.bucket_contiguous, tr("Only connected pixels"));
            connect(ct, &QCheckBox::toggled, this, [this](bool on) { t_->settings().bucket_contiguous = on; });
            auto* aa = add_check(tr("Anti-alias"), s.bucket_antialias, tr("Soft fill edge"));
            connect(aa, &QCheckBox::toggled, this, [this](bool on) { t_->settings().bucket_antialias = on; });
            break;
        }
        case Tool::Gradient: {
            auto* type = new QComboBox;
            type->addItem(tr("Linear"));
            type->addItem(tr("Radial"));
            type->setCurrentIndex(s.gradient_radial ? 1 : 0);
            connect(type, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int i) { t_->settings().gradient_radial = i == 1; });
            addWidget(type);
            auto* op = add_int(tr("Opacity"), 0, 100, static_cast<int>(std::lround(s.gradient_opacity * 100)), QStringLiteral(" %"), QString());
            connect(op, qOverload<int>(&QSpinBox::valueChanged), this, [this](int v) { t_->settings().gradient_opacity = v / 100.0; });
            auto* rev = add_check(tr("Reverse"), s.gradient_reverse, tr("Swap the two colours"));
            connect(rev, &QCheckBox::toggled, this, [this](bool on) { t_->settings().gradient_reverse = on; });
            add_label(tr("   Foreground to background; drag on the canvas"));
            break;
        }
        case Tool::Move: add_label(tr("Drag to move the active layer; arrow keys nudge 1 px (Shift: 10 px)")); break;
        case Tool::Crop: {
            if (c_->has_pending()) {
                const QRect r = c_->crop_rect();
                add_label(tr("%1 × %2 at %3, %4").arg(r.width()).arg(r.height()).arg(r.x()).arg(r.y()));
                auto* ok = new QPushButton(icon(QStringLiteral("check")), tr("Crop"));
                auto* no = new QPushButton(icon(QStringLiteral("cancel")), tr("Cancel"));
                connect(ok, &QPushButton::clicked, c_, &CanvasController::commit_pending);
                connect(no, &QPushButton::clicked, c_, &CanvasController::cancel_pending);
                addWidget(ok);
                addWidget(no);
            } else {
                add_label(tr("Drag a rectangle, then press Enter"));
            }
            break;
        }
        case Tool::Transform: {
            auto* in = new QComboBox;
            in->addItem(tr("Bicubic"));
            in->addItem(tr("Nearest"));
            in->setCurrentIndex(s.transform_bicubic ? 0 : 1);
            connect(in, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int i) { t_->settings().transform_bicubic = i == 0; });
            add_label(tr("Interpolation"));
            addWidget(in);
            auto* ok = new QPushButton(icon(QStringLiteral("check")), tr("Apply"));
            auto* no = new QPushButton(icon(QStringLiteral("cancel")), tr("Cancel"));
            connect(ok, &QPushButton::clicked, c_, &CanvasController::commit_pending);
            connect(no, &QPushButton::clicked, c_, &CanvasController::cancel_pending);
            addWidget(ok);
            addWidget(no);
            break;
        }
        case Tool::Hand:
        case Tool::Zoom: {
            auto* fit = new QPushButton(icon(QStringLiteral("fit")), tr("Fit"));
            auto* one = new QPushButton(icon(QStringLiteral("actual")), tr("100 %"));
            connect(fit, &QPushButton::clicked, c_, &CanvasController::fit_to_view);
            connect(one, &QPushButton::clicked, c_, &CanvasController::actual_pixels);
            addWidget(fit);
            addWidget(one);
            if (tool == Tool::Zoom) add_label(tr("   Click to zoom in, Alt-click to zoom out, drag a rectangle to zoom to it"));
            else add_label(tr("   Drag to pan (Space anywhere)"));
            break;
        }
        case Tool::Eyedropper: add_label(tr("Click samples the merged image into the foreground; Alt-click into the background")); break;
    }
}

}  // namespace rl::gui
