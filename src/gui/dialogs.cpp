// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/dialogs.hpp"

#include <QButtonGroup>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

#include "core/adjust/adjustment.hpp"
#include "gui/json_util.hpp"
#include "gui/theme.hpp"

namespace rl::gui {

// ---- ParamSpec factories ------------------------------------------------------------------------------

ParamSpec ParamSpec::i(std::string key, QString label, int min, int max, int def, QString suffix) {
    ParamSpec p;
    p.kind = Kind::Int;
    p.key = std::move(key);
    p.label = std::move(label);
    p.min = min;
    p.max = max;
    p.def = def;
    p.suffix = std::move(suffix);
    return p;
}
ParamSpec ParamSpec::d(std::string key, QString label, double min, double max, double def, int decimals, QString suffix) {
    ParamSpec p;
    p.kind = Kind::Double;
    p.key = std::move(key);
    p.label = std::move(label);
    p.min = min;
    p.max = max;
    p.def = def;
    p.decimals = decimals;
    p.suffix = std::move(suffix);
    return p;
}
ParamSpec ParamSpec::b(std::string key, QString label, bool def) {
    ParamSpec p;
    p.kind = Kind::Bool;
    p.key = std::move(key);
    p.label = std::move(label);
    p.def = def ? 1 : 0;
    return p;
}
ParamSpec ParamSpec::e(std::string key, QString label, QStringList labels, QStringList values, QString def) {
    ParamSpec p;
    p.kind = Kind::Enum;
    p.key = std::move(key);
    p.label = std::move(label);
    p.enum_labels = std::move(labels);
    p.enum_values = std::move(values);
    p.enum_def = std::move(def);
    return p;
}
ParamSpec ParamSpec::c(std::string key, QString label, QColor def) {
    ParamSpec p;
    p.kind = Kind::Color;
    p.key = std::move(key);
    p.label = std::move(label);
    p.color_def = def;
    return p;
}
ParamSpec ParamSpec::oc(std::string key, QString label, QColor def, bool on) {
    ParamSpec p = c(std::move(key), std::move(label), def);
    p.kind = Kind::OptColor;
    p.opt_on = on;
    return p;
}
ParamSpec ParamSpec::anchor(std::string key, QString label) {
    ParamSpec p;
    p.kind = Kind::Anchor;
    p.key = std::move(key);
    p.label = std::move(label);
    return p;
}

namespace {

class ColorButton : public QToolButton {
public:
    explicit ColorButton(const QColor& c, QWidget* parent = nullptr) : QToolButton(parent), c_(c) {
        setFixedSize(48, 24);
        connect(this, &QToolButton::clicked, this, [this] {
            const QColor n = QColorDialog::getColor(c_, this);
            if (n.isValid()) {
                c_ = n;
                update();
                if (changed) changed();
            }
        });
    }
    QColor color() const { return c_; }
    std::function<void()> changed;

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), QColor(0x16, 0x17, 0x1a));
        p.fillRect(rect().adjusted(2, 2, -2, -2), isEnabled() ? c_ : c_.darker(250));
    }

private:
    QColor c_;
};

}  // namespace

// ---- ParamDialog ---------------------------------------------------------------------------------------------

