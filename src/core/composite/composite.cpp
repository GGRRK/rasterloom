// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/composite/composite.hpp"

#include <cmath>

#include "core/base/mutation.hpp"
#include "core/base/quant.hpp"
#include "core/base/rng.hpp"
#include "core/composite/blend.hpp"

namespace rl::composite {

namespace {

// Mutation 3 only: the sRGB transfer functions (never used by the correct path).
double srgb_to_linear(double c) {
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}
double linear_to_srgb(double c) {
    return c <= 0.0031308 ? c * 12.92 : (1.055 * std::pow(c, 1.0 / 2.4)) - 0.055;
}

}  // namespace

double dissolve_coverage(double c, uint64_t seed, uint32_t x, uint32_t y) {
    return unit(pixel_hash(seed, x, y, 5)) < c ? 1.0 : 0.0;
}

Rgba8 composite_px(Rgba8 bd, Rgba8 src, double c, BlendMode mode, uint64_t seed, uint32_t x, uint32_t y) {
    // 1. Dissolve
    if (mode == BlendMode::Diss) {
        c = dissolve_coverage(c, seed, x, y);
        mode = BlendMode::Norm;
    }
    // 2.
    const double ab = dec(bd.a);
    const double as = c;
    // 3.
    const double ao = as + (ab * (1.0 - as));
    if (ao == 0.0) return Rgba8{};
    // 4.
    Vec3 cb{dec(bd.r), dec(bd.g), dec(bd.b)};
    Vec3 cs{dec(src.r), dec(src.g), dec(src.b)};
    const bool linear = mut::active(3);
    if (linear) {
        for (int ch = 0; ch < 3; ++ch) {
            cb[ch] = srgb_to_linear(cb[ch]);
            cs[ch] = srgb_to_linear(cs[ch]);
        }
    }
    const bool normal = (mode == BlendMode::Norm);
    Vec3 bl;
    if (!normal) {
        if (mut::active(2)) {
            // Mutation 2: B on premultiplied operands. Integer modes see the re-quantised operands.
            Vec3 pb{ab * cb.r, ab * cb.g, ab * cb.b};
            Vec3 ps{as * cs.r, as * cs.g, as * cs.b};
            const int bb[3] = {q(pb.r), q(pb.g), q(pb.b)};
            const int sb[3] = {q(ps.r), q(ps.g), q(ps.b)};
            bl = blend_clamped(mode, pb, ps, bb, sb);
        } else if (linear) {
            const int bb[3] = {q(cb.r), q(cb.g), q(cb.b)};
            const int sb[3] = {q(cs.r), q(cs.g), q(cs.b)};
            bl = blend_clamped(mode, cb, cs, bb, sb);
        } else {
            const int bb[3] = {bd.r, bd.g, bd.b};
            const int sb[3] = {src.r, src.g, src.b};
            bl = blend_clamped(mode, cb, cs, bb, sb);
        }
    }
    // 5.
    double co[3];
    for (int ch = 0; ch < 3; ++ch) {
        double mixed = 0.0;
        if (normal)
            mixed = cs[ch];
        else
            mixed = ((1.0 - ab) * cs[ch]) + (ab * bl[ch]);
        const double t1 = as * mixed;
        const double t2 = (ab * cb[ch]) * (1.0 - as);
        co[ch] = (t1 + t2) / ao;
        if (linear) co[ch] = linear_to_srgb(clamp01(co[ch]));
    }
    // 6.
    Rgba8 out{q(co[0]), q(co[1]), q(co[2]), q(ao)};
    if (mut::active(22)) return out;  // mutation 22: canonicalisation skipped
    return canonicalize(out);
}

Rgba8 adjust_px(Rgba8 bd, const adjust::Adjustment& adj, double c, BlendMode mode, uint64_t seed, uint32_t x,
                uint32_t y) {
    // 1.
    if (bd.a == 0) return Rgba8{};
    // 2.
    if (mode == BlendMode::Diss) {
        c = dissolve_coverage(c, seed, x, y);
        mode = BlendMode::Norm;
    }
    // 3.
    uint8_t ra = bd.r, ga = bd.g, ba = bd.b;
    adj.apply(ra, ga, ba);
    const Vec3 cb{dec(bd.r), dec(bd.g), dec(bd.b)};
    const Vec3 cs{dec(ra), dec(ga), dec(ba)};
    Vec3 bl;
    if (mode == BlendMode::Norm) {
        bl = cs;
    } else {
        const int bb[3] = {bd.r, bd.g, bd.b};
        const int sb[3] = {ra, ga, ba};
        bl = blend_clamped(mode, cb, cs, bb, sb);
    }
    // 4.
    double co[3];
    for (int ch = 0; ch < 3; ++ch) co[ch] = ((1.0 - c) * cb[ch]) + (c * bl[ch]);
    // 5. alpha byte unchanged (non-zero here, so canonicalisation is a no-op)
    return canonicalize(Rgba8{q(co[0]), q(co[1]), q(co[2]), bd.a});
}

}  // namespace rl::composite
