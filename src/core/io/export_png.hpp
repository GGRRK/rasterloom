// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

#include "core/doc/document.hpp"
#include "core/io/png.hpp"

namespace rl::io {

// "out": "png8" (00-conventions C9, doc 10 §11): writes RENDER(root.children, BG) as an 8-bit RGBA
// PNG without altering any byte. Renders one 64-row band of tiles at a time (tiles row-major).
void write_document_png(const DocState& s, const std::string& path);

// The same composite as an in-memory buffer (tests, GUI thumbnails).
RgbaBuffer render_document(const DocState& s);

}  // namespace rl::io
