// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/tools.hpp"

namespace rl::gui {

const std::vector<ToolInfo>& tool_list() {
    static const std::vector<ToolInfo> list = {
        {Tool::Move, "move", "move", "Move", "V"},
        {Tool::MarqueeRect, "marquee_rect", "marquee_rect", "Rectangular Marquee", "M"},
        {Tool::MarqueeEllipse, "marquee_ellipse", "marquee_ellipse", "Elliptical Marquee", "M"},
        {Tool::Lasso, "lasso", "lasso", "Lasso", "L"},
        {Tool::PolygonLasso, "polygon_lasso", "polygon_lasso", "Polygonal Lasso", "L"},
        {Tool::Wand, "wand", "wand", "Magic Wand", "W"},
        {Tool::Crop, "crop", "crop", "Crop", "C"},
        {Tool::Eyedropper, "eyedropper", "eyedropper", "Eyedropper", "I"},
        {Tool::Brush, "brush", "brush", "Brush", "B"},
        {Tool::Eraser, "eraser", "eraser", "Eraser", "E"},
        {Tool::Clone, "clone", "clone", "Clone Stamp", "S"},
        {Tool::Gradient, "gradient", "gradient", "Gradient", "G"},
        {Tool::Bucket, "bucket", "bucket", "Paint Bucket", "G"},
        {Tool::Hand, "hand", "hand", "Hand", "H"},
        {Tool::Zoom, "zoom", "zoom", "Zoom", "Z"},
    };
    return list;
}

const ToolInfo& tool_info(Tool t) {
    static const ToolInfo transform{Tool::Transform, "transform", "transform", "Free Transform", ""};
    if (t == Tool::Transform) return transform;
    for (const ToolInfo& i : tool_list())
        if (i.tool == t) return i;
    return tool_list().front();
}

const char* sel_mode_json(SelMode m) {
    switch (m) {
        case SelMode::New: return "new";
        case SelMode::Add: return "add";
        case SelMode::Subtract: return "subtract";
        case SelMode::Intersect: return "intersect";
    }
    return "new";
}

}  // namespace rl::gui
