// SPDX-License-Identifier: GPL-3.0-or-later
//
// Domain registration for docs/math/20-adjustments-filters.md: add_adjustment (A9) and the six
// filter ops of Part B with the B0.2 `coverage` field. Validation is strict (doc 20 preamble).
#include <limits>

#include "core/adjust/adjust_ops.hpp"
#include "core/filters/filters.hpp"
#include "core/script/domains.hpp"

namespace rl::script {

namespace {

using filters::Coverage;
using filters::Edge;

// Rect components: any integer (resolutions §3); bounded so x + w cannot overflow int64.
constexpr int64_t kCoordMax = std::numeric_limits<int64_t>::max() / 4;

Edge parse_edge(Fields& f) {
    return f.enum_or("edge", "clamp", {"clamp", "transparent"}) == "clamp" ? Edge::Clamp : Edge::Transparent;
}

Coverage parse_coverage(Fields& f) {
    Coverage c;
    const Json* v = f.raw("coverage");
    if (!v) return c;
    if (!v->is_object()) f.fail("field 'coverage' must be an object");
    Fields g(*v, f.context() + " coverage");
    const std::string src = g.req_enum("src", {"selection", "rect", "ramp"});
    if (src == "selection") {
        c.src = Coverage::Src::Selection;
    } else if (src == "rect") {
        c.src = Coverage::Src::Rect;
        c.x = g.req_int("x", -kCoordMax, kCoordMax);
        c.y = g.req_int("y", -kCoordMax, kCoordMax);
        c.w = g.req_int("w", 0, kCoordMax);
        c.h = g.req_int("h", 0, kCoordMax);
        c.value = static_cast<int>(g.int_or("value", 255, 0, 255));
    } else {
        c.src = Coverage::Src::Ramp;
        c.vertical = g.req_enum("dir", {"h", "v"}) == "v";
    }
    g.finish();
    return c;
}

// Reads `layer` and `coverage`, then the filter's own fields via `read`, then applies.
template <class ReadParams>
void run_filter_op(OpContext& ctx, Fields& f, ReadParams read) {
    const std::string layer = f.req_string("layer");
    const filters::FilterParams params = read(f);
    const Coverage cov = parse_coverage(f);
    f.finish();
    filters::apply_filter(ctx.doc, layer, params, cov, ctx.label);
}

void op_gaussian_blur(OpContext& ctx, Fields& f) {
    run_filter_op(ctx, f, [](Fields& g) {
        filters::GaussianBlurParams p;
        p.radius = g.double_or("radius", 1.0, 0.1, 250.0);
        p.edge = parse_edge(g);
        return filters::FilterParams(p);
    });
}

void op_motion_blur(OpContext& ctx, Fields& f) {
    run_filter_op(ctx, f, [](Fields& g) {
        filters::MotionBlurParams p;
        p.angle = g.double_or("angle", 0.0, -360.0, 360.0);
        p.distance = g.double_or("distance", 10.0, 1.0, 2000.0);
        p.edge = parse_edge(g);
        return filters::FilterParams(p);
    });
}

void op_unsharp_mask(OpContext& ctx, Fields& f) {
    run_filter_op(ctx, f, [](Fields& g) {
        filters::UnsharpMaskParams p;
        p.amount = g.double_or("amount", 50.0, 1.0, 500.0);
        p.radius = g.double_or("radius", 1.0, 0.1, 250.0);
        p.threshold = static_cast<int>(g.int_or("threshold", 0, 0, 255));
        p.edge = parse_edge(g);
        return filters::FilterParams(p);
    });
}

void op_add_noise(OpContext& ctx, Fields& f) {
    run_filter_op(ctx, f, [](Fields& g) {
        filters::AddNoiseParams p;
        p.amount = g.double_or("amount", 10.0, 0.0, 400.0);
        p.distribution = g.enum_or("distribution", "uniform", {"uniform", "gaussian"}) == "gaussian"
                             ? filters::NoiseDistribution::Gaussian
                             : filters::NoiseDistribution::Uniform;
        p.monochromatic = g.bool_or("monochromatic", false);
        p.seed = static_cast<uint64_t>(g.int_or("seed", 0, 0, 9007199254740991LL));
        return filters::FilterParams(p);
    });
}

void op_high_pass(OpContext& ctx, Fields& f) {
    run_filter_op(ctx, f, [](Fields& g) {
        filters::HighPassParams p;
        p.radius = g.double_or("radius", 10.0, 0.1, 250.0);
        p.edge = parse_edge(g);
        return filters::FilterParams(p);
    });
}

void op_offset(OpContext& ctx, Fields& f) {
    run_filter_op(ctx, f, [](Fields& g) {
        filters::OffsetParams p;
        p.dx = static_cast<int>(g.int_or("dx", 0, -65536, 65536));
        p.dy = static_cast<int>(g.int_or("dy", 0, -65536, 65536));
        const std::string mode = g.enum_or("mode", "transparent", {"transparent", "repeat", "wrap"});
        p.mode = mode == "wrap" ? filters::OffsetMode::Wrap
                                : (mode == "repeat" ? filters::OffsetMode::Repeat : filters::OffsetMode::Transparent);
        return filters::FilterParams(p);
    });
}

}  // namespace

void registerAdjustFilterOps(OpRegistry& r) {
    registerAdjustmentOps(r);
    r.add("filter_gaussian_blur", op_gaussian_blur);
    r.add("filter_motion_blur", op_motion_blur);
    r.add("filter_unsharp_mask", op_unsharp_mask);
    r.add("filter_add_noise", op_add_noise);
    r.add("filter_high_pass", op_high_pass);
    r.add("filter_offset", op_offset);
}

}  // namespace rl::script
