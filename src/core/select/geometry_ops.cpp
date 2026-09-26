// SPDX-License-Identifier: GPL-3.0-or-later
//
// Render-script ops of docs/math/30-geometry-selection.md §19: selections (src/core/select/),
// transform (src/core/transform/), canvas geometry (src/core/geometry/), gradient / bucket /
// fill_selection (src/core/paint/). Every handler reads and validates all of its fields and calls
// f.finish() before it changes the document, so an invalid script never half-applies an op.
#include <array>
#include <cmath>
#include <limits>
#include <vector>

#include "core/base/error.hpp"
#include "core/geometry/canvas_ops.hpp"
#include "core/geometry/dense.hpp"
#include "core/paint/paint.hpp"
#include "core/script/domains.hpp"
#include "core/select/selection_ops.hpp"
#include "core/transform/transform.hpp"

namespace rl::script {

namespace {

using select::Mode;
using select::Pt;
using transform::Interp;
using transform::Mat3;

constexpr double kAnyLo = -std::numeric_limits<double>::max();
constexpr double kAnyHi = std::numeric_limits<double>::max();
constexpr int64_t kIntLo = std::numeric_limits<int64_t>::min();
constexpr int64_t kIntHi = std::numeric_limits<int64_t>::max();

std::string key_what(const Fields& f, const char* key) { return f.context() + ": field '" + key + "'"; }

Mode read_mode(Fields& f) {
    const std::string s = f.enum_or("mode", "new", {"new", "add", "subtract", "intersect"});
    Mode m = Mode::New;
    select::parse_mode(s, m);
    return m;
}

// [num, num] (any finite values unless `lo`/`hi` say otherwise).
std::array<double, 2> read_pair(const Json& v, const std::string& what, double lo = kAnyLo, double hi = kAnyHi) {
    if (!v.is_array() || v.size() != 2) throw ScriptError(what + " must be an array [num, num]");
    return {Fields::as_double(v[0], what + "[0]", lo, hi), Fields::as_double(v[1], what + "[1]", lo, hi)};
}

std::array<double, 2> pair_req(Fields& f, const char* key) { return read_pair(f.raw_req(key), key_what(f, key)); }

std::array<double, 2> pair_or(Fields& f, const char* key, std::array<double, 2> def, double lo = kAnyLo,
                              double hi = kAnyHi) {
    const Json* v = f.raw(key);
    return v ? read_pair(*v, key_what(f, key), lo, hi) : def;
}

double positive_extent(Fields& f, const char* key) {
    const double v = f.req_double(key, 0.0, 65536.0);
    if (!(v > 0.0)) f.fail(std::string("field '") + key + "' must be > 0");
    return v;
}

Interp read_interp(Fields& f) {
    return f.enum_or("interp", "bicubic", {"bicubic", "nearest"}) == "nearest" ? Interp::Nearest : Interp::Bicubic;
}

double read_opacity(Fields& f) { return f.double_or("opacity", 1.0, 0.0, 1.0); }

// ---- selection shapes --------------------------------------------------------------------------------
void op_select_rect(OpContext& ctx, Fields& f) {
    const double x = f.req_double("x", kAnyLo, kAnyHi);
    const double y = f.req_double("y", kAnyLo, kAnyHi);
    const double w = positive_extent(f, "w");
    const double h = positive_extent(f, "h");
    const bool aa = f.bool_or("antialias", false);
    const Mode mode = read_mode(f);
    f.finish();
    Document& d = ctx.doc;
    select::combine(d.selection(), select::rasterize_rect(d.width(), d.height(), x, y, w, h, aa), mode);
}

void op_select_ellipse(OpContext& ctx, Fields& f) {
    const double x = f.req_double("x", kAnyLo, kAnyHi);
    const double y = f.req_double("y", kAnyLo, kAnyHi);
    const double w = positive_extent(f, "w");
    const double h = positive_extent(f, "h");
    const bool aa = f.bool_or("antialias", true);
    const Mode mode = read_mode(f);
    f.finish();
    Document& d = ctx.doc;
    select::combine(d.selection(), select::rasterize_ellipse(d.width(), d.height(), x, y, w, h, aa), mode);
}

void op_select_polygon(OpContext& ctx, Fields& f) {
    const Json& pts = f.raw_req("points");
    const std::string what = key_what(f, "points");
    if (!pts.is_array() || pts.size() < 3 || pts.size() > 4096)
        throw ScriptError(what + " must be an array of 3..4096 [x, y] points");
    std::vector<Pt> P;
    P.reserve(pts.size());
    for (size_t i = 0; i < pts.size(); ++i) {
        const auto p = read_pair(pts[i], what + "[" + std::to_string(i) + "]");
        P.push_back(Pt{p[0], p[1]});
    }
    const bool aa = f.bool_or("antialias", true);
    const Mode mode = read_mode(f);
    f.finish();
    Document& d = ctx.doc;
    select::combine(d.selection(), select::rasterize_polygon(d.width(), d.height(), P, aa), mode);
}

void op_select_wand(OpContext& ctx, Fields& f) {
    const std::string layer = f.req_string("layer");
    const int64_t x = f.req_int("x", kIntLo, kIntHi);
    const int64_t y = f.req_int("y", kIntLo, kIntHi);
    const int tol = static_cast<int>(f.int_or("tolerance", 32, 0, 255));
    const bool contiguous = f.bool_or("contiguous", true);
    const bool aa = f.bool_or("antialias", true);
    const Mode mode = read_mode(f);
    f.finish();
    Document& d = ctx.doc;
    const Node& n = d.require_raster(layer, ctx.label);
    const uint64_t copy_bytes = geom::dense_bytes(n.pixels.width(), n.pixels.height(), sizeof(Rgba8));
    const mem::Reservation hold = geom::reserve_dense(copy_bytes, "select_wand layer copy");
    const std::vector<Rgba8> px = geom::to_dense(n.pixels);
    select::combine(d.selection(), select::region(px, d.width(), d.height(), x, y, tol, contiguous, aa), mode);
}

void op_select_all(OpContext& ctx, Fields& f) {
    f.finish();
    select::select_all(ctx.doc.selection());
}
void op_deselect(OpContext& ctx, Fields& f) {
    f.finish();
    select::deselect(ctx.doc.selection());
}
void op_reselect(OpContext& ctx, Fields& f) {
    f.finish();
    select::reselect(ctx.doc.selection());
}
void op_select_inverse(OpContext& ctx, Fields& f) {
    f.finish();
    select::select_inverse(ctx.doc.selection());
}

void op_feather(OpContext& ctx, Fields& f) {
    const double r = f.req_double("radius", 0.0, 250.0);
    f.finish();
    select::feather(ctx.doc.selection(), r);
}
void op_expand(OpContext& ctx, Fields& f) {
    const int n = static_cast<int>(f.req_int("by", 1, 100));
    f.finish();
    select::expand(ctx.doc.selection(), n);
}
void op_contract(OpContext& ctx, Fields& f) {
    const int n = static_cast<int>(f.req_int("by", 1, 100));
    f.finish();
    select::contract(ctx.doc.selection(), n);
}

// ---- painting ------------------------------------------------------------------------------------------
void op_fill_selection(OpContext& ctx, Fields& f) {
    const std::string layer = f.req_string("layer");
    const Rgba8 color = f.req_color("color");
    const double opacity = read_opacity(f);
    f.finish();
    Document& d = ctx.doc;
    paint::fill_selection(d.require_raster(layer, ctx.label), d.selection(), color, opacity);
}

void op_gradient(OpContext& ctx, Fields& f) {
    const std::string layer = f.req_string("layer");
    paint::GradientParams p;
    p.type = f.enum_or("type", "linear", {"linear", "radial"}) == "radial" ? paint::GradientType::Radial
                                                                            : paint::GradientType::Linear;
    const auto p0 = pair_req(f, "p0");
    const auto p1 = pair_req(f, "p1");
    p.x0 = p0[0];
    p.y0 = p0[1];
    p.x1 = p1[0];
    p.y1 = p1[1];
    p.c0 = f.req_color("c0");
    p.c1 = f.req_color("c1");
    p.opacity = read_opacity(f);
    p.reverse = f.bool_or("reverse", false);
    f.finish();
    Document& d = ctx.doc;
    paint::gradient(d.require_raster(layer, ctx.label), d.selection(), p);
}

void op_bucket_fill(OpContext& ctx, Fields& f) {
    const std::string layer = f.req_string("layer");
    paint::BucketParams p;
    p.x = f.req_int("x", kIntLo, kIntHi);
    p.y = f.req_int("y", kIntLo, kIntHi);
    p.color = f.req_color("color");
    p.opacity = read_opacity(f);
    p.tolerance = static_cast<int>(f.int_or("tolerance", 32, 0, 255));
    p.contiguous = f.bool_or("contiguous", true);
    p.antialias = f.bool_or("antialias", true);
    f.finish();
    Document& d = ctx.doc;
    paint::bucket_fill(d.require_raster(layer, ctx.label), d.selection(), p);
}

// ---- transform ------------------------------------------------------------------------------------------
void op_transform(OpContext& ctx, Fields& f) {
    Document& d = ctx.doc;
    const std::string layer = f.req_string("layer");
    const Interp interp = read_interp(f);
    const bool has_matrix = f.has("matrix");
    const bool has_params = f.has("translate") || f.has("scale") || f.has("rotate") || f.has("skew") || f.has("pivot");
    const bool has_quad = f.has("quad") || f.has("rect");
    if ((has_matrix ? 1 : 0) + (has_params ? 1 : 0) + (has_quad ? 1 : 0) > 1)
        f.fail("fields of more than one transform form (matrix / params / quad) given");
    Mat3 M = transform::kIdentity;
    if (has_matrix) {
        const Json& m = f.raw_req("matrix");
        const std::string what = key_what(f, "matrix");
        if (!m.is_array() || m.size() != 9) throw ScriptError(what + " must be an array of 9 numbers");
        for (size_t i = 0; i < 9; ++i)
            M[i] = Fields::as_double(m[i], what + "[" + std::to_string(i) + "]", kAnyLo, kAnyHi);
    } else if (has_quad) {
        transform::Quad qd;
        const Json& q = f.raw_req("quad");
        const std::string what = key_what(f, "quad");
        if (!q.is_array() || q.size() != 4) throw ScriptError(what + " must be an array of 4 [x, y] points");
        for (size_t i = 0; i < 4; ++i) {
            const auto p = read_pair(q[i], what + "[" + std::to_string(i) + "]");
            qd.q[i] = {p[0], p[1]};
        }
        qd.rx = 0.0;
        qd.ry = 0.0;
        qd.rw = static_cast<double>(d.width());
        qd.rh = static_cast<double>(d.height());
        if (const Json* r = f.raw("rect")) {
            const std::string rw = key_what(f, "rect");
            if (!r->is_array() || r->size() != 4) throw ScriptError(rw + " must be an array [x, y, w, h]");
            qd.rx = Fields::as_double((*r)[0], rw + "[0]", kAnyLo, kAnyHi);
            qd.ry = Fields::as_double((*r)[1], rw + "[1]", kAnyLo, kAnyHi);
            qd.rw = Fields::as_double((*r)[2], rw + "[2]", kAnyLo, kAnyHi);
            qd.rh = Fields::as_double((*r)[3], rw + "[3]", kAnyLo, kAnyHi);
            if (!(qd.rw > 0.0) || !(qd.rh > 0.0)) throw ScriptError(rw + ": w and h must be > 0");
        }
        const auto m = transform::from_quad(qd);
        if (!m) f.fail("quad is degenerate (den == 0)");
        M = *m;
    } else {
        transform::Params p;
        const auto t = pair_or(f, "translate", {0.0, 0.0});
        const auto s = pair_or(f, "scale", {1.0, 1.0}, -1000.0, 1000.0);
        if (s[0] == 0.0 || s[1] == 0.0) f.fail("field 'scale' entries must be nonzero");
        p.rotate = f.double_or("rotate", 0.0, -3600.0, 3600.0);
        const auto k = pair_or(f, "skew", {0.0, 0.0}, -89.0, 89.0);
        const auto pv = pair_or(f, "pivot", {static_cast<double>(d.width()) / 2.0, static_cast<double>(d.height()) / 2.0});
        p.tx = t[0];
        p.ty = t[1];
        p.sx = s[0];
        p.sy = s[1];
        p.kx = k[0];
        p.ky = k[1];
        p.px = pv[0];
        p.py = pv[1];
        M = transform::from_params(p);
    }
    f.finish();
    Node& n = d.require_raster(layer, ctx.label);
    if (!geom::transform_layer(n, M, interp)) f.fail("matrix is singular (|det| < 1e-12)");
}

// ---- canvas geometry ---------------------------------------------------------------------------------
void op_image_size(OpContext& ctx, Fields& f) {
    const int w = static_cast<int>(f.req_int("w", 1, kMaxCanvasSide));
    const int h = static_cast<int>(f.req_int("h", 1, kMaxCanvasSide));
    const Interp interp = read_interp(f);
    f.finish();
    geom::image_size(ctx.doc.state(), w, h, interp);
}

void op_canvas_size(OpContext& ctx, Fields& f) {
    const int w = static_cast<int>(f.req_int("w", 1, kMaxCanvasSide));
    const int h = static_cast<int>(f.req_int("h", 1, kMaxCanvasSide));
    const std::string a = f.enum_or("anchor", "c", {"tl", "t", "tr", "l", "c", "r", "bl", "b", "br"});
    f.finish();
    geom::Anchor anchor = geom::Anchor::C;
    geom::parse_anchor(a, anchor);
    geom::canvas_size(ctx.doc.state(), w, h, anchor);
}

void op_crop(OpContext& ctx, Fields& f) {
    const int64_t x = f.req_int("x", kIntLo, kIntHi);
    const int64_t y = f.req_int("y", kIntLo, kIntHi);
    const int w = static_cast<int>(f.req_int("w", 1, kMaxCanvasSide));
    const int h = static_cast<int>(f.req_int("h", 1, kMaxCanvasSide));
    f.finish();
    geom::crop(ctx.doc.state(), x, y, w, h);
}

void op_rotate_canvas(OpContext& ctx, Fields& f) {
    const double angle = f.req_double("angle", -3600.0, 3600.0);
    f.finish();
    if (!geom::rotate_canvas_size(ctx.doc.width(), ctx.doc.height(), angle))
        f.fail("rotated canvas would exceed 16384 px per axis");
    geom::rotate_canvas(ctx.doc.state(), angle);
}

void op_flip(OpContext& ctx, Fields& f) {
    const bool h = f.req_enum("axis", {"h", "v"}) == "h";
    const Json* layer = f.raw("layer");
    if (layer && !layer->is_string()) f.fail("field 'layer' must be a string");
    f.finish();
    if (layer)
        geom::flip_layer(ctx.doc.require_raster(layer->get<std::string>(), ctx.label), h);
    else
        geom::flip_canvas(ctx.doc.state(), h);
}

}  // namespace

void registerGeometryOps(OpRegistry& r) {
    r.add("select_rect", op_select_rect);
    r.add("select_ellipse", op_select_ellipse);
    r.add("select_polygon", op_select_polygon);
    r.add("select_wand", op_select_wand);
    r.add("select_all", op_select_all);
    r.add("deselect", op_deselect);
    r.add("reselect", op_reselect);
    r.add("select_inverse", op_select_inverse);
    r.add("feather", op_feather);
    r.add("expand", op_expand);
    r.add("contract", op_contract);
    r.add("fill_selection", op_fill_selection);
    r.add("gradient", op_gradient);
    r.add("bucket_fill", op_bucket_fill);
    r.add("transform", op_transform);
    r.add("image_size", op_image_size);
    r.add("canvas_size", op_canvas_size);
    r.add("crop", op_crop);
    r.add("rotate_canvas", op_rotate_canvas);
    r.add("flip", op_flip);
}

}  // namespace rl::script
