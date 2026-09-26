// SPDX-License-Identifier: GPL-3.0-or-later
//
// The 27 blend modes plus Pass Through (docs/math/10-compositing.md §3.1). JSON spellings are the
// PSD blend-mode keys with trailing spaces removed, case-sensitive.
#pragma once

#include <optional>
#include <string_view>

namespace rl {

enum class BlendMode : unsigned char {
    Pass,  // groups only
    Norm,
    Diss,
    Dark,
    Mul,
    Idiv,
    Lbrn,
    DkCl,
    Lite,
    Scrn,
    Div,
    Lddg,
    LgCl,
    Over,
    SLit,
    HLit,
    VLit,
    LLit,
    PLit,
    HMix,
    Diff,
    Smud,
    Fsub,
    Fdiv,
    Hue,
    Sat,
    Colr,
    Lum,
};

constexpr int kBlendModeCount = 28;  // Pass + 27 modes

// JSON name of a mode ("pass", "norm", "mul", ...).
const char* blend_mode_name(BlendMode m);

// Parses a §3.1 JSON mode string (not "isolated"). nullopt for any other string, including a PSD
// key with its trailing space ("mul ").
std::optional<BlendMode> parse_blend_mode(std::string_view s);

// True for the four W3C non-separable modes and the two integer vector modes (dkCl, lgCl).
bool is_vector_mode(BlendMode m);

}  // namespace rl
