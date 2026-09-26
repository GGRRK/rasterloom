// SPDX-License-Identifier: GPL-3.0-or-later
//
// Placeholder for an adjustment layer read from a file that this build cannot render: either a
// type outside the eight of doc 20 (a PSD Color Balance, Photo Filter, ...: type "unsupported";
// the file's raw block is kept in the node's foreign data and written back unchanged), or one of
// the eight whose implementation is missing from adjust::make_adjustment. It keeps the node an
// adjustment layer in the tree and renders as the identity f(R, G, B) = (R, G, B).
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/adjust/adjustment.hpp"

namespace rl::io {

class UnrenderedAdjustment : public adjust::Adjustment {
public:
    // params() of a placeholder is a neutral InvertParams and means nothing: writers keep such a
    // layer through the node's foreign data (raw block) and Node::adjust_params, never params(),
    // and callers identify the placeholder by type() (kUnsupportedAdjustment or an unbuilt type).
    explicit UnrenderedAdjustment(std::string type)
        : adjust::Adjustment(adjust::InvertParams{}), type_(std::move(type)) {}
    const char* type() const override { return type_.c_str(); }
    void apply(uint8_t&, uint8_t&, uint8_t&) const override {}

private:
    std::string type_;
};

constexpr const char* kUnsupportedAdjustment = "unsupported";

// Builds the adjustment for a node read from a file: adjust::make_adjustment(type, params) when
// that succeeds, else an UnrenderedAdjustment with a warning. `params_json` empty means {}.
std::shared_ptr<const adjust::Adjustment> build_file_adjustment(const std::string& type,
                                                               const std::string& params_json,
                                                               const std::string& what,
                                                               std::vector<std::string>& warnings);

}  // namespace rl::io
