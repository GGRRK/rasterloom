// SPDX-License-Identifier: GPL-3.0-or-later
//
// GUI-facing helpers for the doc 30 tools (no Qt): turn pointer input into render-script ops and
// apply an op to a live document with exactly one history record. Going through the op path keeps
// the GUI byte-identical to scripts (and to the goldens) and gives undo for free (C10).
//
//   Marquee:  auto r = marquee_from_drag(press, current, shift_square, alt_center);
//             if (r) run_op(doc, select_rect_op(*r, mode_from_modifiers(shift, alt)));
//   Lasso:    LassoBuilder b; b.add(x, y) per pointer move; on release run_op(doc, b.op(mode)).
//   Wand:     run_op(doc, select_wand_op("L", px, py, 32, true, true, mode));
//   Ants:     rl::select::outline_polylines(doc.selection().mask) (select/selection_ops.hpp),
//             or rl::select::edge_mask(...) for a 1-bit edge plane.
//   Free transform preview: M = transform::from_params(p) / from_quad(q);
//             auto img = transform::transform_image(layer.pixels, M, interp);  // no history
//             commit with run_op(doc, transform_matrix_op("L", M, interp)).
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/doc/document.hpp"
#include "core/geometry/canvas_ops.hpp"
#include "core/script/fields.hpp"
#include "core/select/selection_ops.hpp"
#include "core/transform/transform.hpp"

namespace rl::geom::gui {

using Json = script::Json;

// Applies one render-script op object ({"op": ..., ...}) to `doc` through the default registry,
// pushing exactly one history record (none for "undo"). On a ScriptError the document and its
// history are left as they were, and the error is rethrown.
void run_op(Document& doc, const Json& op);

// Photoshop-style modifiers: Shift = add, Alt = subtract, Shift+Alt = intersect, none = new.
select::Mode mode_from_modifiers(bool shift, bool alt);
const char* mode_name(select::Mode m);

// A marquee rectangle in canvas pixels.
struct DragRect {
    double x = 0.0, y = 0.0, w = 0.0, h = 0.0;
};

// Rectangle/ellipse marquee from a drag (canvas coordinates of press and current pointer). The
// corners snap to whole pixels (floor of the press, ceil/floor of the pointer, see parity note 2 of
// doc 30 §22). `square` constrains to the larger side; `from_center` makes the press point the
// centre. nullopt while the drag is still empty (zero width or height).
std::optional<DragRect> marquee_from_drag(double px, double py, double cx, double cy, bool square,
                                          bool from_center);

Json select_rect_op(const DragRect& r, select::Mode mode, bool antialias = false);
Json select_ellipse_op(const DragRect& r, select::Mode mode, bool antialias = true);

// Freehand / polygonal lasso: collects pointer positions (canvas doubles). Consecutive duplicates
// are dropped. When more than 4096 points arrive (the op's limit), every second interior point is
// dropped, deterministically, until the path fits.
class LassoBuilder {
public:
    void add(double x, double y);
    void clear() { pts_.clear(); }
    const std::vector<select::Pt>& points() const { return pts_; }
    bool valid() const { return pts_.size() >= 3; }  // what select_polygon accepts
    Json op(select::Mode mode, bool antialias = true) const;

private:
    std::vector<select::Pt> pts_;
};

// Magic wand at the pointer (canvas doubles floored to the pixel under it).
Json select_wand_op(const std::string& layer, double x, double y, int tolerance, bool contiguous, bool antialias,
                    select::Mode mode);

Json select_simple_op(const char* name);  // select_all / deselect / reselect / select_inverse
Json feather_op(double radius);
Json expand_op(int by);
Json contract_op(int by);
Json fill_selection_op(const std::string& layer, const std::string& color_hex, double opacity = 1.0);
Json gradient_op(const std::string& layer, bool radial, double x0, double y0, double x1, double y1,
                 const std::string& c0, const std::string& c1, double opacity = 1.0, bool reverse = false);
Json bucket_fill_op(const std::string& layer, double x, double y, const std::string& color, int tolerance = 32,
                    bool contiguous = true, bool antialias = true, double opacity = 1.0);

// Free transform commit (matrix form: the GUI composes the matrix from its handles).
Json transform_matrix_op(const std::string& layer, const transform::Mat3& m,
                         transform::Interp interp = transform::Interp::Bicubic);
// Forward-maps a point through `m` (for drawing the transform box and handles). nullopt when the
// point maps to or behind the horizon (w <= 0).
std::optional<select::Pt> map_point(const transform::Mat3& m, double x, double y);

Json image_size_op(int w, int h, transform::Interp interp = transform::Interp::Bicubic);
Json canvas_size_op(int w, int h, const std::string& anchor = "c");
Json crop_op(int64_t x, int64_t y, int w, int h);
Json rotate_canvas_op(double angle_deg);
Json flip_op(bool horizontal, const std::string& layer = "");  // empty layer: flip the canvas

}  // namespace rl::geom::gui
