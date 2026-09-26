// SPDX-License-Identifier: GPL-3.0-or-later
//
// Render-script op registry. Ops are keyed by name; each domain registers its ops through one
// explicit registration function (script/domains.hpp) called from the single central list in
// script/register_all.cpp. There is deliberately NO static self-registration: objects in a static
// library that nothing references are dropped by the linker, and their registrars with them.
#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "core/doc/document.hpp"
#include "core/script/fields.hpp"

namespace rl::script {

struct OpContext {
    Document& doc;
    size_t op_index = 0;   // position in the script's "ops" array
    std::string label;     // "op #<index> (<name>)", the prefix for error messages
};

// A handler reads its fields from `f` (then calls f.finish() before changing the document) and
// applies the op to ctx.doc, throwing rl::ScriptError on any invalid input.
using OpHandler = std::function<void(OpContext& ctx, Fields& f)>;

struct OpSpec {
    OpHandler run;
    // When true (every document-changing op, C10), the engine pushes exactly one history record
    // holding the pre-op state before calling the handler. Only `undo` sets this to false.
    bool records_history = true;
};

class OpRegistry {
public:
    // Throws std::logic_error when `name` is already registered (two lanes claiming one op).
    void add(const std::string& name, OpHandler run, bool records_history = true);
    const OpSpec* find(const std::string& name) const;
    std::vector<std::string> names() const;  // sorted
    size_t size() const { return ops_.size(); }

private:
    std::map<std::string, OpSpec> ops_;
};

}  // namespace rl::script
