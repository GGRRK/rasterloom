// SPDX-License-Identifier: GPL-3.0-or-later
//
// One registration function per domain. Each lane implements its function in its own directory and
// adds ops only there; this header and register_all.cpp never need to change when a lane adds an op.
#pragma once

#include "core/script/registry.hpp"

namespace rl::script {

// doc 10: layers, groups, blend/opacity/fill/visibility, clipping, masks, merges, flatten, undo.
// Implemented in src/core/composite/compositing_ops.cpp.
void registerCompositingOps(OpRegistry& r);

// doc 20: add_adjustment and the six filters. Implemented in src/core/filters/filter_ops.cpp
// (add_adjustment itself lives in src/core/adjust/adjust_ops.cpp).
void registerAdjustFilterOps(OpRegistry& r);

// doc 30: selections, transform, image/canvas geometry, gradient tool, bucket fill.
// Implemented in src/core/select/geometry_ops.cpp.
void registerGeometryOps(OpRegistry& r);

// doc 40: brush, eraser and clone strokes. Implemented in src/core/brush/brush_ops.cpp.
void registerBrushOps(OpRegistry& r);

// File I/O ops (PSD / ORA / PNG import-export), if any. Implemented in src/core/io/io_ops.cpp.
void registerIoOps(OpRegistry& r);

// doc 60: delete_layer, set_name, duplicate_layer, set_adjustment, set_group_mode, select_alpha.
// Implemented in src/core/edit/editing_ops.cpp.
void registerEditingOps(OpRegistry& r);

// The central list: calls the six functions above, in that order.
void registerAllOps(OpRegistry& r);

}  // namespace rl::script
