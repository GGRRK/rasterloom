// SPDX-License-Identifier: GPL-3.0-or-later
//
// StrokeAdapter: the ONE place the GUI talks to the brush engine (rl::brush::StrokeSession).
//
// begin() validates the setup and starts a live session on the document (the session takes its
// copy-on-write snapshots and pushes exactly one history record; the document's pixels stay as
// they were). Every accepted input event goes to add_sample(), which feeds the engine
// incrementally and returns the canvas tiles whose preview changed. preview_pixels() /
// preview_mask() are the target's would-be-committed content; the canvas composites them in place
// of the layer. finish() commits: the engine writes the preview into the layer (the committed
// bytes equal the preview byte for byte) and the returned op (brush_stroke / eraser_stroke /
// clone_stroke, every field and every sample) replays to the same pixels through the CLI.
#pragma once

#include <QColor>
#include <QPointF>
#include <QRect>
#include <QRectF>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/brush/brush.hpp"
#include "gui/op_runner.hpp"
#include "gui/tools.hpp"

namespace rl::gui {

struct StrokeSample {
    double x = 0.0;  // canvas coordinates (doc 40 §2.1)
    double y = 0.0;
    double pressure = 1.0;
    double tilt_x = 0.0;
    double tilt_y = 0.0;
    double t_ms = 0.0;
};

enum class StrokeKind { Brush, Eraser, Clone };

struct StrokeSetup {
    StrokeKind kind = StrokeKind::Brush;
    BrushSettings brush;
    std::string layer;
    bool target_mask = false;          // brush only: paint the layer mask
    QColor color = Qt::black;          // brush only (alpha ignored: doc 40 requires AA = FF)
    double view_zoom = 1.0;            // informational (doc 40 §2.2)
    std::optional<QPointF> clone_source;  // clone only: the op's "source" (canvas coords)
    bool clone_aligned = true;
};

// What one add_sample() call changed.
struct StrokeUpdate {
    QRectF rect;                    // canvas pixels whose preview may have changed (empty = none)
    std::vector<QPoint> tiles;      // tiles overlapping `rect`, row-major
    size_t dabs = 0;                // dabs placed by this call
    double engine_ms = 0.0;         // time spent inside the engine
};

class StrokeAdapter {
public:
    StrokeAdapter();
    ~StrokeAdapter();

    // Starts a live stroke on `doc`. Returns an error message (empty on success); on error the
    // document is unchanged and the adapter stays inactive.
    std::string begin(rl::Document& doc, const StrokeSetup& setup, const StrokeSample& first, StrokeUpdate* first_update = nullptr);
    // One accepted input event (doc 40 §5).
    StrokeUpdate add_sample(const StrokeSample& s);
    // The pointer-up sample: added only if position or time differs from the last one (§5).
    StrokeUpdate add_release_sample(const StrokeSample& s);
    bool active() const { return active_; }
    const std::vector<StrokeSample>& samples() const { return samples_; }
    const StrokeSetup& setup() const { return setup_; }

    // The live preview of the target (the layer's pixels, or its mask for a mask stroke).
    const rl::RgbaImage* preview_pixels() const;
    const rl::GrayImage* preview_mask() const;
    bool targets_mask() const { return active_ && setup_.kind == StrokeKind::Brush && setup_.target_mask; }
    size_t dab_count() const;

    // Commits the stroke (writes the preview into the layer) and returns its op; inactive after.
    Json finish();

    // The engine parameters for a setup (exposed for tests).
    static rl::brush::StrokeParams params_for(const StrokeSetup& setup);
    // The op for a setup and a sample list, identical to what finish() returns.
    static Json make_op(const StrokeSetup& setup, const std::vector<StrokeSample>& samples);

private:
    StrokeUpdate feed(const StrokeSample& s);

    bool active_ = false;
    StrokeSetup setup_;
    std::vector<StrokeSample> samples_;
    std::unique_ptr<rl::brush::StrokeSession> session_;
    rl::brush::CloneState clone_;
};

}  // namespace rl::gui
