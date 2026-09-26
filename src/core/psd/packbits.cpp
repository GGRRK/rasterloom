// SPDX-License-Identifier: GPL-3.0-or-later
//
// PackBits, from Apple Technical Note TN1023 and the PSD specification's "RLE compressed" image
// data: a header byte n; 0..127 = copy the next n+1 bytes literally; -1..-127 = repeat the next byte
// 1-n times; -128 = no-op. The encoder is deterministic: runs of 3 or more equal bytes become
// repeat packets (runs of exactly 2 inside a literal stay literal), literals are at most 128 bytes.
#include "core/psd/psd.hpp"

namespace rl::psd {

void packbits_encode(const uint8_t* src, size_t n, std::vector<uint8_t>& out) {
    size_t i = 0;
    while (i < n) {
        // Length of the run starting at i.
        size_t run = 1;
        while (i + run < n && run < 128 && src[i + run] == src[i]) ++run;
        if (run >= 3) {
            out.push_back(static_cast<uint8_t>(static_cast<int8_t>(1 - static_cast<int>(run))));
            out.push_back(src[i]);
            i += run;
            continue;
        }
        // Literal: extend until a run of 3 starts or 128 bytes.
        const size_t start = i;
        size_t len = 0;
        while (i < n && len < 128) {
            if (i + 2 < n && src[i] == src[i + 1] && src[i] == src[i + 2]) break;
            ++i;
            ++len;
        }
        out.push_back(static_cast<uint8_t>(len - 1));
        out.insert(out.end(), src + start, src + start + len);
    }
}

bool packbits_decode(const uint8_t* src, size_t n, uint8_t* out, size_t out_len) {
    size_t i = 0, o = 0;
    while (i < n && o < out_len) {
        const int h = static_cast<int8_t>(src[i++]);
        if (h >= 0) {
            const size_t k = static_cast<size_t>(h) + 1;
            if (i + k > n || o + k > out_len) return false;
            for (size_t j = 0; j < k; ++j) out[o++] = src[i++];
        } else if (h != -128) {
            const size_t k = static_cast<size_t>(1 - h);
            if (i >= n || o + k > out_len) return false;
            const uint8_t v = src[i++];
            for (size_t j = 0; j < k; ++j) out[o++] = v;
        }
    }
    return o == out_len;
}

}  // namespace rl::psd
