// SPDX-License-Identifier: GPL-3.0-or-later
//
// The formulas of docs/math/20-adjustments-filters.md Part A, each in the doc's evaluation order
// (00-conventions C1: binary64, no FMA, no algebraic rewriting). Params are assumed validated.
// Mutation hooks owned here: 24 (A1 gamma), 25 (A2 flat extension), 26 (A4 hue sign), 27 (A7 kd).
#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "core/adjust/adjust_params.hpp"

namespace rl::adjust {

using Lut8 = std::array<uint8_t, 256>;

// ---- A1 Levels -------------------------------------------------------------------------------------
// LUT_c[i] = q(L_rgb(L_c(i / 255.0))).
Lut8 levels_lut(const LevelsSetting& channel, const LevelsSetting& composite);

// ---- A2 Curves -------------------------------------------------------------------------------------
// Natural cubic spline through integer control points (Krita's construction, level units).
class CurveSpline {
public:
    explicit CurveSpline(const CurvePoints& pts);
    // S(x): flat extension outside [X0, Xm], output clamped to [0, 255].
    double eval(double x) const;

    const std::vector<double>& second_derivatives() const { return c_; }
    const std::vector<double>& slopes() const { return b_; }

private:
    std::vector<double> x_, y_, h_, c_, d_, b_;
};

// LUT_c[i] = q(S_rgb(S_c(i)) / 255.0).
Lut8 curves_lut(const CurveSpline& channel, const CurveSpline& composite);

// ---- A3 Brightness/Contrast ----------------------------------------------------------------------
Lut8 brightness_contrast_lut(const BrightnessContrastParams& p);

// ---- A4 HSL (sextant units) ----------------------------------------------------------------------
struct Hsl {
    double h6 = 0.0, s = 0.0, l = 0.0;
};
Hsl rgb_to_hsl(double r, double g, double b);
double hsl_value(double n1, double n2, double h);
void hsl_to_rgb(double h6, double s, double l, double& r, double& g, double& b);

// Per-op precomputed scalars of A4 (master or colorize).
struct HueSatScalars {
    bool colorize = false;
    double dh = 0.0, ks = 1.0, vl = 0.0;  // master
    double hc = 0.0, sc = 0.0;            // colorize
};
HueSatScalars hue_sat_scalars(const HueSaturationParams& p);
void hue_sat_pixel(const HueSatScalars& k, uint8_t& r, uint8_t& g, uint8_t& b);

// ---- A5 Black & White ----------------------------------------------------------------------------
struct BlackWhiteScalars {
    double wR, wY, wG, wC, wB, wM;
    bool tinted = false;
    double th6 = 0.0, ts = 0.0;
};
BlackWhiteScalars black_white_scalars(const BlackWhiteParams& p);
void black_white_pixel(const BlackWhiteScalars& k, uint8_t& r, uint8_t& g, uint8_t& b);

// ---- A6 / A7 / A8 --------------------------------------------------------------------------------
Lut8 invert_lut();
Lut8 posterize_lut(int levels);
// Y8 = (299 R + 587 G + 114 B + 500) // 1000; 255 if Y8 >= level else 0.
uint8_t threshold_value(int level, uint8_t r, uint8_t g, uint8_t b);

}  // namespace rl::adjust
