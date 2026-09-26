// SPDX-License-Identifier: GPL-3.0-or-later
//
// Mutation registry (BUILD-SPEC <verification> "mutation gate"). Each id injects one precise defect
// into one formula of the core so the golden suite can prove it is not vacuous. The set of active
// mutations is configured once at startup (rasterloom-cli --mutate=N / RASTERLOOM_MUTATE=N); unit
// tests may reconfigure it through ScopedMutations. The reference implementation never mutates.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace rl::mut {

constexpr int kCount = 43;  // ids 0..42

namespace detail {
extern bool g_active[kCount];
}

// True when mutation `id` is switched on. Cheap enough for per-pixel use.
inline bool active(int id) { return id >= 0 && id < kCount && detail::g_active[id]; }

// Replaces the active set. Throws std::invalid_argument on an id outside 0..kCount-1.
void set_active(const std::vector<int>& ids);
void clear();
std::vector<int> active_list();

// Parses "N" or "N,M,..." (whitespace around items allowed). Throws std::invalid_argument.
std::vector<int> parse_list(std::string_view text);

// One-line description of mutation `id` (from the "mutation hooks" sections of docs 10/20/30/40/60
// plus BUILD-SPEC's seed list). Returns "" for an id out of range.
const char* description(int id);
// Owning math doc of mutation `id`, e.g. "10-compositing".
const char* owner(int id);

// RAII helper for tests: activates `ids`, restores the previous set on destruction.
class ScopedMutations {
public:
    explicit ScopedMutations(const std::vector<int>& ids);
    ~ScopedMutations();
    ScopedMutations(const ScopedMutations&) = delete;
    ScopedMutations& operator=(const ScopedMutations&) = delete;

private:
    std::vector<int> saved_;
};

}  // namespace rl::mut
