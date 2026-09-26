// SPDX-License-Identifier: GPL-3.0-or-later
//
// Internals shared by the PSD reader and writer.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/doc/node.hpp"

namespace rl::psd::detail {

// Tagged-block keys whose length field is 8 bytes in PSB (psd-tools TaggedBlock._BIG_KEYS; the
// Adobe spec lists a subset). Everything else uses 4 bytes in both versions.
bool is_big_key(const std::string& key);

// Layer tagged blocks the codec interprets and regenerates on save. Every other layer block is
// foreign data (kept verbatim).
bool is_owned_layer_key(const std::string& key);

// Global tagged blocks that are consumed on read and never written back as-is (16/32-bit layer
// info, merged-transparency markers).
bool is_consumed_global_key(const std::string& key);

// Adjustment-layer keys (the Adobe spec's adjustment "Additional Layer Information" list, fill
// layers excluded: SoCo/GdFl/PtFl hold pixels of their own).
bool is_adjustment_key(const std::string& key);

// UTF-8 <-> UTF-16 (code units); invalid UTF-8 bytes become U+FFFD.
std::vector<uint16_t> utf8_to_utf16(const std::string& s);
std::string utf16_to_utf8(const std::vector<uint16_t>& u);
// Latin-1 <-> UTF-8 (the pascal layer name; Mac Roman bytes >= 0x80 are read as Latin-1, a
// documented approximation used only when a layer has no `luni` block).
std::string latin1_to_utf8(const std::string& s);
std::string utf8_to_latin1(const std::string& s);  // '?' for code points above U+00FF

// Adjustment block <-> doc-20 type + params JSON. from_psd returns false when the PSD settings
// are not representable by a doc-20 adjustment.
bool adjustment_from_psd(const std::string& key, const std::vector<uint8_t>& data, std::string& type,
                         std::string& params_json);
// Authors a PSD adjustment block for a node created in Rasterloom. Returns false when the
// adjustment cannot be expressed in PSD (it is then written as an empty layer, with a warning).
bool adjustment_to_psd(const std::string& type, const std::string& params_json, std::string& key,
                       std::vector<uint8_t>& data);

}  // namespace rl::psd::detail
