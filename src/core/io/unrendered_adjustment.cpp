// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/io/unrendered_adjustment.hpp"

#include <nlohmann/json.hpp>

#include "core/base/error.hpp"

namespace rl::io {

std::shared_ptr<const adjust::Adjustment> build_file_adjustment(const std::string& type,
                                                               const std::string& params_json,
                                                               const std::string& what,
                                                               std::vector<std::string>& warnings) {
    if (type == kUnsupportedAdjustment) return std::make_shared<UnrenderedAdjustment>(type);
    try {
        nlohmann::json params = nlohmann::json::object();
        if (!params_json.empty()) params = nlohmann::json::parse(params_json);
        return adjust::make_adjustment(type, &params, what);
    } catch (const std::exception& e) {
        warnings.push_back(what + ": adjustment '" + type + "' is kept but not rendered (" + e.what() + ")");
        return std::make_shared<UnrenderedAdjustment>(type);
    }
}

}  // namespace rl::io
