// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/geometry/gui_api.hpp"

#include <cmath>

#include "core/base/error.hpp"
#include "core/script/engine.hpp"

namespace rl::geom::gui {

void run_op(Document& doc, const Json& op) {
    if (!op.is_object()) throw ScriptError("op must be a JSON object");
    auto it = op.find("op");
    if (it == op.end() || !it->is_string()) throw ScriptError("op: missing string field 'op'");
    const std::string name = it->get<std::string>();
    const script::OpSpec* spec = script::default_registry().find(name);
    if (!spec) throw ScriptError("unknown op '" + name + "'");
    script::OpContext ctx{doc, 0, name};
    script::Fields f(op, ctx.label);
    f.consume("op");
    if (!spec->records_history) {
        spec->run(ctx, f);
        f.finish();
        return;
    }
    doc.push_history();
    try {
        spec->run(ctx, f);
        f.finish();
    } catch (const ScriptError&) {
        doc.undo(1);  // handlers validate before changing anything: this drops the record only
        throw;
    }
}

select::Mode mode_from_modifiers(bool shift, bool alt) {
    if (shift && alt) return select::Mode::Intersect;
    if (shift) return select::Mode::Add;
    if (alt) return select::Mode::Subtract;
    return select::Mode::New;
}

const char* mode_name(select::Mode m) {
    switch (m) {
        case select::Mode::Add: return "add";
        case select::Mode::Subtract: return "subtract";
        case select::Mode::Intersect: return "intersect";
        case select::Mode::New: break;
    }
    return "new";
}

std::optional<DragRect> marquee_from_drag(double px, double py, double cx, double cy, bool square, bool from_center) {
    double dx = cx - px, dy = cy - py;
    if (square) {
        const double s = std::max(std::abs(dx), std::abs(dy));
        dx = (dx < 0.0) ? -s : s;
        dy = (dy < 0.0) ? -s : s;
    }
    double x0 = px, y0 = py, x1 = px + dx, y1 = py + dy;
    if (from_center) {
        x0 = px - dx;
        y0 = py - dy;
    }
    // Snap outward to whole pixels.
    const double l = std::floor(std::min(x0, x1)), r = std::ceil(std::max(x0, x1));
    const double t = std::floor(std::min(y0, y1)), b = std::ceil(std::max(y0, y1));
    if (!(r - l > 0.0) || !(b - t > 0.0) || std::abs(dx) < 1e-9 || std::abs(dy) < 1e-9) return std::nullopt;
    const double w = std::min(r - l, 65536.0), h = std::min(b - t, 65536.0);
    return DragRect{l, t, w, h};
}

namespace {
Json shape_op(const char* name, const DragRect& r, select::Mode mode, bool aa) {
    return Json{{"op", name}, {"x", r.x}, {"y", r.y}, {"w", r.w}, {"h", r.h}, {"antialias", aa}, {"mode", mode_name(mode)}};
}
}  // namespace

Json select_rect_op(const DragRect& r, select::Mode mode, bool antialias) { return shape_op("select_rect", r, mode, antialias); }
Json select_ellipse_op(const DragRect& r, select::Mode mode, bool antialias) {
    return shape_op("select_ellipse", r, mode, antialias);
}

void LassoBuilder::add(double x, double y) {
    if (!std::isfinite(x) || !std::isfinite(y)) return;
    if (!pts_.empty() && pts_.back().x == x && pts_.back().y == y) return;
    pts_.push_back(select::Pt{x, y});
    while (pts_.size() > 4096) {
        std::vector<select::Pt> kept;
        kept.reserve((pts_.size() / 2) + 2);
        for (size_t i = 0; i < pts_.size(); ++i)
            if (i == 0 || i + 1 == pts_.size() || (i % 2) == 0) kept.push_back(pts_[i]);
        pts_.swap(kept);
    }
}

Json LassoBuilder::op(select::Mode mode, bool antialias) const {
    Json pts = Json::array();
    for (const select::Pt& p : pts_) pts.push_back(Json::array({p.x, p.y}));
    return Json{{"op", "select_polygon"}, {"points", pts}, {"antialias", antialias}, {"mode", mode_name(mode)}};
}

Json select_wand_op(const std::string& layer, double x, double y, int tolerance, bool contiguous, bool antialias,
                    select::Mode mode) {
    return Json{{"op", "select_wand"},
                {"layer", layer},
                {"x", static_cast<int64_t>(std::floor(x))},
                {"y", static_cast<int64_t>(std::floor(y))},
                {"tolerance", tolerance},
                {"contiguous", contiguous},
                {"antialias", antialias},
                {"mode", mode_name(mode)}};
}

Json select_simple_op(const char* name) { return Json{{"op", name}}; }
Json feather_op(double radius) { return Json{{"op", "feather"}, {"radius", radius}}; }
Json expand_op(int by) { return Json{{"op", "expand"}, {"by", by}}; }
Json contract_op(int by) { return Json{{"op", "contract"}, {"by", by}}; }

Json fill_selection_op(const std::string& layer, const std::string& color_hex, double opacity) {
    return Json{{"op", "fill_selection"}, {"layer", layer}, {"color", color_hex}, {"opacity", opacity}};
}

Json gradient_op(const std::string& layer, bool radial, double x0, double y0, double x1, double y1,
                 const std::string& c0, const std::string& c1, double opacity, bool reverse) {
    return Json{{"op", "gradient"},
                {"layer", layer},
                {"type", radial ? "radial" : "linear"},
                {"p0", Json::array({x0, y0})},
                {"p1", Json::array({x1, y1})},
                {"c0", c0},
                {"c1", c1},
                {"opacity", opacity},
                {"reverse", reverse}};
}

Json bucket_fill_op(const std::string& layer, double x, double y, const std::string& color, int tolerance,
                    bool contiguous, bool antialias, double opacity) {
    return Json{{"op", "bucket_fill"},
                {"layer", layer},
                {"x", static_cast<int64_t>(std::floor(x))},
                {"y", static_cast<int64_t>(std::floor(y))},
                {"color", color},
                {"tolerance", tolerance},
                {"contiguous", contiguous},
                {"antialias", antialias},
                {"opacity", opacity}};
}

Json transform_matrix_op(const std::string& layer, const transform::Mat3& m, transform::Interp interp) {
    Json mat = Json::array();
    for (double v : m) mat.push_back(v);
    return Json{{"op", "transform"},
                {"layer", layer},
                {"interp", interp == transform::Interp::Nearest ? "nearest" : "bicubic"},
                {"matrix", mat}};
}

std::optional<select::Pt> map_point(const transform::Mat3& m, double x, double y) {
    const double X = ((m[0] * x) + (m[1] * y)) + m[2];
    const double Y = ((m[3] * x) + (m[4] * y)) + m[5];
    const double W = ((m[6] * x) + (m[7] * y)) + m[8];
    if (!(W > 0.0)) return std::nullopt;
    return select::Pt{X / W, Y / W};
}

Json image_size_op(int w, int h, transform::Interp interp) {
    return Json{{"op", "image_size"}, {"w", w}, {"h", h}, {"interp", interp == transform::Interp::Nearest ? "nearest" : "bicubic"}};
}
Json canvas_size_op(int w, int h, const std::string& anchor) {
    return Json{{"op", "canvas_size"}, {"w", w}, {"h", h}, {"anchor", anchor}};
}
Json crop_op(int64_t x, int64_t y, int w, int h) { return Json{{"op", "crop"}, {"x", x}, {"y", y}, {"w", w}, {"h", h}}; }
Json rotate_canvas_op(double angle_deg) { return Json{{"op", "rotate_canvas"}, {"angle", angle_deg}}; }
Json flip_op(bool horizontal, const std::string& layer) {
    Json j{{"op", "flip"}, {"axis", horizontal ? "h" : "v"}};
    if (!layer.empty()) j["layer"] = layer;
    return j;
}

}  // namespace rl::geom::gui