ParamDialog::ParamDialog(const QString& title, const std::vector<ParamSpec>& specs, bool with_preview, QWidget* parent)
    : QDialog(parent), specs_(specs) {
    setWindowTitle(title);
    setObjectName(QStringLiteral("ParamDialog"));
    setMinimumWidth(380);  // readable fields and notes, whatever the parameter count
    debounce_.setSingleShot(true);
    debounce_.setInterval(120);
    connect(&debounce_, &QTimer::timeout, this, &ParamDialog::changed);
    auto* v = new QVBoxLayout(this);
    v->setSizeConstraint(QLayout::SetMinimumSize);
    form_ = new QFormLayout;
    form_->setLabelAlignment(Qt::AlignRight);
    form_->setHorizontalSpacing(12);
    for (const ParamSpec& s : specs_) {
        QWidget* w = nullptr;
        switch (s.kind) {
            case ParamSpec::Kind::Int: {
                auto* sb = new QSpinBox;
                sb->setRange(static_cast<int>(s.min), static_cast<int>(s.max));
                sb->setValue(static_cast<int>(s.def));
                sb->setSuffix(s.suffix);
                sb->setMinimumWidth(110);
                connect(sb, qOverload<int>(&QSpinBox::valueChanged), this, [this] { emit_changed_later(); });
                w = sb;
                break;
            }
            case ParamSpec::Kind::Double: {
                auto* sb = new QDoubleSpinBox;
                sb->setRange(s.min, s.max);
                sb->setDecimals(s.decimals);
                sb->setValue(s.def);
                sb->setSuffix(s.suffix);
                sb->setMinimumWidth(110);
                sb->setSingleStep(std::pow(10.0, -std::min(s.decimals, 1)));
                connect(sb, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this] { emit_changed_later(); });
                w = sb;
                break;
            }
            case ParamSpec::Kind::Bool: {
                auto* cb = new QCheckBox;
                cb->setChecked(s.def != 0);
                connect(cb, &QCheckBox::toggled, this, [this] { emit_changed_later(); });
                w = cb;
                break;
            }
            case ParamSpec::Kind::Enum: {
                auto* cb = new QComboBox;
                for (int k = 0; k < s.enum_labels.size(); ++k) cb->addItem(s.enum_labels[k], s.enum_values[k]);
                cb->setCurrentIndex(std::max(0, cb->findData(s.enum_def)));
                connect(cb, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { emit_changed_later(); });
                w = cb;
                break;
            }
            case ParamSpec::Kind::Color: {
                auto* cbtn = new ColorButton(s.color_def);
                cbtn->changed = [this] { emit_changed_later(); };
                w = cbtn;
                break;
            }
            case ParamSpec::Kind::OptColor: {
                auto* box = new QWidget;
                auto* h = new QHBoxLayout(box);
                h->setContentsMargins(0, 0, 0, 0);
                auto* cb = new QCheckBox;
                cb->setObjectName(QStringLiteral("on"));
                cb->setChecked(s.opt_on);
                auto* cbtn = new ColorButton(s.color_def);
                cbtn->setObjectName(QStringLiteral("color"));
                cbtn->setEnabled(s.opt_on);
                connect(cb, &QCheckBox::toggled, cbtn, &QWidget::setEnabled);
                connect(cb, &QCheckBox::toggled, this, [this] { emit_changed_later(); });
                cbtn->changed = [this] { emit_changed_later(); };
                h->addWidget(cb);
                h->addWidget(cbtn);
                h->addStretch(1);
                w = box;
                break;
            }
            case ParamSpec::Kind::Anchor: {
                auto* box = new QWidget;
                auto* g = new QGridLayout(box);
                g->setSpacing(2);
                g->setContentsMargins(0, 0, 0, 0);
                auto* group = new QButtonGroup(box);
                group->setObjectName(QStringLiteral("anchor"));
                const char* names[9] = {"tl", "t", "tr", "l", "c", "r", "bl", "b", "br"};
                for (int k = 0; k < 9; ++k) {
                    auto* b = new QToolButton;
                    b->setCheckable(true);
                    b->setFixedSize(24, 24);
                    b->setProperty("anchor", QString::fromLatin1(names[k]));
                    b->setText(k == 4 ? QStringLiteral("●") : QString());
                    group->addButton(b, k);
                    g->addWidget(b, k / 3, k % 3);
                    if (k == 4) b->setChecked(true);
                }
                connect(group, &QButtonGroup::idClicked, this, [this, group](int id) {
                    for (QAbstractButton* b : group->buttons()) b->setText(group->id(b) == id ? QStringLiteral("●") : QString());
                    emit_changed_later();
                });
                w = box;
                break;
            }
        }
        w->setObjectName(QString::fromStdString(s.key));
        if (!s.tip.isEmpty()) w->setToolTip(s.tip);
        fields_[s.key] = w;
        form_->addRow(s.label, w);
    }
    v->addLayout(form_);
    extra_ = new QVBoxLayout;
    v->addLayout(extra_);
    status_ = new QLabel;
    status_->setWordWrap(true);
    status_->hide();
    v->addWidget(status_);
    auto* bottom = new QHBoxLayout;
    if (with_preview) {
        preview_ = new QCheckBox(tr("Preview"));
        preview_->setChecked(true);
        connect(preview_, &QCheckBox::toggled, this, [this] { emit changed(); });
        bottom->addWidget(preview_);
    }
    bottom->addStretch(1);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    bottom->addWidget(bb);
    v->addLayout(bottom);
}

QWidget* ParamDialog::field(const std::string& key) const {
    auto it = fields_.find(key);
    return it == fields_.end() ? nullptr : it->second;
}

bool ParamDialog::preview_enabled() const { return preview_ && preview_->isChecked(); }

