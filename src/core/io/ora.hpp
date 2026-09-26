// SPDX-License-Identifier: GPL-3.0-or-later
//
// OpenRaster 0.0.6 (.ora) and Rasterloom's native .orp, which are the same bytes (BUILD-SPEC
// <stack> "Native format" and the 2026-09-26 addendum; layout rules in docs/research/psd-and-ora.md
// section 3).
//
// Archive (deterministic ZIP, core/io/zip.hpp), in this order:
//   mimetype                 STORED first, "image/openraster" (bytes 30 / 38 for shared-mime-info)
//   stack.xml                a truthful viewing baseline for GIMP / Krita / MyPaint:
//                            - raster layers reference data/layerN.png at their content bbox; a
//                              layer with an enabled mask or fill < 1 references a BAKED png (mask and
//                              fill folded into alpha);
//                            - clip groups become <stack isolation="isolate" composite-op=base mode
//                              opacity=base opacity> holding the base (svg:src-over) and the clipped
//                              layers (Normal: svg:src-atop; other modes: the mode +
//                              alpha-preserve="true");
//                            - pass-through groups are isolation="auto", isolated groups
//                              isolation="isolate" + composite-op;
//                            - composite-op: 15 svg: values and 12 krita: ids (research 3.3);
//                            - adjustment layers are not in stack.xml (no <filter> elements, ever);
//                            - every element carries rl:id (xmlns:rl="https://rasterloom.invalid/ns/1").
//   data/layerN.png          the pixels stack.xml references
//   Thumbnails/thumbnail.png <= 256 px, aspect kept, never upscaled, not referenced from XML
//   mergedimage.png          RENDER over the canvas background (exactly what --render-file writes)
//   document.json            the whole document model: canvas, background, the tree with every node
//                            property (fill, clip, clbl, lock_alpha, masks, adjustment type+params,
//                            dissolve seed, names), the selection and its Reselect copy, foreign
//                            data, and "stack_sha256" of the exact stack.xml bytes
//   rasterloom/...           unbaked layer pixels, masks, selection planes, foreign payloads
//
// Reading: when document.json is present and stack_sha256 matches stack.xml, document.json is
// authoritative and the document round-trips losslessly. Otherwise (a plain .ora from GIMP / Krita /
// MyPaint, or stack.xml edited by another tool) the tree is built from stack.xml with a warning.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/doc/document.hpp"

namespace rl::ora {

bool is_ora(const std::vector<uint8_t>& bytes);

std::vector<uint8_t> write(const DocState& s, std::vector<std::string>& warnings);
DocState read(const std::vector<uint8_t>& bytes, std::vector<std::string>& warnings);

// composite-op <-> blend mode (exposed for tests).
const char* composite_op(BlendMode m);

}  // namespace rl::ora
