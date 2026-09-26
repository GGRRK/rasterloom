// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/io/image_util.hpp"

#include "core/base/quant.hpp"

namespace rl::io {

ThumbAccumulator::ThumbAccumulator(int w, int h, int max_side) : w_(w), h_(h), tw_(w), th_(h) {
    if (w > max_side || h > max_side) {
        if (w >= h) {
            tw_ = max_side;
            th_ = static_cast<int>(std::max<int64_t>(1, ((static_cast<int64_t>(h) * max_side) + (w / 2)) / w));
        } else {
            th_ = max_side;
            tw_ = static_cast<int>(std::max<int64_t>(1, ((static_cast<int64_t>(w) * max_side) + (h / 2)) / h));
        }
    }
    // Output pixel o covers source [floor(o*S/T), floor((o+1)*S/T)); for S >= T these partition S.
    auto map = [](int S, int T, std::vector<int>& of, std::vector<int>& cnt) {
        of.assign(static_cast<size_t>(S), 0);
        cnt.assign(static_cast<size_t>(T), 0);
        for (int o = 0; o < T; ++o) {
            const int a = static_cast<int>(static_cast<int64_t>(o) * S / T);
            const int b = static_cast<int>(static_cast<int64_t>(o + 1) * S / T);
            for (int i = a; i < b; ++i) of[static_cast<size_t>(i)] = o;
            cnt[static_cast<size_t>(o)] = b - a;
        }
    };
    map(w_, tw_, ox_of_x_, nx_);
    map(h_, th_, oy_of_y_, ny_);
    acc_.assign(static_cast<size_t>(tw_) * static_cast<size_t>(th_) * 4, 0.0);
}

void ThumbAccumulator::add_row(int y, const Rgba8* row) {
    const size_t oy = static_cast<size_t>(oy_of_y_[static_cast<size_t>(y)]);
    double* base = acc_.data() + (oy * static_cast<size_t>(tw_) * 4);
    for (int x = 0; x < w_; ++x) {
        const Rgba8 p = row[x];
        if (p.a == 0) continue;
        double* o = base + (static_cast<size_t>(ox_of_x_[static_cast<size_t>(x)]) * 4);
        const double a = dec(p.a);
        o[0] += dec(p.r) * a;
        o[1] += dec(p.g) * a;
        o[2] += dec(p.b) * a;
        o[3] += a;
    }
}

RgbaBuffer ThumbAccumulator::finish() const {
    RgbaBuffer out;
    out.w = tw_;
    out.h = th_;
    out.px.resize(static_cast<size_t>(tw_) * static_cast<size_t>(th_));
    for (int oy = 0; oy < th_; ++oy)
        for (int ox = 0; ox < tw_; ++ox) {
            const double* o = acc_.data() + (((static_cast<size_t>(oy) * static_cast<size_t>(tw_)) + static_cast<size_t>(ox)) * 4);
            const double n = static_cast<double>(nx_[static_cast<size_t>(ox)]) * static_cast<double>(ny_[static_cast<size_t>(oy)]);
            Rgba8 p{};
            if (o[3] > 0.0) {
                p.r = q(o[0] / o[3]);
                p.g = q(o[1] / o[3]);
                p.b = q(o[2] / o[3]);
            }
            p.a = q(o[3] / n);
            out.at(ox, oy) = canonicalize(p);
        }
    return out;
}

RgbaBuffer downscale_fit(const RgbaBuffer& src, int max_side) {
    if (src.w <= 0 || src.h <= 0) return RgbaBuffer{};
    if (src.w <= max_side && src.h <= max_side) return src;
    ThumbAccumulator acc(src.w, src.h, max_side);
    for (int y = 0; y < src.h; ++y) acc.add_row(y, src.px.data() + (static_cast<size_t>(y) * static_cast<size_t>(src.w)));
    return acc.finish();
}

Rgba8 matte_white(Rgba8 p) {
    const double a = dec(p.a);
    auto ch = [a](uint8_t c) { return q((dec(c) * a) + (1.0 - a)); };
    return Rgba8{ch(p.r), ch(p.g), ch(p.b), 255};
}

}  // namespace rl::io
