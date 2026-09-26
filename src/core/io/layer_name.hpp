// SPDX-License-Identifier: GPL-3.0-or-later
//
// Layer names read from files (docs/math/60-editing-ops.md §13 "Files"): a name that breaks the
// §1.1 rule is sanitised by the reader, never rejected, and the import reports it with a warning.
//
//   1. decode: every ill-formed piece becomes U+FFFD - a lone UTF-16 surrogate (PSD `luni`), a
//      three-byte encoded surrogate ED A0..BF 80..BF (what an XML character reference to a
//      surrogate or a CESU-8 writer produces) as one U+FFFD, and every other maximal ill-formed
//      UTF-8 subpart (Unicode §3.9, "U+FFFD substitution of maximal subparts") as one U+FFFD;
//   2. drop every U+0000..U+001F and U+007F;
//   3. keep the first 255 code points.
// The result is always a valid name (§1.1) and is otherwise stored exactly (no trimming, no
// normalisation).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace rl::io {

inline constexpr size_t kMaxNameCodePoints = 255;

struct SanitizedName {
    std::string name;        // UTF-8, valid per doc 60 §1.1
    bool replaced = false;   // step 1 substituted U+FFFD
    bool dropped = false;    // step 2 removed control code points
    bool truncated = false;  // step 3 cut the name
    bool changed() const { return replaced || dropped || truncated; }
};

// A name given as (possibly ill-formed) UTF-8 bytes: ORA stack.xml attributes, document.json.
SanitizedName sanitize_layer_name(std::string_view utf8);
// A name given as UTF-16 code units (PSD `luni`), trailing NULs already removed by the caller.
SanitizedName sanitize_layer_name_utf16(const std::vector<uint16_t>& units);

// "<prefix>: layer name '<name>' was sanitised (<what>)" - the import warning for a changed name.
std::string sanitized_name_warning(const std::string& prefix, const SanitizedName& n);

// Makes JSON text parseable by a strict parser without losing anything else: every ill-formed
// UTF-8 piece becomes U+FFFD (as in step 1) and every `\uXXXX` escape of a lone surrogate inside a
// string becomes `�`. `changed` reports whether anything was replaced.
std::string repair_json_text(std::string_view text, bool& changed);

}  // namespace rl::io
