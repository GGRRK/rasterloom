// SPDX-License-Identifier: GPL-3.0-or-later
//
// Rasterloom's own PSD/PSB codec (BUILD-SPEC addendum 2026-09-26: not a PhotoshopAPI fork).
//
// Written from Adobe's public "Adobe Photoshop File Formats Specification"
// (https://www.adobe.com/devnet-apps/photoshop/fileformatashtml/). psd-tools 1.20 (MIT,
// https://github.com/psd-tools/psd-tools) was read as a format reference where the Adobe text is
// silent: tagged-block length padding (layer records: none; global blocks: 4, not counted in the
// length), the PSB 8-byte-length key list, the real-user-mask header placement, and the white
// matte of the merged composite. No code was copied.
//
// Reading: 8/16-bit RGB, Grayscale and CMYK (converted to 8-bit RGB; CMYK is stored inverted;
// 16 -> 8 bit is q(v / 65535), 00-conventions C2), layer tree (lsct/lsdk groups incl. pass-through),
// raster layers (raw / RLE / ZIP / ZIP-with-prediction channels), layer masks, clipping, blend keys,
// opacity, fill (iOpa), visibility, transparency lock, luni names, and adjustment layers mapped to
// the doc-20 types where the PSD settings are representable. Everything else - every tagged block
// the codec does not own, every image resource and global block - is kept byte-for-byte in the
// document's foreign data (core/doc/foreign.hpp) and written back on save. Text, smart-object and
// styled layers are imported as their stored (rasterised) pixels.
//
// Writing: PSD (PSB when either side > 30000 px, or when asked), layer tree, masks, clipping,
// groups, iOpa, luni, RLE channels, and the merged composite rendered by our compositor (the
// document background is not part of a PSD, so the merged image is RENDER(root) over transparent)
// as RLE planar R, G, B, A with a negative layer count (first extra channel = transparency), plus
// IRB 1005 (resolution, unless one was read), 1036 (JPEG thumbnail) and 1057 (versionInfo,
// hasRealMergedData = 1).
//
// Merged-image colour convention: Photoshop stores the colour channels of a composite that has
// transparency matted over white; readers (psd-tools `_remove_white_background`, KImageFormats)
// undo that. We store c' = q(c/255 * a/255 + (1 - a/255)) (io::matte_white) and the straight alpha.
//
// Foreign-data namespaces used on nodes and the document:
//   psd.tb / psd.tb64   layer tagged blocks not owned by the codec (signature 8BIM / 8B64), in file order
//   psd.div             tagged blocks of a group's closing divider record (same encoding: "8BIM"+data)
//   psd.gtb / psd.gtb64 global tagged blocks
//   psd.irb             image resources, key = decimal id, data = the whole resource block
//   psd.glmi            global layer mask info
//   psd.own             codec bookkeeping (record flags, blending ranges, mask tail, raw real-mask
//                       channel, lsct/lspf payloads, pascal name) - not a block of its own
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/doc/document.hpp"

namespace rl::psd {

constexpr int kPsdMaxSide = 30000;
constexpr int kPsbMaxSide = 300000;

// True when `bytes` starts with the PSD/PSB signature.
bool is_psd(const std::vector<uint8_t>& bytes);

// Parses a PSD/PSB file. Throws io::IoError on malformed or unsupported input. Conversions and
// anything dropped are reported in `warnings`.
DocState read(const std::vector<uint8_t>& bytes, std::vector<std::string>& warnings);

struct WriteOptions {
    bool force_psb = false;  // write version 2 even when both sides are <= 30000
};

// Serialises a document. Throws io::IoError when the document cannot be written (a side above
// 300000 px).
std::vector<uint8_t> write(const DocState& s, const WriteOptions& opt, std::vector<std::string>& warnings);

// PSD blend-mode keys (4 bytes, with the trailing spaces): "norm", "mul ", "pass", ...
const char* blend_key(BlendMode m);
std::optional<BlendMode> mode_from_key(const std::string& key);

// PackBits (Apple TN1023 / the PSD "RLE" compression). decode returns false on malformed input
// or when the output would not be exactly `out_len` bytes.
void packbits_encode(const uint8_t* src, size_t n, std::vector<uint8_t>& out);
bool packbits_decode(const uint8_t* src, size_t n, uint8_t* out, size_t out_len);

}  // namespace rl::psd
