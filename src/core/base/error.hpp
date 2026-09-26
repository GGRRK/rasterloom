// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stdexcept>
#include <string>

namespace rl {

// A render-script error (docs/math/00-conventions.md C9): unknown op or field, wrong type,
// out-of-range value, unknown id, wrong node kind, singular matrix, oversize result, undo past the
// start of history. The CLI reports it, exits non-zero and writes no PNG.
class ScriptError : public std::runtime_error {
public:
    explicit ScriptError(const std::string& what) : std::runtime_error(what) {}
};

}  // namespace rl
