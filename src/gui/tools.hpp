// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tool identities and the per-tool settings the Tool Options bar edits. Only parameters of the
// ops in docs/math appear here; the canvas turns a gesture plus these settings into one op.
#pragma once

#include <QColor>
#include <QKeySequence>
#include <QObject>
#include <QString>

#include <vector>

namespace rl::gui {

enum class Tool {
    Move,
    MarqueeRect,
    MarqueeEllipse,
    Lasso,
    PolygonLasso,
    Wand,
    Crop,
    Eyedropper,
    Brush,
    Eraser,
    Clone,
    Gradient,
    Bucket,
    Hand,
    Zoom,
    Transform,  // Free Transform (Edit menu, Ctrl+T); not on the toolbar
};

struct ToolInfo {
    Tool tool;
    const char* id;      // stable key ("brush")
    const char* icon;    // icons.hpp glyph
    const char* name;    // display name
    const char* key;     // single-key shortcut ("B"), shared keys cycle within their group
};

// Toolbar order (Transform excluded).
const std::vector<ToolInfo>& tool_list();
const ToolInfo& tool_info(Tool t);

// Selection combine mode (doc 30 §4).
enum class SelMode { New, Add, Subtract, Intersect };
const char* sel_mode_json(SelMode m);

struct BrushSettings {
    double size = 24.0;        // doc 40 [1, 5000]
    double hardness = 0.8;     // [0, 1]
    double spacing = 0.25;     // [0.01, 10]
    double opacity = 1.0;      // [0, 1]
    double flow = 1.0;         // [0, 1]
    double angle = 0.0;        // [-360, 360]
    double roundness = 1.0;    // [0.01, 1]
    bool buildup = false;      // mode "buildup" (else "wash")
    double dabs_per_second = 0.0;
    double smoothing = 0.0;    // [0, 0.99]
    bool pressure_size = true;
    bool pressure_opacity = false;
};

struct ToolSettings {
    BrushSettings brush;
    BrushSettings eraser{24.0, 0.8, 0.25, 1.0, 1.0, 0.0, 1.0, false, 0.0, 0.0, true, false};
    BrushSettings clone{32.0, 0.6, 0.25, 1.0, 1.0, 0.0, 1.0, false, 0.0, 0.0, true, false};
    bool clone_aligned = true;

    SelMode sel_mode = SelMode::New;
    bool marquee_antialias = false;   // select_rect default false; ellipse default true (below)
    bool ellipse_antialias = true;
    bool lasso_antialias = true;

    int wand_tolerance = 32;
    bool wand_contiguous = true;
    bool wand_antialias = true;

    int bucket_tolerance = 32;
    bool bucket_contiguous = true;
    bool bucket_antialias = true;
    double bucket_opacity = 1.0;

    bool gradient_radial = false;
    bool gradient_reverse = false;
    double gradient_opacity = 1.0;

    bool transform_bicubic = true;
};

// The tool, settings and colours shared by the canvas, the options bar and the colour panel.
class ToolState : public QObject {
    Q_OBJECT
public:
    explicit ToolState(QObject* parent = nullptr) : QObject(parent) {}

    Tool tool() const { return tool_; }
    void set_tool(Tool t) {
        if (t == tool_) return;
        prev_ = tool_;
        tool_ = t;
        emit tool_changed(t);
    }
    Tool previous_tool() const { return prev_; }

    ToolSettings& settings() { return s_; }
    const ToolSettings& settings() const { return s_; }
    void notify_settings() { emit settings_changed(); }

    QColor fg() const { return fg_; }
    QColor bg() const { return bg_; }
    void set_fg(const QColor& c) {
        if (c == fg_) return;
        fg_ = c;
        emit colors_changed();
    }
    void set_bg(const QColor& c) {
        if (c == bg_) return;
        bg_ = c;
        emit colors_changed();
    }
    void swap_colors() {
        std::swap(fg_, bg_);
        emit colors_changed();
    }
    void reset_colors() {
        fg_ = Qt::black;
        bg_ = Qt::white;
        emit colors_changed();
    }

    // The brush-like settings of the current tool (brush / eraser / clone), or nullptr.
    BrushSettings* brush_settings_for(Tool t) {
        if (t == Tool::Brush) return &s_.brush;
        if (t == Tool::Eraser) return &s_.eraser;
        if (t == Tool::Clone) return &s_.clone;
        return nullptr;
    }

signals:
    void tool_changed(rl::gui::Tool t);
    void settings_changed();
    void colors_changed();

private:
    Tool tool_ = Tool::Brush;
    Tool prev_ = Tool::Move;
    ToolSettings s_;
    QColor fg_ = QColor(0x1e, 0x3a, 0x8a);
    QColor bg_ = Qt::white;
};

}  // namespace rl::gui
