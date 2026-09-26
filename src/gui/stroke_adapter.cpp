// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/stroke_adapter.hpp"

#include <QElapsedTimer>

#include <algorithm>
#include <exception>

namespace rl::gui {

namespace {

rl::brush::Sample to_engine(const StrokeSample& s) {
    rl::brush::Sample e;
    e.x = std::clamp(s.x, -1e6, 1e6);
    e.y = std::clamp(s.y, -1e6, 1e6);
    e.pressure = s.pressure;
    e.tilt_x = std::clamp(s.tilt_x, -90.0, 90.0);
    e.tilt_y = std::clamp(s.tilt_y, -90.0, 90.0);
    e.t_ms = std::max(0.0, s.t_ms);
    return e;
}

}  // namespace

StrokeAdapter::StrokeAdapter() = default;
StrokeAdapter::~StrokeAdapter() = default;

rl::brush::StrokeParams StrokeAdapter::params_for(const StrokeSetup& st) {
    namespace br = rl::brush;
    const BrushSettings& b = st.brush;
    br::StrokeParams p;
    p.tool = st.kind == StrokeKind::Brush ? br::Tool::Brush : st.kind == StrokeKind::Eraser ? br::Tool::Eraser : br::Tool::Clone;
    p.target = st.kind == StrokeKind::Brush && st.target_mask ? br::Target::Mask : br::Target::Pixels;
    p.color = rl::Rgba8{static_cast<uint8_t>(st.color.red()), static_cast<uint8_t>(st.color.green()),
                        static_cast<uint8_t>(st.color.blue()), 255};
    p.size = std::clamp(b.size, 1.0, 5000.0);
    p.hardness = std::clamp(b.hardness, 0.0, 1.0);
    p.spacing = std::clamp(b.spacing, 0.01, 10.0);
    p.opacity = std::clamp(b.opacity, 0.0, 1.0);
    p.flow = std::clamp(b.flow, 0.0, 1.0);
    p.angle = std::clamp(b.angle, -360.0, 360.0);
    p.roundness = std::clamp(b.roundness, 0.01, 1.0);
    p.mode = b.buildup ? br::Mode::Buildup : br::Mode::Wash;
    p.dabs_per_second = std::clamp(b.dabs_per_second, 0.0, 1000.0);
    p.smoothing = std::clamp(b.smoothing, 0.0, 0.99);
    const br::Curve identity = {{0.0, 0.0}, {1.0, 1.0}};
    if (b.pressure_size) p.size_curve = identity;
    if (b.pressure_opacity) p.opacity_curve = identity;
    p.view_zoom = std::clamp(st.view_zoom, 1e-6, 256.0);
    if (st.kind == StrokeKind::Clone) {
        if (st.clone_source) p.source = std::array<double, 2>{st.clone_source->x(), st.clone_source->y()};
        p.aligned = st.clone_aligned;
    }
    return p;
}

Json StrokeAdapter::make_op(const StrokeSetup& st, const std::vector<StrokeSample>& samples) {
    std::vector<rl::brush::Sample> es;
    es.reserve(samples.size());
    for (const StrokeSample& s : samples) es.push_back(to_engine(s));
    return rl::brush::to_op(st.layer, params_for(st), es);
}

std::string StrokeAdapter::begin(rl::Document& doc, const StrokeSetup& setup, const StrokeSample& first, StrokeUpdate* first_update) {
    if (active_) finish();
    setup_ = setup;
    samples_.clear();
    clone_ = rl::brush::CloneState{};  // every op carries its own "source": replayable on its own
    auto s = std::make_unique<rl::brush::StrokeSession>();
    try {
        s->begin(doc, setup.layer, params_for(setup), rl::brush::SessionOptions{true, true},
                 setup.kind == StrokeKind::Clone ? &clone_ : nullptr, "stroke");
    } catch (const std::exception& e) {
        return e.what();
    }
    session_ = std::move(s);
    active_ = true;
    StrokeUpdate u = feed(first);
    if (first_update) *first_update = std::move(u);
    return {};
}

StrokeUpdate StrokeAdapter::feed(const StrokeSample& s) {
    StrokeUpdate u;
    if (!active_) return u;
    const rl::brush::Sample e = to_engine(s);
    QElapsedTimer t;
    t.start();
    rl::brush::StrokeDelta d;
    try {
        d = session_->add_samples(std::span<const rl::brush::Sample>(&e, 1));
    } catch (const std::exception&) {
        // Past the dab cap or an invalid sample: the stroke keeps what it has (§5 caps).
        return u;
    }
    u.engine_ms = static_cast<double>(t.nsecsElapsed()) / 1e6;
    samples_.push_back(s);
    u.dabs = d.dabs;
    if (!d.rect.empty())
        u.rect = QRectF(d.rect.x0, d.rect.y0, d.rect.x1 - d.rect.x0, d.rect.y1 - d.rect.y0);
    u.tiles.reserve(d.tiles.size());
    for (const rl::TileKey& k : d.tiles) u.tiles.emplace_back(k.tx, k.ty);
    return u;
}

StrokeUpdate StrokeAdapter::add_sample(const StrokeSample& s) { return feed(s); }

StrokeUpdate StrokeAdapter::add_release_sample(const StrokeSample& s) {
    if (!active_ || samples_.empty()) return {};
    const StrokeSample& last = samples_.back();
    if (s.x == last.x && s.y == last.y && s.t_ms == last.t_ms) return {};
    return feed(s);
}

const rl::RgbaImage* StrokeAdapter::preview_pixels() const { return active_ ? session_->preview_pixels() : nullptr; }
const rl::GrayImage* StrokeAdapter::preview_mask() const { return active_ ? session_->preview_mask() : nullptr; }
size_t StrokeAdapter::dab_count() const { return active_ ? session_->dab_count() : 0; }

Json StrokeAdapter::finish() {
    if (!active_) return Json();
    Json op = session_->end();
    session_.reset();
    active_ = false;
    samples_.clear();
    return op;
}

}  // namespace rl::gui