void ParamDialog::set_status(const QString& text, bool warning) {
    status_->setText(text);
    status_->setStyleSheet(warning ? QStringLiteral("color:#e8a33d;") : QStringLiteral("color:#8d9199;"));
    status_->setVisible(!text.isEmpty());
    // Grow to fit the (word-wrapped) status line; some platforms do not enforce the minimum size.
    if (isVisible() && layout()) {
        layout()->activate();
        const QSize want = sizeHint();
        if (height() < want.height() || width() < want.width()) resize(std::max(width(), want.width()), std::max(height(), want.height()));
    }
}

void ParamDialog::add_note(const QString& text) {
    auto* l = new QLabel(text);
    l->setWordWrap(true);
    l->setStyleSheet(QStringLiteral("color:#8d9199;"));
    extra_->addWidget(l);
}

Json ParamDialog::values() const {
    Json j = Json::object();
    for (const ParamSpec& s : specs_) {
        QWidget* w = fields_.at(s.key);
        switch (s.kind) {
            case ParamSpec::Kind::Int: j[s.key] = static_cast<QSpinBox*>(w)->value(); break;
            case ParamSpec::Kind::Double: j[s.key] = static_cast<QDoubleSpinBox*>(w)->value(); break;
            case ParamSpec::Kind::Bool: j[s.key] = static_cast<QCheckBox*>(w)->isChecked(); break;
            case ParamSpec::Kind::Enum: j[s.key] = to_std(static_cast<QComboBox*>(w)->currentData().toString()); break;
            case ParamSpec::Kind::Color: j[s.key] = hex(static_cast<ColorButton*>(w)->color()); break;
            case ParamSpec::Kind::OptColor: {
                auto* on = w->findChild<QCheckBox*>(QStringLiteral("on"));
                auto* cb = static_cast<ColorButton*>(w->findChild<QToolButton*>(QStringLiteral("color")));
                j[s.key] = on->isChecked() ? Json(hex_opaque(cb->color())) : Json(nullptr);
                break;
            }
            case ParamSpec::Kind::Anchor: {
                auto* g = w->findChild<QButtonGroup*>(QStringLiteral("anchor"));
                QAbstractButton* b = g->checkedButton();
                j[s.key] = b ? to_std(b->property("anchor").toString()) : std::string("c");
                break;
            }
        }
    }
    return j;
}

// ---- Levels -------------------------------------------------------------------------------------------------

LevelsDialog::LevelsDialog(QWidget* parent) : ParamDialog(tr("New Levels Layer"), {}, true, parent) {
    channel_ = new QComboBox;
    channel_->addItems({tr("RGB"), tr("Red"), tr("Green"), tr("Blue")});
    form_->addRow(tr("Channel"), channel_);
    ib_ = new QSpinBox;
    ib_->setRange(0, 254);
    gamma_ = new QDoubleSpinBox;
    gamma_->setRange(0.1, 9.99);
    gamma_->setDecimals(2);
    gamma_->setSingleStep(0.05);
    iw_ = new QSpinBox;
    iw_->setRange(1, 255);
    ob_ = new QSpinBox;
    ob_->setRange(0, 255);
    ow_ = new QSpinBox;
    ow_->setRange(0, 255);
    form_->addRow(tr("Input black"), ib_);
    form_->addRow(tr("Gamma"), gamma_);
    form_->addRow(tr("Input white"), iw_);
    form_->addRow(tr("Output black"), ob_);
    form_->addRow(tr("Output white"), ow_);
    add_note(tr("Input black must stay below input white."));
    load_channel();
    connect(channel_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int i) {
        store_channel();
        cur_ = i;
        load_channel();
    });
    for (QSpinBox* sb : {ib_, iw_, ob_, ow_})
        connect(sb, qOverload<int>(&QSpinBox::valueChanged), this, [this] {
            if (!loading_) {
                store_channel();
                emit_changed_later();
            }
        });
    connect(gamma_, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this] {
        if (!loading_) {
            store_channel();
            emit_changed_later();
        }
    });
}

void LevelsDialog::load_channel() {
    loading_ = true;
    const Ch& c = ch_[cur_];
    ib_->setValue(c.in_black);
    iw_->setValue(c.in_white);
    gamma_->setValue(c.gamma);
    ob_->setValue(c.out_black);
    ow_->setValue(c.out_white);
    loading_ = false;
}

void LevelsDialog::store_channel() {
    Ch& c = ch_[cur_];
    c.in_black = ib_->value();
    c.in_white = iw_->value();
    c.gamma = gamma_->value();
    c.out_black = ob_->value();
    c.out_white = ow_->value();
}

