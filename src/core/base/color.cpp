// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/base/color.hpp"

namespace rl {

namespace {
int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
}  // namespace

std::optional<Rgba8> parse_hex_color(std::string_view s) {
    if (s.size() != 7 && s.size() != 9) return std::nullopt;
    if (s[0] != '#') return std::nullopt;
    uint8_t v[4] = {0, 0, 0, 255};
    const size_t n = (s.size() - 1) / 2;
    for (size_t i = 0; i < n; ++i) {
        const int hi = hexval(s[1 + (2 * i)]);
        const int lo = hexval(s[2 + (2 * i)]);
        if (hi < 0 || lo < 0) return std::nullopt;
        v[i] = static_cast<uint8_t>((hi * 16) + lo);
    }
    return Rgba8{v[0], v[1], v[2], v[3]};
}

}  // namespace rl
