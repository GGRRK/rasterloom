// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/adjust/adjust_math.hpp"

#include <algorithm>
#include <cmath>

#include "core/base/mutation.hpp"
#include "core/base/quant.hpp"

namespace rl::adjust {

// ---- A1 Levels -------------------------------------------------------------------------------------
namespace {

struct LevelsPre {
    double lb, hb, ob, ow, inv, gamma;
};

LevelsPre levels_pre(const LevelsSetting& s) {
    LevelsPre p{};
    p.lb = static_cast<double>(s.in_black) / 255.0;
    p.hb = static_cast<double>(s.in_white) / 255.0;
    p.ob = static_cast<double>(s.out_black) / 255.0;
    p.ow = static_cast<double>(s.out_white) / 255.0;
    p.inv = 1.0 / s.gamma;
    p.gamma = s.gamma;
    return p;
}

double levels_map(const LevelsPre& p, double x) {
    double v = (x - p.lb) / (p.hb - p.lb);
    v = clamp(v, 0.0, 1.0);
    if (p.gamma != 1.0 && v > 0.0) {
        // Mutation 24 (doc 20 Part D): pow(v, gamma) instead of pow(v, 1/gamma).
        v = mut::active(24) ? std::pow(v, p.gamma) : std::pow(v, p.inv);
    }
    return p.ob + (v * (p.ow - p.ob));
}

}  // namespace

Lut8 levels_lut(const LevelsSetting& channel, const LevelsSetting& composite) {
    const LevelsPre pc = levels_pre(channel);
    const LevelsPre pr = levels_pre(composite);
    Lut8 lut{};
    for (int i = 0; i < 256; ++i) {
        const double x = static_cast<double>(i) / 255.0;
        const double y = levels_map(pr, levels_map(pc, x));
        lut[static_cast<size_t>(i)] = q(y);
    }
    return lut;
}

// ---- A2 Curves -------------------------------------------------------------------------------------
CurveSpline::CurveSpline(const CurvePoints& pts) {
    const size_t n = pts.size();
    for (const auto& p : pts) {
        x_.push_back(static_cast<double>(p[0]));
        y_.push_back(static_cast<double>(p[1]));
    }
    const size_t m = n - 1;
    const double K6 = 1.0 / 6.0;
    h_.assign(m, 0.0);
    for (size_t i = 0; i < m; ++i) h_[i] = x_[i + 1] - x_[i];

    c_.assign(m + 1, 0.0);
    if (m >= 2) {
        const size_t s = m - 1;
        std::vector<double> TB(s), TF(s), TA(s > 1 ? s - 1 : 0), U(s);
        for (size_t k = 0; k < s; ++k) {
            TB[k] = 2.0 * (h_[k] + h_[k + 1]);
            TF[k] = 6.0 * (((y_[k + 2] - y_[k + 1]) / h_[k + 1]) - ((y_[k + 1] - y_[k]) / h_[k]));
        }
        for (size_t k = 0; k + 1 < s; ++k) TA[k] = h_[k + 1];
        if (s == 1) {
            U[0] = TF[0] / TB[0];
        } else {
            std::vector<double> AL(s), BE(s);
            AL[1] = -TA[0] / TB[0];
            BE[1] = TF[0] / TB[0];
            for (size_t k = 1; k + 2 <= s; ++k) {  // k in 1..s-2
                const double den = (TA[k - 1] * AL[k]) + TB[k];
                AL[k + 1] = -TA[k] / den;
                BE[k + 1] = (TF[k] - (TA[k - 1] * BE[k])) / den;
            }
            U[s - 1] = (TF[s - 1] - (TA[s - 2] * BE[s - 1])) / (TB[s - 1] + (TA[s - 2] * AL[s - 1]));
            for (size_t k = s - 1; k-- > 0;) U[k] = (AL[k + 1] * U[k + 1]) + BE[k + 1];
        }
        for (size_t k = 0; k < s; ++k) c_[k + 1] = U[k];
    }

    d_.assign(m, 0.0);
    b_.assign(m, 0.0);
    for (size_t i = 0; i < m; ++i) {
        d_[i] = (c_[i + 1] - c_[i]) / h_[i];
        b_[i] = ((-0.5 * (c_[i] * h_[i])) - (K6 * ((d_[i] * h_[i]) * h_[i]))) + ((y_[i + 1] - y_[i]) / h_[i]);
    }
}

double CurveSpline::eval(double x) const {
    const double K6 = 1.0 / 6.0;
    const size_t m = x_.size() - 1;
    // Mutation 25 (doc 20 Part D): the flat-extension clamp is removed.
    if (!mut::active(25)) x = clamp(x, x_[0], x_[m]);
    size_t i = 0;
    for (size_t j = 0; j < m; ++j)
        if (x_[j] <= x) i = j;
    const double t = x - x_[i];
    const double y = ((y_[i] + (b_[i] * t)) + (((0.5 * c_[i]) * t) * t)) + ((((K6 * d_[i]) * t) * t) * t);
    return clamp(y, 0.0, 255.0);
}

Lut8 curves_lut(const CurveSpline& channel, const CurveSpline& composite) {
    Lut8 lut{};
    for (int i = 0; i < 256; ++i)
        lut[static_cast<size_t>(i)] = q(composite.eval(channel.eval(static_cast<double>(i))) / 255.0);
    return lut;
}

// ---- A3 Brightness/Contrast ----------------------------------------------------------------------
Lut8 brightness_contrast_lut(const BrightnessContrastParams& p) {
    const double bh = p.brightness / 2.0;
    double slant = 1.0;
    if (p.contrast != 0.0) slant = std::tan((p.contrast + 1.0) * 0.7853981633974483);
    Lut8 lut{};
    for (int i = 0; i < 256; ++i) {
        double v = static_cast<double>(i) / 255.0;
        if (bh < 0.0)
            v = v * (1.0 + bh);
        else
            v = v + ((1.0 - v) * bh);
        v = ((v - 0.5) * slant) + 0.5;
        lut[static_cast<size_t>(i)] = q(v);
    }
    return lut;
}

// ---- A4 HSL ------------------------------------------------------------------------------------------
Hsl rgb_to_hsl(double r, double g, double b) {
    const double mx = std::max(std::max(r, g), b);
    const double mn = std::min(std::min(r, g), b);
    const double l = (mx + mn) / 2.0;
    const double d = mx - mn;
    if (d == 0.0) return Hsl{0.0, 0.0, l};
    double s;
    if (l <= 0.5)
        s = d / (mx + mn);
    else
        s = d / ((2.0 - mx) - mn);
    double h6;
    if (r == mx)
        h6 = (g - b) / d;
    else if (g == mx)
        h6 = 2.0 + ((b - r) / d);
    else
        h6 = 4.0 + ((r - g) / d);
    if (h6 < 0.0) h6 = h6 + 6.0;
    return Hsl{h6, s, l};
}

double hsl_value(double n1, double n2, double h) {
    if (h > 6.0)
        h = h - 6.0;
    else if (h < 0.0)
        h = h + 6.0;
    if (h < 1.0) return n1 + ((n2 - n1) * h);
    if (h < 3.0) return n2;
    if (h < 4.0) return n1 + ((n2 - n1) * (4.0 - h));
    return n1;
}

void hsl_to_rgb(double h6, double s, double l, double& r, double& g, double& b) {
    if (s == 0.0) {
        r = g = b = l;
        return;
    }
    double m2;
    if (l <= 0.5)
        m2 = l * (1.0 + s);
    else
        m2 = (l + s) - (l * s);
    const double m1 = (2.0 * l) - m2;
    r = hsl_value(m1, m2, h6 + 2.0);
    g = hsl_value(m1, m2, h6);
    b = hsl_value(m1, m2, h6 - 2.0);
}

HueSatScalars hue_sat_scalars(const HueSaturationParams& p) {
    HueSatScalars k;
    k.colorize = p.colorize;
    k.vl = p.lightness / 100.0;
    if (p.colorize) {
        k.hc = p.hue / 60.0;
        if (k.hc >= 6.0) k.hc = k.hc - 6.0;
        k.sc = p.saturation / 100.0;
    } else {
        k.dh = p.hue / 60.0;
        k.ks = 1.0 + (p.saturation / 100.0);
    }
    return k;
}

void hue_sat_pixel(const HueSatScalars& k, uint8_t& r8, uint8_t& g8, uint8_t& b8) {
    Hsl c = rgb_to_hsl(dec(r8), dec(g8), dec(b8));
    double l = c.l;
    if (k.vl < 0.0)
        l = l * (k.vl + 1.0);
    else
        l = l + (k.vl * (1.0 - l));
    double r, g, b;
    if (k.colorize) {
        hsl_to_rgb(k.hc, k.sc, l, r, g, b);
    } else {
        // Mutation 26 (doc 20 Part D): h6 - dh instead of h6 + dh.
        double h6 = mut::active(26) ? (c.h6 - k.dh) : (c.h6 + k.dh);
        if (h6 >= 6.0)
            h6 = h6 - 6.0;
        else if (h6 < 0.0)
            h6 = h6 + 6.0;
        const double s = clamp(c.s * k.ks, 0.0, 1.0);
        hsl_to_rgb(h6, s, l, r, g, b);
    }
    r8 = q(r);
    g8 = q(g);
    b8 = q(b);
}

// ---- A5 Black & White ----------------------------------------------------------------------------
BlackWhiteScalars black_white_scalars(const BlackWhiteParams& p) {
    BlackWhiteScalars k{};
    k.wR = p.reds / 100.0;
    k.wY = p.yellows / 100.0;
    k.wG = p.greens / 100.0;
    k.wC = p.cyans / 100.0;
    k.wB = p.blues / 100.0;
    k.wM = p.magentas / 100.0;
    if (p.tint) {
        k.tinted = true;
        const Hsl t = rgb_to_hsl(dec(p.tint->r), dec(p.tint->g), dec(p.tint->b));
        k.th6 = t.h6;
        k.ts = t.s;
    }
    return k;
}

void black_white_pixel(const BlackWhiteScalars& k, uint8_t& r8, uint8_t& g8, uint8_t& b8) {
    const int R = r8, G = g8, B = b8;
    const int mx = std::max(std::max(R, G), B);
    const int mn = std::min(std::min(R, G), B);
    const int md = ((R + G + B) - mx) - mn;
    const double wp = (R == mx) ? k.wR : ((G == mx) ? k.wG : k.wB);
    const double ws = (B == mn) ? k.wY : ((R == mn) ? k.wC : k.wM);
    const double Y = (static_cast<double>(mn) + (static_cast<double>(md - mn) * ws)) +
                     (static_cast<double>(mx - md) * wp);
    const double g = Y / 255.0;
    if (!k.tinted) {
        r8 = g8 = b8 = q(g);
        return;
    }
    const double gl = clamp(g, 0.0, 1.0);
    double r, g2, b;
    hsl_to_rgb(k.th6, k.ts, gl, r, g2, b);
    r8 = q(r);
    g8 = q(g2);
    b8 = q(b);
}

// ---- A6 / A7 / A8 --------------------------------------------------------------------------------
Lut8 invert_lut() {
    Lut8 lut{};
    for (int i = 0; i < 256; ++i) lut[static_cast<size_t>(i)] = static_cast<uint8_t>(255 - i);
    return lut;
}

Lut8 posterize_lut(int levels) {
    // Mutation 27 (doc 20 Part D): kd = n instead of n - 1.
    const double kd = static_cast<double>(mut::active(27) ? levels : (levels - 1));
    Lut8 lut{};
    for (int i = 0; i < 256; ++i) {
        const double t = (static_cast<double>(i) / 255.0) * kd;
        const double j = std::floor(t) + (((t - std::floor(t)) >= 0.5) ? 1.0 : 0.0);
        lut[static_cast<size_t>(i)] = q(j / kd);
    }
    return lut;
}

uint8_t threshold_value(int level, uint8_t r, uint8_t g, uint8_t b) {
    const int64_t y8 = ((int64_t{299} * r) + (int64_t{587} * g) + (int64_t{114} * b) + 500) / 1000;
    return (y8 >= level) ? uint8_t{255} : uint8_t{0};
}

}  // namespace rl::adjust
