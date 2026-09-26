// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/script/registry.hpp"

#include <stdexcept>

namespace rl::script {

void OpRegistry::add(const std::string& name, OpHandler run, bool records_history) {
    if (ops_.count(name) != 0) throw std::logic_error("render-script op registered twice: " + name);
    ops_.emplace(name, OpSpec{std::move(run), records_history});
}

const OpSpec* OpRegistry::find(const std::string& name) const {
    auto it = ops_.find(name);
    return it == ops_.end() ? nullptr : &it->second;
}

std::vector<std::string> OpRegistry::names() const {
    std::vector<std::string> out;
    for (const auto& kv : ops_) out.push_back(kv.first);
    return out;
}

}  // namespace rl::script
