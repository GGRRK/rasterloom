// SPDX-License-Identifier: GPL-3.0-or-later
//
// The layer tree (docs/math/10-compositing.md §1). A Node is a value type: copying a node tree
// copies tile handles only (tiles are shared copy-on-write), which is what makes history snapshots
// cheap (00-conventions C10, BUILD-SPEC D5).
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/adjust/adjustment.hpp"
#include "core/doc/blend_mode.hpp"
#include "core/doc/foreign.hpp"
#include "core/tile/tiled_image.hpp"

namespace rl {

enum class NodeKind : unsigned char { Raster, Adjustment, Group };

const char* node_kind_name(NodeKind k);

// A layer mask: a canvas-sized byte plane (sparse; unwritten area reads as the `outside` value,
// which is the plane's background) and an enabled flag (doc 10 §8).
struct LayerMask {
    GrayImage plane;
    bool enabled = true;
};

struct Node {
    std::string id;
    NodeKind kind = NodeKind::Raster;
    // Display name (file formats carry one per layer). Empty means "show the id". Not used by any
    // math; render scripts address nodes by id only.
    std::string name;

    // Every node kind (§1).
    bool visible = true;
    double opacity = 1.0;
    std::optional<LayerMask> mask;
    // Raster/adjustment: a blend mode other than Pass (default Norm). Group: Pass (default) or any
    // blend mode (isolated).
    BlendMode mode = BlendMode::Norm;
    uint64_t seed = 0;  // Dissolve seed (§3.5)

    // Raster and adjustment layers only.
    double fill = 1.0;
    bool clip = false;
    bool clbl = true;

    // Raster only.
    bool lock_alpha = false;
    RgbaImage pixels;

    // Adjustment only. The type is fixed at creation; set_adjustment (doc 60 §5) replaces the params.
    std::shared_ptr<const adjust::Adjustment> adjustment;
    // The params object the adjustment was built from, as compact JSON text (nlohmann dump()).
    // Canonical: always an object's text, "{}" when there are no params (never ""), whether the
    // node came from add_adjustment, set_adjustment or a file reader, so dump-tree after a
    // save/reopen matches. File formats save it so that the adjustment can be rebuilt on open.
    std::string adjust_params;

    // Group only; index 0 = bottom.
    std::vector<Node> children;

    // Data a file format carried for this node that the model does not interpret (core/doc/
    // foreign.hpp). Written back unchanged on save.
    ForeignData foreign;

    bool is_raster() const { return kind == NodeKind::Raster; }
    bool is_adjustment() const { return kind == NodeKind::Adjustment; }
    bool is_group() const { return kind == NodeKind::Group; }

    // Factories with doc 10 §1 defaults.
    static Node make_raster(std::string id, int w, int h);
    static Node make_adjustment(std::string id, std::shared_ptr<const adjust::Adjustment> adj);
    static Node make_group(std::string id, BlendMode mode = BlendMode::Pass);
};

}  // namespace rl
