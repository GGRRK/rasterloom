// SPDX-License-Identifier: GPL-3.0-or-later
//
// JPEG through libjpeg-turbo. The writer is deterministic: baseline, 4:2:0 subsampling, ISLOW DCT,
// standard Huffman tables, a JFIF APP0 with 1:1 density and no other markers (no timestamps, no
// EXIF). JPEG has no alpha: callers matte first (io::matte_white). The reader converts greyscale,
// YCbCr, RGB, CMYK and YCCK (Adobe-inverted CMYK) to 8-bit RGBA with alpha 255.
#pragma once

#include <cstdint>
#include <vector>

#include "core/io/png.hpp"

namespace rl::io {

constexpr int kDefaultJpegQuality = 92;

// Ignores alpha. Throws IoError.
std::vector<uint8_t> encode_jpeg(const RgbaBuffer& img, int quality = kDefaultJpegQuality);
// Throws IoError.
RgbaBuffer decode_jpeg(const std::vector<uint8_t>& bytes);
bool is_jpeg(const std::vector<uint8_t>& bytes);

}  // namespace rl::io
