// SPDX-License-Identifier: GPL-3.0-or-later
//
// Render-script engine: parses {"canvas":{...}, "ops":[...], "out":"png8"} (doc 10 §11) and runs
// the ops in order against a Document with history depth 1000 (C10). Any invalid input throws
// rl::ScriptError (C9).
#pragma once

#include <memory>
#include <string>

#include "core/doc/document.hpp"
#include "core/script/registry.hpp"

namespace rl::script {

// The registry with every domain registered (built once, on first use).
const OpRegistry& default_registry();

struct ScriptResult {
    std::unique_ptr<Document> doc;
    std::string out;  // the "out" format; "png8" is the only one in v0.1
};

ScriptResult run_script(const Json& script, const OpRegistry& registry = default_registry());

// Parses JSON text (a parse error is a ScriptError) and runs it.
ScriptResult run_script_text(const std::string& text, const OpRegistry& registry = default_registry());

// Reads and runs a script file. An unreadable file throws ScriptError.
ScriptResult run_script_file(const std::string& path, const OpRegistry& registry = default_registry());

}  // namespace rl::script
