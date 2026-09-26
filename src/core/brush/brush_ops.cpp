// SPDX-License-Identifier: GPL-3.0-or-later
//
// Domain registration for docs/math/40-brush.md (brush lane): brush_stroke, eraser_stroke,
// clone_stroke. `undo` is owned by doc 10 / C10 and registered by registerCompositingOps.
//
// Each op parses its JSON (doc 40 §2), then drives the same StrokeSession API the GUI uses, with
// ONE add_samples() call per JSON sample (doc 40 §3), so the per-event walk state (mutation 8) and
// the history granularity (mutation 10) are exercised by the headless goldens.
#include <limits>
#include <memory>

#include "core/base/error.hpp"
#include "core/brush/brush.hpp"
#include "core/script/domains.hpp"

namespace rl::script {

namespace {

using brush::Curve;
using brush::Sample;
using brush::StrokeParams;
using brush::Tool;

constexpr double kDblMax = std::numeric_limits<double>::max();

std::optional<Curve> read_curve(Fields& f, const char* key) {
    const Json* v = f.raw(key);
    if (!v || v->is_null()) return std::nullopt;
    const std::string what = f.context() + ": field '" + key + "'";
    if (!v->is_array()) Fields::fail_at(what, "must be null or an array of [x, y] points");
    if (v->size() < 2 || v->size() > 16) Fields::fail_at(what, "must have 2 to 16 points");
    Curve c;
    for (size_t i = 0; i < v->size(); ++i) {
        const Json& pt = (*v)[i];
        const std::string pw = what + " point " + std::to_string(i);
        if (!pt.is_array() || pt.size() != 2) Fields::fail_at(pw, "must be an array [x, y]");
        c.push_back({Fields::as_double(pt[0], pw + " x", 0.0, 1.0), Fields::as_double(pt[1], pw + " y", 0.0, 1.0)});
        if (i > 0 && !(c[i][0] > c[i - 1][0])) Fields::fail_at(what, "x must be strictly increasing");
    }
    if (c.front()[0] != 0.0) Fields::fail_at(what, "first x must be exactly 0.0");
    if (c.back()[0] != 1.0) Fields::fail_at(what, "last x must be exactly 1.0");
    return c;
}

std::vector<Sample> read_samples(Fields& f) {
    const Json& arr = f.raw_req("samples");
    const std::string what = f.context() + ": field 'samples'";
    if (!arr.is_array()) Fields::fail_at(what, "must be an array of sample objects");
    if (arr.empty()) Fields::fail_at(what, "must contain at least 1 sample");
    std::vector<Sample> out;
    out.reserve(arr.size());
    double prev_t = 0.0;  // doc 40 §2.1: t_ms defaults to the previous sample's raw t_ms (first: 0.0)
    for (size_t i = 0; i < arr.size(); ++i) {
        const Json& so = arr[i];
        const std::string sctx = f.context() + ": sample " + std::to_string(i);
        if (!so.is_object()) Fields::fail_at(sctx, "must be a JSON object");
        Fields sf(so, sctx);
        Sample s;
        s.x = sf.req_double("x", -1e6, 1e6);
        s.y = sf.req_double("y", -1e6, 1e6);
        s.pressure = sf.double_or("pressure", 1.0, -kDblMax, kDblMax);
        s.tilt_x = sf.double_or("tilt_x", 0.0, -90.0, 90.0);
        s.tilt_y = sf.double_or("tilt_y", 0.0, -90.0, 90.0);
        s.t_ms = sf.double_or("t_ms", prev_t, 0.0, kDblMax);
        sf.finish();
        prev_t = s.t_ms;
        out.push_back(s);
    }
    return out;
}

void run_stroke(OpContext& ctx, Fields& f, Tool tool) {
    StrokeParams p;
    p.tool = tool;
    const std::string layer = f.req_string("layer");
    if (tool == Tool::Brush) {
        p.target = f.enum_or("target", "pixels", {"pixels", "mask"}) == "mask" ? brush::Target::Mask
                                                                                : brush::Target::Pixels;
        p.color = f.color_or("color", Rgba8{0, 0, 0, 255});
        if (p.color.a != 255) f.fail("field 'color' must be \"#RRGGBB\" or \"#RRGGBBFF\" (alpha FF)");
    }
    p.size = f.double_or("size", 20.0, 1.0, 5000.0);
    p.hardness = f.double_or("hardness", 1.0, 0.0, 1.0);
    p.spacing = f.double_or("spacing", 0.25, 0.01, 10.0);
    p.opacity = f.double_or("opacity", 1.0, 0.0, 1.0);
    p.flow = f.double_or("flow", 1.0, 0.0, 1.0);
    p.angle = f.double_or("angle", 0.0, -360.0, 360.0);
    p.roundness = f.double_or("roundness", 1.0, 0.01, 1.0);
    p.mode = f.enum_or("mode", "wash", {"wash", "buildup"}) == "buildup" ? brush::Mode::Buildup : brush::Mode::Wash;
    p.dabs_per_second = f.double_or("dabs_per_second", 0.0, 0.0, 1000.0);
    p.smoothing = f.double_or("smoothing", 0.0, 0.0, 0.99);
    p.size_curve = read_curve(f, "size_curve");
    p.opacity_curve = read_curve(f, "opacity_curve");
    p.view_zoom = f.double_or("view_zoom", 1.0, 0.0, 256.0);
    if (p.view_zoom == 0.0) f.fail("field 'view_zoom' must be > 0");
    if (tool == Tool::Clone) {
        if (const Json* src = f.raw("source")) {
            const std::string what = f.context() + ": field 'source'";
            if (!src->is_array() || src->size() != 2) Fields::fail_at(what, "must be an array [x, y]");
            p.source = std::array<double, 2>{Fields::as_double((*src)[0], what + " x", -1e6, 1e6),
                                             Fields::as_double((*src)[1], what + " y", -1e6, 1e6)};
        }
        p.aligned = f.bool_or("aligned", true);
        p.source_layer = f.string_or("source_layer", layer);
    }
    const std::vector<Sample> samples = read_samples(f);
    f.finish();

    brush::CloneState* clone = nullptr;
    if (tool == Tool::Clone) {
        // Tool state for this script run (doc 40 §4): not document state, never undone.
        std::shared_ptr<void>& slot = ctx.doc.tool_state("brush.clone");
        if (!slot) slot = std::make_shared<brush::CloneState>();
        clone = static_cast<brush::CloneState*>(slot.get());
    }

    brush::SessionOptions opts;
    opts.preview = false;       // headless: commit only
    opts.push_history = false;  // the engine already pushed this op's record (C10)
    brush::StrokeSession session;
    session.begin(ctx.doc, layer, p, opts, clone, ctx.label);
    for (const Sample& s : samples) session.add_samples(std::span<const Sample>(&s, 1));
    session.end();
}

void op_brush_stroke(OpContext& ctx, Fields& f) { run_stroke(ctx, f, Tool::Brush); }
void op_eraser_stroke(OpContext& ctx, Fields& f) { run_stroke(ctx, f, Tool::Eraser); }
void op_clone_stroke(OpContext& ctx, Fields& f) { run_stroke(ctx, f, Tool::Clone); }

}  // namespace

void registerBrushOps(OpRegistry& r) {
    r.add("brush_stroke", op_brush_stroke);
    r.add("eraser_stroke", op_eraser_stroke);
    r.add("clone_stroke", op_clone_stroke);
}

}  // namespace rl::script
