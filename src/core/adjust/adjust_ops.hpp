// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/script/registry.hpp"

namespace rl::script {

// Registers `add_adjustment`. Called from registerAdjustFilterOps (src/core/filters/filter_ops.cpp).
void registerAdjustmentOps(OpRegistry& r);

}  // namespace rl::script
