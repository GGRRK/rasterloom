// SPDX-License-Identifier: GPL-3.0-or-later
//
// Parameter dialogs. Each dialog produces the params of exactly one op in docs/math; ranges and
// defaults are the docs' tables, so what the user can enter is what the op accepts.
#pragma once

#include <QColor>
#include <QDialog>
#include <QStringList>
#include <QTimer>

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "gui/op_runner.hpp"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QSpinBox;
class QFormLayout;
class QLabel;
class QVBoxLayout;

namespace rl::gui {

struct ParamSpec {
    enum class Kind { Int, Double, Bool, Enum, Color, OptColor, Anchor };
    Kind kind = Kind::Double;
    std::string key;
    QString label;
    double min = 0, max = 1, def = 0;
    int decimals = 1;
    QString suffix;
    QStringList enum_labels;
    QStringList enum_values;
    QString enum_def;
    QColor color_def = Qt::black;
    bool opt_on = false;
    QString tip;

    static ParamSpec i(std::string key, QString label, int min, int max, int def, QString suffix = {});
    static ParamSpec d(std::string key, QString label, double min, double max, double def, int decimals = 1, QString suffix = {});
    static ParamSpec b(std::string key, QString label, bool def);
    static ParamSpec e(std::string key, QString label, QStringList labels, QStringList values, QString def);
    static ParamSpec c(std::string key, QString label, QColor def);
    static ParamSpec oc(std::string key, QString label, QColor def, bool on);
    static ParamSpec anchor(std::string key, QString label);
};

// A form built from ParamSpecs, with an optional live-preview checkbox and a status line.
class ParamDialog : public QDialog {
    Q_OBJECT
public:
    ParamDialog(const QString& title, const std::vector<ParamSpec>& specs, bool with_preview, QWidget* parent = nullptr);

    Json values() const;           // {key: value} for every spec
    QWidget* field(const std::string& key) const;
    bool preview_enabled() const;
    void set_status(const QString& text, bool warning);
    void add_note(const QString& text);
    QVBoxLayout* extra_area() const { return extra_; }

signals:
    void changed();  // debounced (120 ms) after any edit

protected:
    void emit_changed_later() { debounce_.start(); }
    QFormLayout* form_ = nullptr;
    QVBoxLayout* extra_ = nullptr;

private:
    std::vector<ParamSpec> specs_;
    std::map<std::string, QWidget*> fields_;
    QCheckBox* preview_ = nullptr;
    QLabel* status_ = nullptr;
    QTimer debounce_;
};

// Levels (doc 20 A1): per-channel in/gamma/out.
class LevelsDialog : public ParamDialog {
    Q_OBJECT
public:
    explicit LevelsDialog(QWidget* parent = nullptr);
    Json params() const;
    void set_params(const Json& p);  // doc 20 levels params (params_to_json form)

private:
    void load_channel();
    void store_channel();
    struct Ch {
        int in_black = 0, in_white = 255, out_black = 0, out_white = 255;
        double gamma = 1.0;
    };
    Ch ch_[4];
    int cur_ = 0;
    QComboBox* channel_;
    QSpinBox *ib_, *iw_, *ob_, *ow_;
    QDoubleSpinBox* gamma_;
    bool loading_ = false;
};

// Curves (doc 20 A2): up to 16 integer control points per channel.
class CurveEditor : public QWidget {
    Q_OBJECT
public:
    explicit CurveEditor(QWidget* parent = nullptr);
    std::vector<std::pair<int, int>> points() const { return pts_; }
    void set_points(std::vector<std::pair<int, int>> p);
    void set_preview_lut(const std::vector<int>& lut) {
        lut_ = lut;
        update();
    }
    QSize sizeHint() const override { return {280, 280}; }

signals:
    void edited();

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;

private:
    QRectF area() const;
    QPointF to_w(double x, double y) const;
    std::pair<int, int> from_w(const QPointF& p) const;
    std::vector<std::pair<int, int>> pts_{{0, 0}, {255, 255}};
    std::vector<int> lut_;
    int drag_ = -1;
};

class CurvesDialog : public ParamDialog {
    Q_OBJECT
public:
    explicit CurvesDialog(QWidget* parent = nullptr);
    Json params() const;
    void set_params(const Json& p);  // doc 20 curves params (params_to_json form)

private:
    void refresh_lut();
    std::vector<std::pair<int, int>> ch_[4];
    int cur_ = 0;
    QComboBox* channel_;
    CurveEditor* editor_;
};

}  // namespace rl::gui
