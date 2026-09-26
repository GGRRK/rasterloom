// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/io/layer_name.hpp"

namespace rl::io {

namespace {

constexpr char32_t kReplacement = 0xFFFD;

void put_utf8(std::string& o, char32_t cp) {
    if (cp < 0x80) {
        o.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        o.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        o.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        o.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        o.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        o.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        o.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        o.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        o.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        o.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

bool in(unsigned char c, unsigned char lo, unsigned char hi) { return c >= lo && c <= hi; }

// Decodes one code point at s[i]; advances i. Well-formed sequences follow Unicode Table 3-7.
char32_t decode_one(std::string_view s, size_t& i, bool& replaced) {
    const auto b = [&](size_t k) { return static_cast<unsigned char>(s[k]); };
    const unsigned char c = b(i);
    if (c < 0x80) {
        ++i;
        return c;
    }
    size_t need = 0;
    unsigned char lo = 0x80, hi = 0xBF;  // allowed range of the second byte
    char32_t cp = 0;
    if (in(c, 0xC2, 0xDF)) {
        need = 1;
        cp = c & 0x1Fu;
    } else if (in(c, 0xE0, 0xEF)) {
        need = 2;
        cp = c & 0x0Fu;
        if (c == 0xE0) lo = 0xA0;
        if (c == 0xED) {
            // An encoded surrogate (ED A0..BF 80..BF) is one lone surrogate: one U+FFFD.
            if (i + 2 < s.size() && in(b(i + 1), 0xA0, 0xBF) && in(b(i + 2), 0x80, 0xBF)) {
                i += 3;
                replaced = true;
                return kReplacement;
            }
            hi = 0x9F;
        }
    } else if (in(c, 0xF0, 0xF4)) {
        need = 3;
        cp = c & 0x07u;
        if (c == 0xF0) lo = 0x90;
        if (c == 0xF4) hi = 0x8F;
    } else {  // 80..C1, F5..FF: never valid anywhere
        ++i;
        replaced = true;
        return kReplacement;
    }
    size_t k = i + 1;
    for (size_t n = 0; n < need; ++n, ++k) {
        const bool ok = k < s.size() && (n == 0 ? in(b(k), lo, hi) : in(b(k), 0x80, 0xBF));
        if (!ok) {  // the maximal subpart s[i, k) becomes one U+FFFD
            i = k;
            replaced = true;
            return kReplacement;
        }
        cp = (cp << 6) | (b(k) & 0x3Fu);
    }
    i = k;
    return cp;
}

SanitizedName finish(const std::u32string& cps, bool replaced) {
    SanitizedName r;
    r.replaced = replaced;
    size_t count = 0;
    for (char32_t cp : cps) {
        if (cp <= 0x1F || cp == 0x7F) {
            r.dropped = true;
            continue;
        }
        if (count == kMaxNameCodePoints) {
            r.truncated = true;
            break;
        }
        put_utf8(r.name, cp);
        ++count;
    }
    return r;
}

}  // namespace

SanitizedName sanitize_layer_name(std::string_view utf8) {
    std::u32string cps;
    bool replaced = false;
    for (size_t i = 0; i < utf8.size();) cps.push_back(decode_one(utf8, i, replaced));
    return finish(cps, replaced);
}

SanitizedName sanitize_layer_name_utf16(const std::vector<uint16_t>& u) {
    std::u32string cps;
    bool replaced = false;
    for (size_t i = 0; i < u.size(); ++i) {
        char32_t cp = u[i];
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < u.size() && u[i + 1] >= 0xDC00 && u[i + 1] < 0xE000) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (u[i + 1] - 0xDC00u);
            ++i;
        } else if (cp >= 0xD800 && cp < 0xE000) {
            cp = kReplacement;
            replaced = true;
        }
        cps.push_back(cp);
    }
    return finish(cps, replaced);
}

std::string sanitized_name_warning(const std::string& prefix, const SanitizedName& n) {
    std::string what;
    auto add = [&](const char* s) {
        if (!what.empty()) what += ", ";
        what += s;
    };
    if (n.replaced) add("invalid characters replaced with U+FFFD");
    if (n.dropped) add("control characters removed");
    if (n.truncated) add("cut to 255 characters");
    return prefix + ": layer name '" + n.name + "' was sanitised (" + what + ")";
}

std::string repair_json_text(std::string_view text, bool& changed) {
    changed = false;
    // Pass 1: well-formed UTF-8.
    std::string t;
    t.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < 0x80) {
            t.push_back(static_cast<char>(c));
            ++i;
            continue;
        }
        bool replaced = false;
        const size_t start = i;
        const char32_t cp = decode_one(text, i, replaced);
        if (replaced) {
            changed = true;
            put_utf8(t, cp);
        } else {
            t.append(text.substr(start, i - start));
        }
    }
    // Pass 2: lone surrogate escapes inside strings.
    auto hex4 = [&](size_t at, uint32_t& v) {
        if (at + 4 > t.size()) return false;
        v = 0;
        for (size_t k = at; k < at + 4; ++k) {
            const char h = t[k];
            v <<= 4;
            if (h >= '0' && h <= '9') v |= static_cast<uint32_t>(h - '0');
            else if (h >= 'a' && h <= 'f') v |= static_cast<uint32_t>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') v |= static_cast<uint32_t>(h - 'A' + 10);
            else return false;
        }
        return true;
    };
    std::string o;
    o.reserve(t.size());
    bool in_string = false;
    for (size_t i = 0; i < t.size(); ++i) {
        const char c = t[i];
        if (!in_string) {
            if (c == '"') in_string = true;
            o.push_back(c);
            continue;
        }
        if (c == '"') {
            in_string = false;
            o.push_back(c);
            continue;
        }
        if (c != '\\' || i + 1 >= t.size()) {
            o.push_back(c);
            continue;
        }
        uint32_t v = 0;
        if (t[i + 1] != 'u' || !hex4(i + 2, v) || v < 0xD800 || v > 0xDFFF) {
            o.push_back(c);  // any other escape: copy the backslash and the escaped character
            o.push_back(t[i + 1]);
            ++i;
            continue;
        }
        uint32_t w = 0;
        if (v < 0xDC00 && i + 11 < t.size() && t[i + 6] == '\\' && t[i + 7] == 'u' && hex4(i + 8, w) &&
            w >= 0xDC00 && w <= 0xDFFF) {
            o.append(t, i, 12);  // a proper pair
            i += 11;
            continue;
        }
        o += "\\uFFFD";
        changed = true;
        i += 5;
    }
    return o;
}

}  // namespace rl::io
