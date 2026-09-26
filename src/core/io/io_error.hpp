// SPDX-License-Identifier: GPL-3.0-or-later
//
// Errors of the file I/O layer (open, save, import, export). The message is user-facing: it names
// the file and what was wrong ("PSD: unsupported colour mode Lab (9)").
#pragma once

#include <stdexcept>
#include <string>

namespace rl::io {

class IoError : public std::runtime_error {
public:
    explicit IoError(const std::string& what) : std::runtime_error(what) {}
};

}  // namespace rl::io