Json LevelsDialog::params() const {
    Json p = Json::object();
    const char* keys[4] = {"rgb", "r", "g", "b"};
    for (int i = 0; i < 4; ++i) {
        const Ch& c = ch_[i];
        if (c.in_black == 0 && c.in_white == 255 && c.gamma == 1.0 && c.out_black == 0 && c.out_white == 255) continue;
        p[keys[i]] = {{"in_black", c.in_black}, {"in_white", c.in_white}, {"gamma", c.gamma},
                      {"out_black", c.out_black}, {"out_white", c.out_white}};
    }
    return p;
}

void LevelsDialog::set_params(const Json& p) {
    const char* keys[4] = {"rgb", "r", "g", "b"};
    for (int i = 0; i < 4; ++i) {
        const auto it = p.find(keys[i]);
        if (it == p.end() || !it->is_object()) continue;
        Ch& c = ch_[i];
        c.in_black = it->value("in_black", 0);
        c.in_white = it->value("in_white", 255);
        c.gamma = it->value("gamma", 1.0);
        c.out_black = it->value("out_black", 0);
        c.out_white = it->value("out_white", 255);
    }
    load_channel();
}

// ---- Curves -----------------------------------------------------------------------------------------------------

CurveEditor::CurveEditor(QWidget* parent) : QWidget(parent) {
    setMinimumSize(240, 240);
    setMouseTracking(true);
}

void CurveEditor::set_points(std::vector<std::pair<int, int>> p) {
    pts_ = std::move(p);
    update();
}

QRectF CurveEditor::area() const { return QRectF(8, 8, width() - 16, height() - 16); }

QPointF CurveEditor::to_w(double x, double y) const {
    const QRectF a = area();
    return QPointF(a.left() + x / 255.0 * a.width(), a.bottom() - y / 255.0 * a.height());
}

std::pair<int, int> CurveEditor::from_w(const QPointF& p) const {
    const QRectF a = area();
    const int x = std::clamp(static_cast<int>(std::lround((p.x() - a.left()) / a.width() * 255.0)), 0, 255);
    const int y = std::clamp(static_cast<int>(std::lround((a.bottom() - p.y()) / a.height() * 255.0)), 0, 255);
    return {x, y};
}

void CurveEditor::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF a = area();
    p.fillRect(rect(), theme::kBase);
    p.setPen(QColor(0x3c, 0x3f, 0x45));
    for (int i = 0; i <= 4; ++i) {
        p.drawLine(QPointF(a.left() + a.width() * i / 4, a.top()), QPointF(a.left() + a.width() * i / 4, a.bottom()));
        p.drawLine(QPointF(a.left(), a.top() + a.height() * i / 4), QPointF(a.right(), a.top() + a.height() * i / 4));
    }
    p.setPen(QPen(QColor(0x5a, 0x5e, 0x65), 1, Qt::DashLine));
    p.drawLine(to_w(0, 0), to_w(255, 255));
    QPainterPath path;
    if (lut_.size() == 256) {
        // The curve exactly as the core evaluates it (the adjustment's own LUT).
        path.moveTo(to_w(0, lut_[0]));
        for (int x = 1; x < 256; ++x) path.lineTo(to_w(x, lut_[static_cast<size_t>(x)]));
    } else {
        path.moveTo(to_w(0, pts_.front().second));
        for (const auto& [x, y] : pts_) path.lineTo(to_w(x, y));
        path.lineTo(to_w(255, pts_.back().second));
    }
    p.setPen(QPen(theme::kText, 1.6));
    p.drawPath(path);
    p.setBrush(theme::kWindow);
    p.setPen(QPen(theme::kText, 1.4));
    for (size_t i = 0; i < pts_.size(); ++i) {
        if (static_cast<int>(i) == drag_) p.setBrush(theme::kAccent);
        else p.setBrush(theme::kWindow);
        p.drawRect(QRectF(to_w(pts_[i].first, pts_[i].second) - QPointF(4, 4), QSizeF(8, 8)));
    }
}

void CurveEditor::mousePressEvent(QMouseEvent* e) {
    const QPointF w = e->position();
    drag_ = -1;
    for (size_t i = 0; i < pts_.size(); ++i)
        if (QLineF(to_w(pts_[i].first, pts_[i].second), w).length() < 8) drag_ = static_cast<int>(i);
    if (e->button() == Qt::RightButton) {
        if (drag_ >= 0 && pts_.size() > 2) {
            pts_.erase(pts_.begin() + drag_);
            drag_ = -1;
            update();
            emit edited();
        }
        return;
    }
    if (drag_ < 0 && pts_.size() < 16) {
        const auto np = from_w(w);
        for (const auto& q : pts_)
            if (q.first == np.first) return;
        pts_.push_back(np);
        std::sort(pts_.begin(), pts_.end());
        for (size_t i = 0; i < pts_.size(); ++i)
            if (pts_[i] == np) drag_ = static_cast<int>(i);
        update();
        emit edited();
    }
}

