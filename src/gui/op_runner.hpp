// SPDX-License-Identifier: GPL-3.0-or-later
//
// Runs ONE render-script op (the JSON grammar of docs/math/*) against a live rl::Document through
// the core op registry, exactly as the script engine does for each element of "ops": look the op
// up, push one history record when the op records history, run the handler, reject unknown fields.
// On any error the document is restored to its state before the op (history included), so a
// rejected GUI action never leaves a half-applied change or a stray history record.
//
// The GUI registry is exactly the core's default set (the GUI registers no ops of its own; the
// layer-tree edits are doc 60's core ops). An op the registry does not know is reported as
// NotAvailable, so a menu item whose op another lane has not merged yet lights up by itself once
// the op is registered.
#pragma once

#include <string>

#include "core/doc/document.hpp"
#include "core/script/registry.hpp"

namespace rl::gui {

using Json = rl::script::Json;

enum class OpStatus { Ok, NotAvailable, Error };

struct OpResult {
    OpStatus status = OpStatus::Ok;
    std::string message;  // empty on success
    bool ok() const { return status == OpStatus::Ok; }
};

// The registry the GUI executes against (built once).
const rl::script::OpRegistry& gui_registry();

// True when `op_name` is registered (menus use this to annotate not-yet-available items).
bool op_available(const std::string& op_name);

// Applies `op` (a JSON object with an "op" field) to `doc`.
OpResult apply_op(rl::Document& doc, const Json& op);

}  // namespace rl::gui
