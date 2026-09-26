// SPDX-License-Identifier: GPL-3.0-or-later
//
// `add_adjustment` (docs/math/20-adjustments-filters.md A9). Placement fields and defaults are
// exactly add_layer's (doc 10 §11.2: `id`, `parent`, top insertion); content-fill fields are not
// placement fields and are rejected.
#include "core/adjust/adjust_ops.hpp"

#include "core/adjust/adjustment.hpp"

namespace rl::script {

namespace {
void op_add_adjustment(OpContext& ctx, Fields& f) {
    const std::string id = f.req_string("id");
    const std::string parent = f.string_or("parent", "root");
    const std::string type = f.req_string("type");
    const Json* params = f.raw("params");
    f.finish();
    auto adj = adjust::make_adjustment(type, params, ctx.label);
    Node n = Node::make_adjustment(id, std::move(adj));
    if (params) n.adjust_params = params->dump();  // kept so file formats can save the params ("{}" otherwise)
    ctx.doc.add_node_top(parent, std::move(n), ctx.label);
}
}  // namespace

void registerAdjustmentOps(OpRegistry& r) { r.add("add_adjustment", op_add_adjustment); }

}  // namespace rl::script
