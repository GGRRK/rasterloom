// SPDX-License-Identifier: GPL-3.0-or-later
//
// Adjustment-layer parameter types and the per-pixel adjusted-colour function (docs/math/
// 20-adjustments-filters.md Part A). Doc 10 consumes an adjustment only as the black box
// ADJ(node, Rb, Gb, Bb) -> (R', G', B') on bytes; alpha handling (A0's canonical rule) is done by
// the compositor's ADJUST primitive (doc 10 §4.2 step 1).
//
// Five types (levels, curves, brightness_contrast, invert, posterize) are LutAdjustment; three
// (hue_saturation, black_white, threshold) mix channels per pixel. An Adjustment object is
// immutable: `set_adjustment` (docs/math/60-editing-ops.md §5) replaces a layer's params by
// building a new one. Every adjustment keeps the typed params it was built from (params()), so the
// GUI and the file writers can read them back.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include <nlohmann/json_fwd.hpp>

#include "core/adjust/adjust_params.hpp"

namespace rl::adjust {

class Adjustment {
public:
    explicit Adjustment(AdjustmentParams params) : params_(std::move(params)) {}
    virtual ~Adjustment() = default;

    // The doc-20 type name ("invert", "levels", ...).
    virtual const char* type() const = 0;

    // f(R8, G8, B8) of doc 20 A0: bytes in, bytes out. Never reads alpha.
    virtual void apply(uint8_t& r, uint8_t& g, uint8_t& b) const = 0;

    // The (validated) parameters this adjustment was built from.
    const AdjustmentParams& params() const { return params_; }

private:
    AdjustmentParams params_;
};

// A per-channel lookup-table adjustment: f(R8, G8, B8) = (LUT_R[R8], LUT_G[G8], LUT_B[B8]).
class LutAdjustment : public Adjustment {
public:
    using Lut = std::array<uint8_t, 256>;
    LutAdjustment(AdjustmentParams params, std::string type_name, const Lut& r, const Lut& g, const Lut& b)
        : Adjustment(std::move(params)), type_(std::move(type_name)), r_(r), g_(g), b_(b) {}

    const char* type() const override { return type_.c_str(); }
    void apply(uint8_t& r, uint8_t& g, uint8_t& b) const override {
        r = r_[r];
        g = g_[g];
        b = b_[b];
    }
    const Lut& lut_r() const { return r_; }
    const Lut& lut_g() const { return g_; }
    const Lut& lut_b() const { return b_; }

private:
    std::string type_;
    Lut r_, g_, b_;
};

// The eight doc-20 type names (A9), in doc order.
bool is_known_type(const std::string& type);

// Type name of a params variant ("levels", ...).
const char* type_name(const AdjustmentParams& p);

// Builds an adjustment from typed params. Validates every range of A1-A8 and throws rl::ScriptError
// (prefixed with `context`) on a violation. This is the GUI's entry point.
std::shared_ptr<const Adjustment> make_adjustment(const AdjustmentParams& params,
                                                  const std::string& context = "adjustment");

// Parses a render-script params object (A9) for `type`. `params` may be null (omitted, = {}).
// Throws rl::ScriptError on an unknown type or invalid params (unknown key, wrong type, out of range).
AdjustmentParams parse_adjustment_params(const std::string& type, const nlohmann::json* params,
                                         const std::string& context);

// parse_adjustment_params + make_adjustment (the add_adjustment op).
std::shared_ptr<const Adjustment> make_adjustment(const std::string& type, const nlohmann::json* params,
                                                  const std::string& context);

// The params as a script-shaped JSON object (every field written explicitly), such that
// parse_adjustment_params(type_name(p), &json) reproduces `p`. For save files and the GUI.
nlohmann::json params_to_json(const AdjustmentParams& p);

}  // namespace rl::adjust
