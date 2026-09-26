// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/doc/node.hpp"

namespace rl {

const char* node_kind_name(NodeKind k) {
    switch (k) {
        case NodeKind::Raster: return "raster layer";
        case NodeKind::Adjustment: return "adjustment layer";
        case NodeKind::Group: return "group";
    }
    return "node";
}

Node Node::make_raster(std::string id, int w, int h) {
    Node n;
    n.id = std::move(id);
    n.kind = NodeKind::Raster;
    n.mode = BlendMode::Norm;
    n.pixels.reset(w, h);
    return n;
}

Node Node::make_adjustment(std::string id, std::shared_ptr<const adjust::Adjustment> adj) {
    Node n;
    n.id = std::move(id);
    n.kind = NodeKind::Adjustment;
    n.mode = BlendMode::Norm;
    n.adjustment = std::move(adj);
    n.adjust_params = "{}";  // canonical empty params (node.hpp)
    return n;
}

Node Node::make_group(std::string id, BlendMode mode) {
    Node n;
    n.id = std::move(id);
    n.kind = NodeKind::Group;
    n.mode = mode;
    return n;
}

}  // namespace rl
