// SPDX-License-Identifier: GPL-3.0-or-later
//
// The six destructive filters of docs/math/20-adjustments-filters.md Part B and their framework
// (B0: lock transparency, coverage masks, premultiplied coverage lerp).
//
// Layering:
//   run_filter(params, O)             -> F, the filter over the whole layer (B1..B6), pure
//   finish_filter(O, F, lock, M)      -> R, the B0 pipeline tail (lock step, coverage lerp, canonical)
//   filter_layer(layer, params, cov, selection) -> the layer's new pixels, document untouched
//                                        (the GUI's live preview: render a copy, never history)
//   apply_filter(doc, id, params, cov, ctx)     -> writes the layer (no history push: the script
//                                        engine or the GUI pushes the one record first, C10)
//
// Mutation hooks owned here: 12 and 29 (B1), 28 (B3).
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "core/base/types.hpp"
#include "core/doc/document.hpp"

namespace rl::filters {

// A dense, canvas-sized RGBA8 buffer (row-major, straight alpha, canonical).
struct Image {
    int w = 0;
    int h = 0;
    std::vector<Rgba8> px;

    Image() = default;
    Image(int width, int height) : w(width), h(height), px(static_cast<size_t>(width) * static_cast<size_t>(height)) {}
    Rgba8& at(int x, int y) { return px[(static_cast<size_t>(y) * static_cast<size_t>(w)) + static_cast<size_t>(x)]; }
    const Rgba8& at(int x, int y) const {
        return px[(static_cast<size_t>(y) * static_cast<size_t>(w)) + static_cast<size_t>(x)];
    }
};

// B0.3 edge modes for reads outside the canvas.
enum class Edge { Clamp, Transparent };

struct GaussianBlurParams {
    double radius = 1.0;  // sigma in pixels, 0.1..250
    Edge edge = Edge::Clamp;
};
struct MotionBlurParams {
    double angle = 0.0;      // degrees, -360..360, counter-clockwise positive
    double distance = 10.0;  // pixels, 1..2000
    Edge edge = Edge::Clamp;
};
struct UnsharpMaskParams {
    double amount = 50.0;  // percent, 1..500
    double radius = 1.0;   // sigma, 0.1..250
    int threshold = 0;     // levels, 0..255
    Edge edge = Edge::Clamp;
};
enum class NoiseDistribution { Uniform, Gaussian };
struct AddNoiseParams {
    double amount = 10.0;  // percent of 255 levels, 0..400
    NoiseDistribution distribution = NoiseDistribution::Uniform;
    bool monochromatic = false;
    uint64_t seed = 0;  // 0..2^53-1
};
struct HighPassParams {
    double radius = 10.0;  // sigma, 0.1..250
    Edge edge = Edge::Clamp;
};
enum class OffsetMode { Transparent, Repeat, Wrap };
struct OffsetParams {
    int dx = 0;  // -65536..65536
    int dy = 0;
    OffsetMode mode = OffsetMode::Transparent;
};

using FilterParams = std::variant<GaussianBlurParams, MotionBlurParams, UnsharpMaskParams, AddNoiseParams,
                                  HighPassParams, OffsetParams>;

// The render-script op name of a filter ("filter_gaussian_blur", ...).
const char* op_name(const FilterParams& p);

// B0.2 coverage source.
struct Coverage {
    enum class Src { All, Selection, Rect, Ramp };
    Src src = Src::All;
    int64_t x = 0, y = 0, w = 0, h = 0;  // Rect (w, h >= 0; may extend past the canvas)
    int value = 255;                     // Rect, 0..255
    bool vertical = false;               // Ramp: false = "h", true = "v"

    static Coverage all() { return Coverage{}; }
    static Coverage selection() {
        Coverage c;
        c.src = Src::Selection;
        return c;
    }
};

// Throws rl::ScriptError (prefixed with `context`) when a parameter is outside its B1-B6 range.
void validate(const FilterParams& p, const std::string& context);
void validate(const Coverage& c, const std::string& context);

// B1 box widths from sigma (n = 3, Kovesi), narrow boxes first.
std::array<int, 3> box_widths(double sigma);

// B1 complete output (Stages 1-4, canonical rule; no coverage, no lock).
Image gaussian_blur(const Image& src, double radius, Edge edge);

// F = filter(O, params) over the whole layer (B1-B6).
Image run_filter(const FilterParams& p, const Image& src);

// M(x, y) of B0.2 for a W x H canvas (row-major bytes).
std::vector<uint8_t> coverage_mask(const Coverage& c, int w, int h, const Selection& sel);

// B0 pipeline tail: lock step (when lock_alpha), coverage lerp with M (empty M = 255 everywhere),
// canonical rule. Returns R.
Image finish_filter(const Image& original, Image filtered, bool lock_alpha, const std::vector<uint8_t>& mask);

// Dense <-> tiled conversion for a canvas-sized layer.
Image to_image(const RgbaImage& img);
// Writes `src` into `dst` tile by tile, leaving tiles whose content is unchanged shared (CoW) and
// keeping the image sparse.
void store_image(RgbaImage& dst, const Image& src);

// The new pixels of a raster layer after the filter (B0 pipeline), without touching the layer or
// any document history. This is the GUI's filter-dialog preview.
RgbaImage filter_layer(const Node& layer, const FilterParams& p, const Coverage& c, const Selection& sel);

// Applies a filter to raster layer `layer_id` of `doc` (script error when the id is unknown or not a
// raster layer). Pushes no history record; callers push exactly one first (C10).
void apply_filter(Document& doc, const std::string& layer_id, const FilterParams& p, const Coverage& c,
                  const std::string& context);

}  // namespace rl::filters
