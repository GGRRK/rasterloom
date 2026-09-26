// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/edit/names.hpp"

#include <cstdint>

namespace rl::edit {

namespace {

// Decodes one UTF-8 scalar value at s[i]; advances i. Returns false on any ill-formed sequence,
// including surrogate code points (U+D800..U+DFFF) and overlong encodings.
bool next_scalar(const std::string& s, size_t& i, uint32_t& cp) {
    const auto byte = [&](size_t k) { return static_cast<unsigned char>(s[k]); };
    const unsigned char b0 = byte(i);
    size_t len = 0;
    uint32_t min = 0;
    if (b0 < 0x80) {
        cp = b0;
        i += 1;
        return true;
    } else if ((b0 & 0xE0) == 0xC0) {
        len = 2;
        cp = b0 & 0x1Fu;
        min = 0x80;
    } else if ((b0 & 0xF0) == 0xE0) {
        len = 3;
        cp = b0 & 0x0Fu;
        min = 0x800;
    } else if ((b0 & 0xF8) == 0xF0) {
        len = 4;
        cp = b0 & 0x07u;
        min = 0x10000;
    } else {
        return false;
    }
    if (i + len > s.size()) return false;
    for (size_t k = 1; k < len; ++k) {
        const unsigned char b = byte(i + k);
        if ((b & 0xC0) != 0x80) return false;
        cp = (cp << 6) | (b & 0x3Fu);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
    i += len;
    return true;
}

}  // namespace

size_t utf8_scalar_count(const std::string& s) {
    size_t n = 0;
    uint32_t cp = 0;
    for (size_t i = 0; i < s.size();) {
        if (!next_scalar(s, i, cp)) return SIZE_MAX;
        ++n;
    }
    return n;
}

std::string name_problem(const std::string& s) {
    size_t n = 0;
    uint32_t cp = 0;
    for (size_t i = 0; i < s.size();) {
        if (!next_scalar(s, i, cp)) return "is not well-formed UTF-8 of Unicode scalar values";
        if (cp <= 0x1F || cp == 0x7F) return "contains a control character (U+0000..U+001F or U+007F)";
        ++n;
    }
    if (n > kMaxNameCodePoints)
        return "is " + std::to_string(n) + " code points long (at most " + std::to_string(kMaxNameCodePoints) +
               ")";
    return {};
}

bool is_valid_name(const std::string& s) { return name_problem(s).empty(); }

}  // namespace rl::edit
