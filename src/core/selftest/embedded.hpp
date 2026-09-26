// SPDX-License-Identifier: GPL-3.0-or-later
//
// Files embedded into the rasterloomselftest library at build time by cmake/EmbedFiles.cmake (the
// frozen golden corpus under tests/goldens/, produced by tools/freeze_goldens.py).
#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

namespace rl::selftest {

struct EmbeddedFile {
    const char* path;  // relative to tests/goldens, '/' separated; nullptr in an empty table
    const char* data;  // NUL-terminated for convenience; `size` excludes the terminator
    std::size_t size;
};

// Every embedded file, sorted by path. Defined in the generated index (selftest_index.cpp).
const std::vector<EmbeddedFile>& embedded_files();

// The file at `path`, or nullptr.
const EmbeddedFile* find_embedded(std::string_view path);

}  // namespace rl::selftest
