// SPDX-License-Identifier: GPL-3.0-or-later
//
// Node display names (docs/math/60-editing-ops.md §1.1). A name is a display label only: it never
// addresses a node and never enters a pixel formula.
#pragma once

#include <cstddef>
#include <string>

#include "core/doc/node.hpp"

namespace rl::edit {

// Maximum length of a valid name, in Unicode code points (§1.1 rule 3).
constexpr size_t kMaxNameCodePoints = 255;

// display_name(node) = node.name if non-empty, else node.id (§1.1).
inline const std::string& display_name(const Node& n) { return n.name.empty() ? n.id : n.name; }

// Number of code points of `s` when it is well-formed UTF-8 made of Unicode scalar values only
// (no surrogates, no overlong forms, nothing above U+10FFFF); SIZE_MAX otherwise.
size_t utf8_scalar_count(const std::string& s);

// §1.1: well-formed UTF-8 of scalar values, no code point in U+0000..U+001F or U+007F, at most 255
// code points. "" is valid.
bool is_valid_name(const std::string& s);

// Why `s` is not a valid name (empty string when it is valid). For error messages.
std::string name_problem(const std::string& s);

}  // namespace rl::edit
