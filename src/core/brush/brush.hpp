// SPDX-License-Identifier: GPL-3.0-or-later
//
// Brush engine, eraser and clone stamp (docs/math/40-brush.md).
//
// The one entry point is StrokeSession: begin() -> add_samples() any number of times -> end().
// The render-script ops brush_stroke / eraser_stroke / clone_stroke drive exactly this API with one
// add_samples() call per JSON sample (doc 40 §3), and the GUI drives it with one call per input
// event (or any batching it likes): the result is byte-identical however the sample stream is
// chunked, because every piece of per-stroke state (spacing accumulator, EMA, clone offset) lives
// in the session and is advanced per sample, never per call.
//
// Memory: the stroke buffer B (one double per canvas pixel, doc 40 §3.1.5) is sparse by 64x64
// tile: only tiles a dab has touched are allocated (32 KiB each), so a 16384^2 canvas never
// allocates the 2 GiB dense buffer.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/base/types.hpp"
#include "core/doc/document.hpp"
#include "core/tile/tiled_image.hpp"

namespace rl::brush {

// One input sample (doc 40 §2.1). Canvas coordinates (the GUI maps widget positions through the
// inverse view transform BEFORE building a Sample). All fields are required at this level; the
// render-script parser resolves the JSON defaults (pressure 1.0, tilt 0.0, t_ms = previous raw).
struct Sample {
    double x = 0.0;         // finite, [-1e6, 1e6]
    double y = 0.0;         // finite, [-1e6, 1e6]
    double pressure = 1.0;  // any finite value; the engine clamps to [0, 1] when it reads it
    double tilt_x = 0.0;    // degrees [-90, 90]; recorded only (unused by v0.1 math)
    double tilt_y = 0.0;    // degrees [-90, 90]; recorded only
    double t_ms = 0.0;      // finite, >= 0; ms since an arbitrary stroke origin (sample 0 = 0)
};

enum class Tool : unsigned char { Brush, Eraser, Clone };
enum class Target : unsigned char { Pixels, Mask };
enum class Mode : unsigned char { Wash, Buildup };

// A pressure curve: 2..16 points (x strictly increasing, first x 0.0, last x 1.0, x/y in [0, 1]).
using Curve = std::vector<std::array<double, 2>>;

// Every brush field of doc 40 §2.2-2.4, constant for the whole stroke. Defaults = the JSON defaults.
struct StrokeParams {
    Tool tool = Tool::Brush;
    Target target = Target::Pixels;  // Brush only; Eraser/Clone always paint pixels
    Rgba8 color{0, 0, 0, 255};       // Brush only; alpha must be 255
    double size = 20.0;              // [1, 5000] nominal dab diameter, canvas px
    double hardness = 1.0;           // [0, 1]
    double spacing = 0.25;           // [0.01, 10] fraction of the diameter
    double opacity = 1.0;            // [0, 1]
    double flow = 1.0;               // [0, 1]
    double angle = 0.0;              // [-360, 360] degrees, counter-clockwise on screen
    double roundness = 1.0;          // [0.01, 1]
    Mode mode = Mode::Wash;
    double dabs_per_second = 0.0;    // [0, 1000] airbrush rate, 0 = off
    double smoothing = 0.0;          // [0, 0.99] EMA stabilizer, 0 = off
    std::optional<Curve> size_curve;     // nullopt = factor 1.0
    std::optional<Curve> opacity_curve;  // nullopt = factor 1.0
    double view_zoom = 1.0;          // (0, 256]; informational only, never enters the math

