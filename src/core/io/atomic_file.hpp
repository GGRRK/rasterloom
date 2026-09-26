// SPDX-License-Identifier: GPL-3.0-or-later
//
// Atomic file replacement (BUILD-SPEC <stack> "Atomic save: tmp + fsync + rename(2)").
//
// The bytes go to a temporary file created next to the target (same directory, so rename(2) is
// atomic), are fsync'ed, the temporary gets the permissions of the file it replaces (or 0666 minus
// umask for a new file), and it is renamed over the target; then the directory is fsync'ed. On any
// failure the temporary is removed and the existing target is left exactly as it was: a failed
// save never truncates or half-writes the user's file.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rl::io {

// Throws IoError (the target is untouched then).
void atomic_write_file(const std::string& path, const std::vector<uint8_t>& bytes);

// Reads a whole file. Throws IoError.
std::vector<uint8_t> read_file(const std::string& path);

}  // namespace rl::io
