// SPDX-License-Identifier: GPL-3.0-or-later
//
// Typed parameters of the eight adjustment types (docs/math/20-adjustments-filters.md A1-A8).
// Every struct's defaults are the doc's defaults. The GUI edits these structs directly; render
// scripts reach them through parse_adjustment_params() (adjustment.hpp). Ranges are checked by the
// builders (make_adjustment), never clamped.
#pragma once

#include <array>
#include <optional>
#include <variant>
#include <vector>

#include "core/base/types.hpp"

namespace rl::adjust {

// A1: one Levels setting S = (in_black, in_white, gamma, out_black, out_white).
struct LevelsSetting {
    int in_black = 0;      // 0..254
    int in_white = 255;    // 1..255, in_black < in_white
    double gamma = 1.0;    // 0.1..9.99
    int out_black = 0;     // 0..255
    int out_white = 255;   // 0..255 (may be < out_black)
};

struct LevelsParams {
    LevelsSetting rgb, r, g, b;
};

// A2: 2..16 control points (in, out), ints 0..255, `in` strictly increasing.
using CurvePoints = std::vector<std::array<int, 2>>;

inline CurvePoints identity_curve() { return CurvePoints{{{0, 0}}, {{255, 255}}}; }

struct CurvesParams {
    CurvePoints rgb = identity_curve();
    CurvePoints r = identity_curve();
    CurvePoints g = identity_curve();
    CurvePoints b = identity_curve();
};

// A3.
struct BrightnessContrastParams {
    double brightness = 0.0;  // -1..1
    double contrast = 0.0;    // -1..1
};

// A4. Note the doc's colorize-dependent default: saturation is 25.0 when colorize is true (the
// script parser applies it; a GUI that toggles colorize should do the same).
struct HueSaturationParams {
    bool colorize = false;
    double hue = 0.0;         // -180..180 (colorize: 0..360)
    double saturation = 0.0;  // -100..100 (colorize: 0..100)
    double lightness = 0.0;   // -100..100
};

// A5.
struct BlackWhiteParams {
    double reds = 40.0, yellows = 60.0, greens = 40.0, cyans = 60.0, blues = 20.0, magentas = 80.0;  // -200..300
    std::optional<Rgba8> tint;  // RGB only; alpha ignored (always FF from "#RRGGBB")
};

// A6.
struct InvertParams {};

// A7.
struct PosterizeParams {
    int levels = 4;  // 2..255
};

// A8.
struct ThresholdParams {
    int level = 128;  // 1..255
};

// Variant order = the doc's type order (A9).
using AdjustmentParams = std::variant<LevelsParams, CurvesParams, BrightnessContrastParams, HueSaturationParams,
                                      BlackWhiteParams, InvertParams, PosterizeParams, ThresholdParams>;

}  // namespace rl::adjust
