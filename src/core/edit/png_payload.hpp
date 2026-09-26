// SPDX-License-Identifier: GPL-3.0-or-later
//
// The `png` payload of `place_image` (docs/math/60-editing-ops.md §14.1-§14.2): strict base64 and
// the exact PNG subset (8-bit RGBA, non-interlaced) the doc defines, decoded with zlib only, so the
// accept/reject rules are the doc's and not a general-purpose PNG library's.
//
// encode_payload() is what the GUI puts into a place_image op: the core PNG encoder's bytes
// (always 8-bit RGBA, colour type 6, no interlace), base64-encoded.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/io/png.hpp"

namespace rl::edit {

// §14.1. Returns false (and a reason in `err`) when `s` is not canonical-alphabet padded base64.
bool decode_base64(const std::string& s, std::vector<uint8_t>& out, std::string& err);
std::string encode_base64(const uint8_t* data, size_t n);

// §14.2. Throws std::runtime_error with a short reason when `bytes` is not the PNG subset.
io::RgbaBuffer decode_png_payload(const std::vector<uint8_t>& bytes);

// Both steps; throws std::runtime_error.
io::RgbaBuffer decode_payload(const std::string& b64);

// base64(PNG of `img`), the form place_image accepts. Pixels are stored as given (canonicalise
// first when the source might hold colour under alpha 0; place_image canonicalises anyway).
std::string encode_payload(const io::RgbaBuffer& img);

}  // namespace rl::edit