    // Clone only.
    std::optional<std::array<double, 2>> source;  // sets the clone source point (Alt-click)
    bool aligned = true;
    std::string source_layer;  // empty = the target layer
};

// Throws rl::ScriptError(context + ...) when a field is out of its doc 40 range.
void validate(const StrokeParams& p, const std::string& context);
void validate(const Sample& s, const std::string& context);

// Clone-stamp tool state (doc 40 §4): not document state, never undone. The render-script ops keep
// one per script run (in Document::tool_state); the GUI keeps one per clone tool.
struct CloneState {
    std::optional<std::array<double, 2>> src;
    std::optional<std::array<int64_t, 2>> off;
};

// Integer pixel rectangle [x0, x1) x [y0, y1).
struct IRect {
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
    bool empty() const { return x1 <= x0 || y1 <= y0; }
};

// What changed since the previous add_samples() call.
struct StrokeDelta {
    IRect rect;                  // canvas pixels whose preview may have changed (clipped to canvas)
    std::vector<TileKey> tiles;  // the tiles overlapping `rect`, row-major
    size_t dabs = 0;             // dabs placed by this call
};

struct SessionOptions {
    // Maintain preview_pixels()/preview_mask() incrementally (GUI). The script ops turn it off.
    bool preview = true;
    // Push the one history record for the stroke at begin() (GUI). The script engine pushes the
    // record itself before running an op, so the script ops pass false.
    bool push_history = true;
};

class StrokeSession {
public:
    StrokeSession();
    ~StrokeSession();
    StrokeSession(const StrokeSession&) = delete;
    StrokeSession& operator=(const StrokeSession&) = delete;

    // Validates the params, resolves the target (a raster layer; with Target::Mask it must have a
    // mask), takes the snapshots S0 (and SRC for clone), bakes the pressure LUTs. For Tool::Clone,
    // `clone` must be non-null; it is updated per doc 40 §4 (a stroke without `source` when no
    // source was ever set is a ScriptError). Throws rl::ScriptError(context + ...) on any invalid
    // input, leaving the document unchanged. The document must not be structurally changed until
    // end().
    void begin(Document& doc, const std::string& layer_id, const StrokeParams& params,
               const SessionOptions& opts = {}, CloneState* clone = nullptr,
               const std::string& context = "stroke");

    // Feeds samples in input order. Throws rl::ScriptError when a sample is invalid or the stroke
    // would exceed 1,000,000 dabs (doc 40 §3.4); the session is then unusable.
    StrokeDelta add_samples(std::span<const Sample> samples);

    // The target layer's pixels as they would be committed now: composite(S0, B) (doc 40 §3.9).
    // Valid between begin() and end() with SessionOptions::preview; nullptr otherwise, or when the
    // target is the mask (then preview_mask() is non-null).
    const RgbaImage* preview_pixels() const;
    const GrayImage* preview_mask() const;

    // Commits composite(S0, B) into the target (doc 40 §3.9), and returns the render-script op
    // (brush_stroke / eraser_stroke / clone_stroke) whose replay reproduces the stroke byte for
    // byte, given the same document and clone state before it. At least one sample is required.
    nlohmann::json end();

    bool active() const;
    size_t dab_count() const;
    size_t sample_count() const;
    // Allocated stroke-buffer tiles (sparseness check).
    size_t buffer_tiles() const;
    const std::string& layer_id() const;
    Target target() const;

    struct Impl;  // opaque (public only so the detail:: test hooks can drive the walk)

private:
    std::unique_ptr<Impl> d_;
};

// The render-script op name for a tool.
const char* op_name(Tool t);

// Serialises params + samples to the op JSON (the inverse of the script parser).
nlohmann::json to_op(const std::string& layer_id, const StrokeParams& p, const std::vector<Sample>& samples);

// Testing hooks (doc 40 §8 worked examples).
namespace detail {
// Bakes the 256-entry uint16 LUT of doc 40 §3.3.
std::array<uint16_t, 256> bake_lut(const Curve& c);
// Dab mask m for one pixel (doc 40 §3.6).
double dab_mask(double cx, double cy, double size, double hardness, double roundness, double angle,
                int px, int py);
// One stroke-buffer update B -> B' for a pixel with mask m under a dab of flow f and opacity O
// (doc 40 §3.7, honouring mutations 7 and 42).
double accumulate(double b, double m, double flow, double opacity, Mode mode);
// Dab positions (x, y, p, d) a stroke would place (walk only, no pixels).
struct DabInfo {
    double x, y, p, d;
};
std::vector<DabInfo> walk(const StrokeParams& p, const std::vector<Sample>& samples, double* final_frac);
}  // namespace detail

}  // namespace rl::brush
