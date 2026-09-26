// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/doc/blend_mode.hpp"

namespace rl {

namespace {
constexpr const char* kNames[kBlendModeCount] = {
    "pass", "norm", "diss", "dark", "mul",  "idiv", "lbrn", "dkCl", "lite", "scrn",
    "div",  "lddg", "lgCl", "over", "sLit", "hLit", "vLit", "lLit", "pLit", "hMix",
    "diff", "smud", "fsub", "fdiv", "hue",  "sat",  "colr", "lum",
};
}  // namespace

const char* blend_mode_name(BlendMode m) { return kNames[static_cast<int>(m)]; }

std::optional<BlendMode> parse_blend_mode(std::string_view s) {
    for (int i = 0; i < kBlendModeCount; ++i)
        if (s == kNames[i]) return static_cast<BlendMode>(i);
    return std::nullopt;
}

bool is_vector_mode(BlendMode m) {
    switch (m) {
        case BlendMode::DkCl:
        case BlendMode::LgCl:
        case BlendMode::Hue:
        case BlendMode::Sat:
        case BlendMode::Colr:
        case BlendMode::Lum:
            return true;
        default:
            return false;
    }
}

}  // namespace rl
