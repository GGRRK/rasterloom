// SPDX-License-Identifier: GPL-3.0-or-later
//
// B1 Gaussian Blur: three box passes per axis on exact-integer premultiplied planes. Each 1-D pass
// is a running sum (O(1) per pixel in the radius); every pass rounds, (S + r) // w, exactly as the
// direct sum would, because the sums are exact integers.
#include <algorithm>
#include <cmath>

#include "core/base/mutation.hpp"
#include "core/base/quant.hpp"
#include "core/filters/filters.hpp"
#include "core/tile/memory.hpp"

namespace rl::filters {

std::array<int, 3> box_widths(double sigma) {
    const double s = sigma;
    const double wi = std::sqrt((((12.0 * s) * s) / 3.0) + 1.0);
    int64_t wl = static_cast<int64_t>(std::floor(wi));
    if (wl % 2 == 0) wl = wl - 1;
    const int64_t wu = wl + 2;
    const double mi = (((((12.0 * s) * s) - static_cast<double>(3 * wl * wl)) - static_cast<double>(12 * wl)) - 9.0) /
                      ((-4.0 * static_cast<double>(wl)) - 4.0);
    const double m = clamp(rhaz(mi), 0.0, 3.0);
    std::array<int, 3> widths{};
    for (int j = 0; j < 3; ++j) widths[static_cast<size_t>(j)] = static_cast<int>((j < m) ? wl : wu);
    return widths;
}

namespace {

// Plane samples: premultiplied channel bytes c * a (or a * 255), at most 255 * 255 = 65025, so they
// fit a uint16 exactly; a box average of such values never exceeds their maximum. The running sums
// are int64 as before, so every pass rounds exactly as the direct sum would.
using Sample = uint16_t;

// One 1-D box pass of odd width w over `n` samples at `base`, `stride` apart, in place.
// out[i] = (sum_{j=i-r..i+r} E(j) + r) // w. `tmp` is scratch of size >= n.
void box_line(Sample* base, size_t stride, int n, int w, Edge edge, std::vector<Sample>& tmp) {
    if (w == 1) return;
    const int r = (w - 1) / 2;
    for (int i = 0; i < n; ++i) tmp[static_cast<size_t>(i)] = base[static_cast<size_t>(i) * stride];
    auto E = [&](int j) -> int64_t {
        if (j < 0 || j >= n) {
            if (edge == Edge::Transparent) return 0;
            j = std::min(std::max(j, 0), n - 1);
        }
        return tmp[static_cast<size_t>(j)];
    };
    int64_t S = 0;
    for (int j = -r; j <= r; ++j) S += E(j);
    for (int i = 0; i < n; ++i) {
        base[static_cast<size_t>(i) * stride] = static_cast<Sample>((S + r) / w);
        S += E(i + r + 1) - E(i - r);
    }
}

void box_pass(std::vector<Sample>& plane, int W, int H, int w, bool vertical, Edge edge, std::vector<Sample>& tmp) {
    if (!vertical) {
        for (int y = 0; y < H; ++y) box_line(plane.data() + (static_cast<size_t>(y) * W), 1, W, w, edge, tmp);
    } else {
        for (int x = 0; x < W; ++x) box_line(plane.data() + x, static_cast<size_t>(W), H, w, edge, tmp);
    }
}

// Stages 2 and 3 on one plane. Mutation 12: all vertical passes before all horizontal passes.
void blur_plane(std::vector<Sample>& p, int W, int H, const std::array<int, 3>& widths, Edge edge,
                std::vector<Sample>& tmp) {
    const bool v_first = mut::active(12);
    for (int stage = 0; stage < 2; ++stage) {
        const bool vertical = (stage == 0) == v_first;
        for (int w : widths) box_pass(p, W, H, w, vertical, edge, tmp);
    }
}

}  // namespace

// The four channels are independent until stage 4, so they are blurred one plane at a time: the
// alpha plane first (stage 4 needs it for every colour channel), then each colour plane in one
// reused buffer, unpremultiplied straight into the output. Two uint16 planes instead of four
// int32 planes (1/4 of the former working memory); every value is bit-identical.
Image gaussian_blur(const Image& src, double radius, Edge edge) {
    const int W = src.w, H = src.h;
    const size_t N = src.px.size();
    const std::array<int, 3> widths = box_widths(radius);
    const bool m29 = mut::active(29);
    mem::Reservation hold(static_cast<uint64_t>(N) * (2 * sizeof(Sample) + sizeof(Rgba8)), "gaussian blur planes");

    std::vector<Sample> tmp(static_cast<size_t>(std::max(W, H)));
    // Stage 1 + 2/3 for alpha: pa = a * 255.
    std::vector<Sample> A(N);
    for (size_t i = 0; i < N; ++i) A[i] = static_cast<Sample>(src.px[i].a * 255);
    blur_plane(A, W, H, widths, edge, tmp);

    Image out(W, H);
    std::vector<Sample> C(N);
    for (int c = 0; c < 3; ++c) {
        // Stage 1 for colour c. Mutation 29: colour not multiplied by alpha.
        for (size_t i = 0; i < N; ++i) {
            const Rgba8 px = src.px[i];
            const int32_t v = (c == 0) ? px.r : (c == 1) ? px.g : px.b;
            C[i] = static_cast<Sample>(v * (m29 ? 255 : static_cast<int32_t>(px.a)));
        }
        blur_plane(C, W, H, widths, edge, tmp);
        // Stage 4 for colour c.
        for (size_t i = 0; i < N; ++i) {
            const int32_t pa = A[i];
            double Cv;
            if (m29)
                Cv = static_cast<double>(C[i]) / 65025.0;
            else
                Cv = (pa > 0) ? (static_cast<double>(C[i]) / static_cast<double>(pa)) : 0.0;
            uint8_t* ch = (c == 0) ? &out.px[i].r : (c == 1) ? &out.px[i].g : &out.px[i].b;
            *ch = q(Cv);
        }
    }
    // Stage 4 alpha, then canonicalise the whole pixel.
    for (size_t i = 0; i < N; ++i) {
        const double a = static_cast<double>(A[i]) / 65025.0;
        out.px[i].a = q(a);
        out.px[i] = canonicalize(out.px[i]);
    }
    return out;
}

}  // namespace rl::filters
