// SPDX-License-Identifier: GPL-3.0-or-later
//
// PNG read/write through libpng: 8-bit RGBA, straight alpha. The writer emits only IHDR, IDAT and
// IEND (no gAMA/sRGB/iCCP/cHRM/tIME/text chunks), with fixed zlib settings, so the bytes depend on
// the pixels alone (BUILD-SPEC "render every script twice and cmp"). The reader applies no gamma or
// colour transform; it only expands formats to 8-bit RGBA.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/base/types.hpp"

namespace rl::io {

struct RgbaBuffer {
    int w = 0;
    int h = 0;
    std::vector<Rgba8> px;  // row-major, w * h

    Rgba8& at(int x, int y) { return px[(static_cast<size_t>(y) * static_cast<size_t>(w)) + static_cast<size_t>(x)]; }
    const Rgba8& at(int x, int y) const {
        return px[(static_cast<size_t>(y) * static_cast<size_t>(w)) + static_cast<size_t>(x)];
    }
};

// Supplies row y (w pixels); rows are requested in order 0..h-1. The pointer must stay valid until
// the next call. May throw; the partial file is then removed.
using RowProvider = std::function<const Rgba8*(int y)>;

// Writes atomically (to "<path>.tmp", then rename). Throws std::runtime_error on failure; no file
// is left at `path` then.
void write_png_rows(const std::string& path, int w, int h, const RowProvider& rows);
void write_png(const std::string& path, const RgbaBuffer& img);

// Encodes to memory (same bytes as write_png would produce).
std::vector<uint8_t> encode_png(const RgbaBuffer& img);
std::vector<uint8_t> encode_png_rows(int w, int h, const RowProvider& rows);

// Reads any PNG and converts it to 8-bit RGBA (palette/grey expanded, tRNS -> alpha, 16-bit scaled
// to 8-bit, missing alpha = 255). Pixels are returned as stored: NOT canonicalised. Throws
// std::runtime_error on failure.
RgbaBuffer read_png(const std::string& path);
RgbaBuffer decode_png(const std::vector<uint8_t>& bytes);

}  // namespace rl::io
