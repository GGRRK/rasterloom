// SPDX-License-Identifier: GPL-3.0-or-later
//
// THE central list of op domains. Explicit calls (not static self-registration) so that no domain
// can be silently dead-stripped out of librasterloomcore.a.
#include "core/script/domains.hpp"

namespace rl::script {

void registerAllOps(OpRegistry& r) {
    registerCompositingOps(r);
    registerAdjustFilterOps(r);
    registerGeometryOps(r);
    registerBrushOps(r);
    registerIoOps(r);
    registerEditingOps(r);
}

}  // namespace rl::script
