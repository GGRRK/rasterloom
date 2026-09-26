// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/composite/blend.hpp"

#include <algorithm>
#include <cmath>

#include "core/base/mutation.hpp"
#include "core/base/quant.hpp"

namespace rl::composite {

// ---- §3.2 W3C separable helpers -------------------------------------------------------------------

double Multiply(double a, double b) { return a * b; }

double Screen(double a, double b) { return 1.0 - ((1.0 - a) * (1.0 - b)); }

double ColorDodge(double cb, double s) {
    if (mut::active(16)) {  // mutation 16: s == 1 checked first, so B(0, 1) = 1
        if (s == 1.0) return 1.0;
        if (cb == 0.0) return 0.0;
        return std::min(1.0, cb / (1.0 - s));
    }
    if (cb == 0.0) return 0.0;
    if (s == 1.0) return 1.0;
    return std::min(1.0, cb / (1.0 - s));
}

double ColorBurn(double cb, double s) {
    if (cb == 1.0) return 1.0;
    if (s == 0.0) return 0.0;
    return 1.0 - std::min(1.0, (1.0 - cb) / s);
}

double HardLight(double cb, double s) {
    if (s <= 0.5) return Multiply(cb, 2.0 * s);
    return Screen(cb, (2.0 * s) - 1.0);
}

double SoftLight(double cb, double cs) {
    if (mut::active(0)) {  // mutation 0: Pegtop
        return ((1.0 - cb) * (cb * cs)) + (cb * Screen(cb, cs));
    }
    if (cs <= 0.5) return cb - (((1.0 - (2.0 * cs)) * cb) * (1.0 - cb));
    double D = 0.0;
    if (cb <= 0.25)
        D = ((((16.0 * cb) - 12.0) * cb) + 4.0) * cb;
    else
        D = std::sqrt(cb);
    return cb + (((2.0 * cs) - 1.0) * (D - cb));
}

// ---- §3.4 W3C non-separable helpers ---------------------------------------------------------------

double Lum(const Vec3& c) {
    if (mut::active(1))  // mutation 1: Rec.709 luma
        return ((0.2126 * c.r) + (0.7152 * c.g)) + (0.0722 * c.b);
    return ((0.3 * c.r) + (0.59 * c.g)) + (0.11 * c.b);
}

Vec3 ClipColor(Vec3 c) {
    // L, n, x are computed once, on entry.
    const double L = Lum(c);
    const double n = std::min(std::min(c.r, c.g), c.b);
    const double x = std::max(std::max(c.r, c.g), c.b);
    if (n < 0.0) {
        const double d1 = L - n;
        if (d1 > 0.0) {
            for (int ch = 0; ch < 3; ++ch) c[ch] = L + (((c[ch] - L) * L) / d1);
        }
    }
    if (x > 1.0) {
        const double d2 = x - L;
        if (d2 > 0.0) {
            for (int ch = 0; ch < 3; ++ch) c[ch] = L + (((c[ch] - L) * (1.0 - L)) / d2);
        }
    }
    return c;
}

Vec3 SetLum(const Vec3& c, double l) {
    const double d = l - Lum(c);
    return ClipColor(Vec3{c.r + d, c.g + d, c.b + d});
}

double Sat(const Vec3& c) { return std::max(std::max(c.r, c.g), c.b) - std::min(std::min(c.r, c.g), c.b); }

Vec3 SetSat(const Vec3& c, double s) {
    const double mx = std::max(std::max(c.r, c.g), c.b);
    const double mn = std::min(std::min(c.r, c.g), c.b);
    if (!(mx > mn)) return Vec3{0.0, 0.0, 0.0};
    int imax = 0;
    while (c[imax] != mx) ++imax;  // FIRST index (r, g, b) equal to mx
    int imin = 0;
    while (c[imin] != mn) ++imin;  // FIRST index equal to mn (differs from imax since mx > mn)
    const int imid = 3 - imax - imin;
    Vec3 out;
    out[imax] = s;
    out[imid] = ((c[imid] - c[imin]) * s) / (c[imax] - c[imin]);
    out[imin] = 0.0;
    return out;
}

// ---- mode tables ---------------------------------------------------------------------------------

double blend_separable(BlendMode m, double cb, double cs, int bb, int sb) {
    switch (m) {
        case BlendMode::Norm:
        case BlendMode::Diss:
            return cs;
        case BlendMode::Mul:
            return Multiply(cb, cs);
        case BlendMode::Scrn:
            return Screen(cb, cs);
        case BlendMode::Over:
            return HardLight(cs, cb);
        case BlendMode::Dark:
            return std::min(cb, cs);
        case BlendMode::Lite:
            return std::max(cb, cs);
        case BlendMode::Div:
            return ColorDodge(cb, cs);
        case BlendMode::Idiv:
            return ColorBurn(cb, cs);
        case BlendMode::HLit:
            return HardLight(cb, cs);
        case BlendMode::SLit:
            return SoftLight(cb, cs);
        case BlendMode::Diff:
            return std::abs(cb - cs);
        case BlendMode::Smud:
            return (cb + cs) - ((2.0 * cb) * cs);
        case BlendMode::Lbrn:
            return std::max(0.0, (cb + cs) - 1.0);
        case BlendMode::Lddg:
            return std::min(1.0, cb + cs);
        case BlendMode::Fsub:
            return std::max(0.0, cb - cs);
        case BlendMode::Fdiv:
            if (mut::active(21)) {  // mutation 21: cs == 0 checked first, so B(0, 0) = 1
                if (cs == 0.0) return 1.0;
                if (cb == 0.0) return 0.0;
                return std::min(1.0, cb / cs);
            }
            if (cb == 0.0) return 0.0;
            if (cs == 0.0) return 1.0;
            return std::min(1.0, cb / cs);
        case BlendMode::VLit:
            if (cs <= 0.5) return ColorBurn(cb, 2.0 * cs);
            return ColorDodge(cb, (2.0 * cs) - 1.0);
        case BlendMode::LLit:
            return clamp01((cb + (2.0 * cs)) - 1.0);
        case BlendMode::PLit:
            if (cs <= 0.5) return std::min(cb, 2.0 * cs);
            return std::max(cb, (2.0 * cs) - 1.0);
        case BlendMode::HMix:
            if (mut::active(17)) return (sb + bb) > 255 ? 1.0 : 0.0;  // mutation 17
            return (sb + bb) >= 255 ? 1.0 : 0.0;
        default:
            return cs;  // vector modes are not handled here
    }
}

Vec3 blend_vector(BlendMode m, const Vec3& cb, const Vec3& cs, const int bb[3], const int sb[3]) {
    switch (m) {
        case BlendMode::DkCl:
        case BlendMode::LgCl: {
            const int Ls = ((30 * sb[0]) + (59 * sb[1])) + (11 * sb[2]);
            const int Lb = ((30 * bb[0]) + (59 * bb[1])) + (11 * bb[2]);
            bool pick_source = false;
            if (m == BlendMode::DkCl)
                pick_source = mut::active(18) ? (Ls <= Lb) : (Ls < Lb);  // mutation 18: ties -> source
            else
                pick_source = mut::active(18) ? (Ls >= Lb) : (Ls > Lb);
            return pick_source ? cs : cb;
        }
        case BlendMode::Hue:
            return SetLum(SetSat(cs, Sat(cb)), Lum(cb));
        case BlendMode::Sat:
            return SetLum(SetSat(cb, Sat(cs)), Lum(cb));
        case BlendMode::Colr:
            return SetLum(cs, Lum(cb));
        case BlendMode::Lum:
            return SetLum(cb, Lum(cs));
        default:
            return cs;
    }
}

Vec3 blend_clamped(BlendMode m, const Vec3& cb, const Vec3& cs, const int bb[3], const int sb[3]) {
    Vec3 out;
    if (is_vector_mode(m)) {
        out = blend_vector(m, cb, cs, bb, sb);
    } else {
        for (int ch = 0; ch < 3; ++ch) out[ch] = blend_separable(m, cb[ch], cs[ch], bb[ch], sb[ch]);
    }
    for (int ch = 0; ch < 3; ++ch) out[ch] = clamp01(out[ch]);
    return out;
}

}  // namespace rl::composite
