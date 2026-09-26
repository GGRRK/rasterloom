// SPDX-License-Identifier: GPL-3.0-or-later
//
// Free transform through a 3x3 homography and the shared resampling kernel
// (docs/math/30-geometry-selection.md §11 kernel + premultiplied mixing, §12 transform).
//
// Mutation hooks owned here: 11 (straight-alpha bicubic, every bicubic path), 30 (bicubic drops the
// -0.5 pixel-centre offset), 31 (inverse computed from the transposed matrix).
#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "core/tile/tiled_image.hpp"

namespace rl::transform {

// Row-major [m0 m1 m2; m3 m4 m5; m6 m7 m8] acting on column vectors (§12.1).
using Mat3 = std::array<double, 9>;

constexpr Mat3 kIdentity{1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};

// §12.1: C = A · B, each entry ((a0*b0) + (a1*b1)) + (a2*b2).
Mat3 mul(const Mat3& a, const Mat3& b);
double deg_to_rad(double deg);  // deg * (3.141592653589793 / 180.0)
Mat3 Tr(double tx, double ty);
Mat3 Sc(double sx, double sy);
Mat3 Ro(double deg);
Mat3 Sk(double kx, double ky);

// §12.2 params form: scale, then skew, then rotate, all about the pivot, then translate.
struct Params {
    double tx = 0.0, ty = 0.0;
    double sx = 1.0, sy = 1.0;
    double rotate = 0.0;  // degrees, clockwise on screen
    double kx = 0.0, ky = 0.0;  // skew degrees
    double px = 0.0, py = 0.0;  // pivot
};
Mat3 from_params(const Params& p);

// §12.2 quad form: source rect (rx, ry, rw, rh), destination corners TL, TR, BR, BL.
// Returns nullopt when den == 0 (an invalid script).
struct Quad {
    double rx = 0.0, ry = 0.0, rw = 1.0, rh = 1.0;
    std::array<std::array<double, 2>, 4> q{};
};
std::optional<Mat3> from_quad(const Quad& qd);

// §12.3 adjugate / determinant. nullopt when !(|det| >= 1e-12) (singular or NaN). Mutation 31:
// computed from the transpose of `m`.
std::optional<Mat3> inverse(const Mat3& m);

// §11.1 Keys cubic, a = -0.5, in Pillow's evaluation form.
double keys(double d);

enum class Interp { Bicubic, Nearest };

// §12.4: resamples a row-major sw x sh RGBA source into a dw x dh destination through the
// INVERSE matrix `inv` (destination -> source). Out-of-source taps are transparent.
std::vector<Rgba8> warp_rgba(const std::vector<Rgba8>& src, int sw, int sh, int dw, int dh, const Mat3& inv,
                             Interp interp);
// §11.4 grey variant (layer masks, rotate_canvas): taps dec(m), outside 1.0, bicubic only. A pixel
// whose source position fails the horizon/range test of §12.4 is 255 (doc 50 §4 reading).
std::vector<uint8_t> warp_gray(const std::vector<uint8_t>& src, int sw, int sh, int dw, int dh, const Mat3& inv);

// The same kernels reading straight from tiles and writing the result in 64-row bands: no dense
// copy of the source or the destination is ever made (the large-image path). `out_bg` is the
// result's background (tiles equal to it stay absent). Byte-identical to warp_rgba / warp_gray.
RgbaImage warp_rgba_tiled(const RgbaImage& src, int dw, int dh, const Mat3& inv, Interp interp);
GrayImage warp_gray_tiled(const GrayImage& src, int dw, int dh, const Mat3& inv, Gray8 out_bg);

// Free transform of one layer image (no history, no document): returns the canvas-sized result
// of mapping `src` through the FORWARD matrix `m`. nullopt when `m` is singular. This is the op's
// pixel path and also the GUI's live preview (apply to a copy, show it, commit via the op).
std::optional<RgbaImage> transform_image(const RgbaImage& src, const Mat3& m, Interp interp);

}  // namespace rl::transform
