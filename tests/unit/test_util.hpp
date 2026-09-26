// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

#include "core/io/export_png.hpp"
#include "core/script/engine.hpp"

namespace rltest {

// Runs a script given as JSON text and returns the final composite (RENDER over bg).
inline rl::io::RgbaBuffer render_text(const std::string& json) {
    auto res = rl::script::run_script_text(json);
    return rl::io::render_document(res.doc->state());
}

// Wraps an ops array (JSON text) into a full script.
inline std::string make_script(int w, int h, const std::string& ops, const std::string& bg = "#00000000") {
    return R"({"canvas":{"w":)" + std::to_string(w) + R"(,"h":)" + std::to_string(h) + R"(,"bg":")" + bg +
           R"("},"ops":[)" + ops + R"(],"out":"png8"})";
}

inline bool same_pixels(const rl::io::RgbaBuffer& a, const rl::io::RgbaBuffer& b) {
    return a.w == b.w && a.h == b.h && a.px == b.px;
}

}  // namespace rltest
