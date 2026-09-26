// SPDX-License-Identifier: GPL-3.0-or-later
//
// SHA-256 (FIPS 180-4), used for foreign-block fingerprints in --dump-tree and for the .orp
// stack.xml integrity hash. Not used for anything security-relevant.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace rl::io {

std::array<uint8_t, 32> sha256(const uint8_t* data, size_t n);
// Lower-case hex digest.
std::string sha256_hex(const uint8_t* data, size_t n);
inline std::string sha256_hex(const std::vector<uint8_t>& v) { return sha256_hex(v.data(), v.size()); }
inline std::string sha256_hex(const std::string& s) {
    return sha256_hex(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

}  // namespace rl::io