void CurveEditor::mouseMoveEvent(QMouseEvent* e) {
    if (drag_ < 0 || !(e->buttons() & Qt::LeftButton)) return;
    auto np = from_w(e->position());
    // Keep x strictly increasing (doc 20 A2).
    const int lo = drag_ > 0 ? pts_[static_cast<size_t>(drag_) - 1].first + 1 : 0;
    const int hi = drag_ + 1 < static_cast<int>(pts_.size()) ? pts_[static_cast<size_t>(drag_) + 1].first - 1 : 255;
    np.first = std::clamp(np.first, lo, hi);
    pts_[static_cast<size_t>(drag_)] = np;
    update();
    emit edited();
}

void CurveEditor::mouseReleaseEvent(QMouseEvent*) {
    drag_ = -1;
    update();
}

CurvesDialog::CurvesDialog(QWidget* parent) : ParamDialog(tr("New Curves Layer"), {}, true, parent) {
    for (auto& c : ch_) c = {{0, 0}, {255, 255}};
    channel_ = new QComboBox;
    channel_->addItems({tr("RGB"), tr("Red"), tr("Green"), tr("Blue")});
    form_->addRow(tr("Channel"), channel_);
    editor_ = new CurveEditor;
    extra_->addWidget(editor_, 1);
    add_note(tr("Click to add a point (up to 16), drag to move, right-click to remove."));
    connect(channel_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int i) {
        ch_[cur_] = editor_->points();
        cur_ = i;
        editor_->set_points(ch_[cur_]);
        refresh_lut();
    });
    connect(editor_, &CurveEditor::edited, this, [this] {
        ch_[cur_] = editor_->points();
        refresh_lut();
        emit_changed_later();
    });
    refresh_lut();
}

void CurvesDialog::refresh_lut() {
    // Ask the core for the exact curve (when the curves adjustment is available in this build).
    Json p = Json::object();
    Json pts = Json::array();
    for (const auto& [x, y] : ch_[cur_]) pts.push_back(Json::array({x, y}));
    const char* keys[4] = {"rgb", "r", "g", "b"};
    p[keys[cur_]] = pts;
    std::vector<int> lut;
    try {
        auto adj = rl::adjust::make_adjustment("curves", &p, "curves preview");
        if (auto* l = dynamic_cast<const rl::adjust::LutAdjustment*>(adj.get())) {
            const auto& t = cur_ == 2 ? l->lut_g() : cur_ == 3 ? l->lut_b() : l->lut_r();
            lut.assign(t.begin(), t.end());
        }
    } catch (const std::exception&) {
        // Not available in this build: the editor shows the control polygon instead.
    }
    editor_->set_preview_lut(lut);
}

void CurvesDialog::set_params(const Json& p) {
    const char* keys[4] = {"rgb", "r", "g", "b"};
    for (int i = 0; i < 4; ++i) {
        const auto it = p.find(keys[i]);
        if (it == p.end() || !it->is_array() || it->size() < 2) continue;
        std::vector<std::pair<int, int>> pts;
        for (const Json& q : *it)
            if (q.is_array() && q.size() == 2)
                pts.emplace_back(static_cast<int>(std::lround(q[0].get<double>())), static_cast<int>(std::lround(q[1].get<double>())));
        if (pts.size() >= 2) ch_[i] = pts;
    }
    editor_->set_points(ch_[cur_]);
    refresh_lut();
}

Json CurvesDialog::params() const {
    Json p = Json::object();
    const char* keys[4] = {"rgb", "r", "g", "b"};
    std::vector<std::pair<int, int>> cur[4] = {ch_[0], ch_[1], ch_[2], ch_[3]};
    cur[cur_] = editor_->points();
    for (int i = 0; i < 4; ++i) {
        if (cur[i] == std::vector<std::pair<int, int>>{{0, 0}, {255, 255}}) continue;
        Json pts = Json::array();
        for (const auto& [x, y] : cur[i]) pts.push_back(Json::array({x, y}));
        p[keys[i]] = pts;
    }
    return p;
}

}  // namespace rl::gui
