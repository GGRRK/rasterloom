// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/op_runner.hpp"

#include <exception>

#include "core/base/error.hpp"
#include "core/script/domains.hpp"

namespace rl::gui {

const rl::script::OpRegistry& gui_registry() {
    static const rl::script::OpRegistry reg = [] {
        rl::script::OpRegistry r;
        rl::script::registerAllOps(r);
        return r;
    }();
    return reg;
}

bool op_available(const std::string& op_name) { return gui_registry().find(op_name) != nullptr; }

OpResult apply_op(rl::Document& doc, const Json& op) {
    if (!op.is_object()) return {OpStatus::Error, "op must be a JSON object"};
    auto it = op.find("op");
    if (it == op.end() || !it->is_string()) return {OpStatus::Error, "missing string field 'op'"};
    const std::string name = it->get<std::string>();
    const rl::script::OpSpec* spec = gui_registry().find(name);
    if (!spec) return {OpStatus::NotAvailable, "op '" + name + "' is not registered"};

    // Cheap: a DocState copy shares every tile (copy-on-write).
    const rl::DocState before = doc.state();
    const size_t records_before = doc.history_size();
    rl::script::OpContext ctx{doc, 0, name};
    try {
        rl::script::Fields f(op, name);
        f.consume("op");
        if (spec->records_history) doc.push_history();
        spec->run(ctx, f);
        f.finish();
    } catch (const std::exception& e) {
        // Roll back: drop the record this op pushed (if any), restore the state.
        if (spec->records_history && doc.history_size() > records_before) doc.history().pop(1);
        doc.state() = before;
        std::string msg = e.what();
        if (msg.find("not yet implemented") != std::string::npos)
            return {OpStatus::NotAvailable, msg};
        return {OpStatus::Error, msg};
    }
    return {};
}

}  // namespace rl::gui
