// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <optional>
#include <string_view>

#include "core/base/types.hpp"

namespace rl {

// Parses "#RRGGBB" (alpha FF) or "#RRGGBBAA", hex digits case-insensitive, straight alpha
// (00-conventions C7, doc 10 §11). Returns nullopt on any other spelling. The result is NOT
// canonicalised; callers that store it apply canonicalize().
std::optional<Rgba8> parse_hex_color(std::string_view s);

}  // namespace rl
