// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

#include "core/doc/document.hpp"
#include "core/script/engine.hpp"

namespace rliotest {

// A document built by a render script (ops JSON array text), canvas w x h.
inline rl::DocState doc_from_ops(int w, int h, const std::string& ops, const std::string& bg = "#00000000") {
    const std::string s = R"({"canvas":{"w":)" + std::to_string(w) + R"(,"h":)" + std::to_string(h) + R"(,"bg":")" + bg +
                          R"("},"ops":[)" + ops + R"(],"out":"png8"})";
    return rl::script::run_script_text(s).doc->state();
}

// A fresh scratch directory under the test output dir.
std::string scratch_dir(const std::string& name);

}  // namespace rliotest
