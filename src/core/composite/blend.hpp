// SPDX-License-Identifier: GPL-3.0-or-later
//
// Blend functions B(cb, cs) of docs/math/10-compositing.md §3, evaluated exactly as written there
// (00-conventions C1: parentheses as shown, left to right, no rewriting, no FMA).
//
// Mutation hooks owned here: 0 (Pegtop soft light), 1 (Rec.709 Lum), 16 (ColorDodge guard order),
// 17 (Hard Mix threshold), 18 (Darker/Lighter Color ties), 21 (Divide guard order).
#pragma once

#include "core/doc/blend_mode.hpp"

namespace rl::composite {

struct Vec3 {
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;

    double& operator[](int i) { return i == 0 ? r : (i == 1 ? g : b); }
    double operator[](int i) const { return i == 0 ? r : (i == 1 ? g : b); }
};

// ---- W3C helpers (§3.2, §3.4) ---------------------------------------------------------------------
double Multiply(double a, double b);
double Screen(double a, double b);
double ColorDodge(double cb, double s);
double ColorBurn(double cb, double s);
double HardLight(double cb, double s);
double SoftLight(double cb, double cs);

double Lum(const Vec3& c);
Vec3 ClipColor(Vec3 c);
Vec3 SetLum(const Vec3& c, double l);
double Sat(const Vec3& c);
Vec3 SetSat(const Vec3& c, double s);

// B for a separable mode on one channel. `cb`, `cs` are the decoded channel values; `bb`, `sb` the
// backdrop/source bytes they came from (used only by the integer mode hMix). Not clamped.
double blend_separable(BlendMode m, double cb, double cs, int bb, int sb);

// B for a vector mode (dkCl, lgCl, hue, sat, colr, lum). `bb`, `sb` are the bytes (used only by the
// integer modes dkCl/lgCl). Not clamped.
Vec3 blend_vector(BlendMode m, const Vec3& cb, const Vec3& cs, const int bb[3], const int sb[3]);

// bl = clamp01(B(cb, cs)) per channel, for any mode except Pass/Norm/Diss (§4.1 step 4).
Vec3 blend_clamped(BlendMode m, const Vec3& cb, const Vec3& cs, const int bb[3], const int sb[3]);

}  // namespace rl::composite
