// SPDX-License-Identifier: GPL-3.0-or-later
//
// Structural pixel operations of docs/math/10-compositing.md §8 (apply mask) and §9 (merge down,
// merge visible, flatten). Preconditions are checked and violations throw rl::ScriptError.
#pragma once

#include <string>

#include "core/doc/document.hpp"

namespace rl::composite {

// §8 Apply: A' = q(n(A) * n(M)) for a raster layer (stored mask applied even if disabled), colour
// unchanged, canonicalised; the mask is removed. Ignores lock_alpha.
void apply_mask(Node& raster, const std::string& context);

// §9.1: merges the node `upper_id` into the raster layer directly below it.
void merge_down(Document& doc, const std::string& upper_id, const std::string& context);

// §9.2: renders the visible top-level nodes onto transparency into a new raster layer `id`.
void merge_visible(Document& doc, const std::string& id, const std::string& context);

// §9.3: renders the whole document (bg included) into one raster layer `id`; bg becomes #00000000.
void flatten(Document& doc, const std::string& id, const std::string& context);

}  // namespace rl::composite
