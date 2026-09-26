// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tool Options bar: the current tool's parameters (only fields of the ops in docs/math).
#pragma once

#include <QToolBar>

class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QSpinBox;

namespace rl::gui {

class CanvasController;
class EditorSession;
class ToolState;

class ToolOptionsBar : public QToolBar {
    Q_OBJECT
public:
    ToolOptionsBar(ToolState* t, CanvasController* c, EditorSession* s, QWidget* parent = nullptr);

public slots:
    void rebuild();

private:
    QLabel* add_label(const QString& text);
    QDoubleSpinBox* add_double(const QString& label, double min, double max, double value, int decimals,
                                     const QString& suffix, const QString& tip);
    QSpinBox* add_int(const QString& label, int min, int max, int value, const QString& suffix, const QString& tip);
    QCheckBox* add_check(const QString& text, bool value, const QString& tip);
    void add_sel_modes();
    void add_brush(bool clone);

    ToolState* t_;
    CanvasController* c_;
    EditorSession* s_;
};

}  // namespace rl::gui
