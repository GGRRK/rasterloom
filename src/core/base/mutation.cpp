// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/base/mutation.hpp"

#include <cctype>
#include <stdexcept>

namespace rl::mut {

namespace detail {
bool g_active[kCount] = {};
}

namespace {

struct Info {
    const char* owner;
    const char* text;
};

// Descriptions are one-line summaries of the "mutation hooks" tables of docs/math/10, 20, 30, 40
// and the BUILD-SPEC seed list (id 13).
constexpr Info kTable[kCount] = {
    /* 0 */ {"10-compositing", "sLit uses Pegtop soft light instead of the W3C formula"},
    /* 1 */ {"10-compositing", "non-separable Lum uses Rec.709 weights (0.2126/0.7152/0.0722)"},
    /* 2 */ {"10-compositing", "COMPOSITE step 4 evaluates B on premultiplied operands B(ab*cb, as*cs)"},
    /* 3 */ {"10-compositing", "COMPOSITE in linear light (sRGB decode of cb/cs, encode of co)"},
    /* 4 */ {"10-compositing", "pass-through group rendered as an isolated Normal group"},
    /* 5 */ {"10-compositing", "clipped coverage inside G is ci*S (base shape applied twice)"},
    /* 6 */ {"10-compositing", "clip-base fill treated as opacity (G seeded q(1.0), cg*(o0*f0))"},
    /* 7 */ {"40-brush", "flow as repeated source-over instead of the Wash/Build-up lerp"},
    /* 8 */ {"40-brush", "spacing accumulator frac reset at every add_sample"},
    /* 9 */ {"40-brush", "spacing measured in screen pixels (L * view_zoom)"},
    /* 10 */ {"40-brush", "a history record committed per dab instead of per stroke"},
    /* 11 */ {"30-geometry-selection", "bicubic resample without premultiplying taps"},
    /* 12 */ {"20-adjustments-filters", "gaussian blur vertical passes run before horizontal passes"},
    /* 13 */ {"tile-engine", "dense tile grid instead of the shared empty tile"},
    /* 14 */ {"10-compositing", "q truncates: floor(clamp01(x)*255.0) instead of rounding"},
    /* 15 */ {"30-geometry-selection", "magic wand region uses 4-connectivity instead of 8"},
    /* 16 */ {"10-compositing", "ColorDodge checks s==1 before cb==0, so B(0,1)=1"},
    /* 17 */ {"10-compositing", "hMix threshold off by one: (S+B) > 255"},
    /* 18 */ {"10-compositing", "dkCl/lgCl ties pick the source instead of the backdrop"},
    /* 19 */ {"10-compositing", "pass-through opacity/mask applied per child instead of one lerp"},
    /* 20 */ {"10-compositing", "clbl ignored: clbl=false bases rendered as clbl=true"},
    /* 21 */ {"10-compositing", "fdiv checks cs==0 before cb==0, so B(0,0)=1"},
    /* 22 */ {"10-compositing", "canonicalisation skipped when ao>0 but q(ao)==0"},
    /* 23 */ {"10-compositing", "clip shape ignores the base mask: S = n(A0)"},
    /* 24 */ {"20-adjustments-filters", "levels uses pow(v, gamma) instead of pow(v, 1/gamma)"},
    /* 25 */ {"20-adjustments-filters", "curves input clamp to [X0, Xm] removed (cubic extrapolation)"},
    /* 26 */ {"20-adjustments-filters", "hue/saturation master rotates h6 - dh instead of h6 + dh"},
    /* 27 */ {"20-adjustments-filters", "posterize uses kd = n instead of n - 1"},
    /* 28 */ {"20-adjustments-filters", "unsharp mask threshold compares abs(d)/255 against a level count"},
    /* 29 */ {"20-adjustments-filters", "gaussian blur without premultiply (straight colour blurred)"},
    /* 30 */ {"30-geometry-selection", "bicubic sampling drops the -0.5 pixel-centre offset"},
    /* 31 */ {"30-geometry-selection", "transform inverse computed from the transposed matrix"},
    /* 32 */ {"30-geometry-selection", "feather uses sigma = r instead of r / 2"},
    /* 33 */ {"30-geometry-selection", "polygon inside-test uses nonzero winding instead of even-odd"},
    /* 34 */ {"30-geometry-selection", "selection subtract uses min(S, 255 - B) instead of max(S - B, 0)"},
    /* 35 */ {"30-geometry-selection", "colour-distance test normalises d by 255 but not the tolerance"},
    /* 36 */ {"40-brush", "dab falloff inverted in the transition band (a ring)"},
    /* 37 */ {"40-brush", "pressure LUT lookup uses max(q(p) - 1, 0)"},
    /* 38 */ {"40-brush", "stabilizer EMA also applied to pressure"},
    /* 39 */ {"40-brush", "eraser ignores lock_alpha and reduces alpha on locked layers"},
    /* 40 */ {"60-editing-ops", "duplicate_layer without parent inserts the copy at the top of the container, not at i + 1"},
    /* 41 */ {"60-editing-ops", "set_adjustment merges params into the previous params instead of replacing them"},
    /* 42 */ {"40-brush", "pre-2026-09-26 flow model: mask-shaped ceiling (Wash T = O * m, Build-up m) instead of mask-scaled rate"},
    /* 43 */ {"60-editing-ops", "place_image default centring truncates (W - w) / 2 toward zero instead of floor division"},
    /* 44 */ {"60-editing-ops", "COPY treats partially selected pixels as fully selected (alpha not scaled by coverage)"},
    /* 45 */ {"60-editing-ops", "clear ignores the selection and clears the whole layer"},
};

}  // namespace

void set_active(const std::vector<int>& ids) {
    for (int id : ids) {
        if (id < 0 || id >= kCount) {
            throw std::invalid_argument("mutation id out of range 0.." + std::to_string(kCount - 1) +
                                        ": " + std::to_string(id));
        }
    }
    clear();
    for (int id : ids) detail::g_active[id] = true;
}

void clear() {
    for (bool& b : detail::g_active) b = false;
}

std::vector<int> active_list() {
    std::vector<int> out;
    for (int i = 0; i < kCount; ++i)
        if (detail::g_active[i]) out.push_back(i);
    return out;
}

std::vector<int> parse_list(std::string_view text) {
    std::vector<int> ids;
    size_t pos = 0;
    while (true) {
        size_t comma = text.find(',', pos);
        std::string_view item =
            text.substr(pos, comma == std::string_view::npos ? std::string_view::npos : comma - pos);
        while (!item.empty() && std::isspace(static_cast<unsigned char>(item.front())))
            item.remove_prefix(1);
        while (!item.empty() && std::isspace(static_cast<unsigned char>(item.back())))
            item.remove_suffix(1);
        if (item.empty() || item.size() > 3)
            throw std::invalid_argument("bad mutation list: '" + std::string(text) + "'");
        int v = 0;
        for (char ch : item) {
            if (ch < '0' || ch > '9')
                throw std::invalid_argument("bad mutation list: '" + std::string(text) + "'");
            v = (v * 10) + (ch - '0');
        }
        if (v >= kCount)
            throw std::invalid_argument("mutation id out of range 0.." + std::to_string(kCount - 1) +
                                        ": " + std::to_string(v));
        ids.push_back(v);
        if (comma == std::string_view::npos) break;
        pos = comma + 1;
    }
    return ids;
}

const char* description(int id) { return (id >= 0 && id < kCount) ? kTable[id].text : ""; }
const char* owner(int id) { return (id >= 0 && id < kCount) ? kTable[id].owner : ""; }

ScopedMutations::ScopedMutations(const std::vector<int>& ids) : saved_(active_list()) {
    set_active(ids);
}
ScopedMutations::~ScopedMutations() { set_active(saved_); }

}  // namespace rl::mut
