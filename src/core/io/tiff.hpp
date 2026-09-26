// SPDX-License-Identifier: GPL-3.0-or-later
//
// 8-bit TIFF through libtiff. The writer is deterministic: one strip per 64 rows, LZW with
// horizontal differencing, RGB (opaque images) or RGBA with ExtraSamples = unassociated alpha,
// no DateTime / Software / host tags. The reader accepts 8-bit greyscale (min-is-black/white),
// RGB and RGBA (unassociated or associated alpha), contiguous or planar, and falls back to
// libtiff's RGBA interface (un-premultiplied) for anything else (palette, 16-bit, CMYK, YCbCr, ...).
#pragma once

#include <cstdint>
#include <vector>

#include "core/io/png.hpp"

namespace rl::io {

std::vector<uint8_t> encode_tiff(const RgbaBuffer& img);
RgbaBuffer decode_tiff(const std::vector<uint8_t>& bytes);
bool is_tiff(const std::vector<uint8_t>& bytes);

}  // namespace rl::io
