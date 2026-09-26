// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tile memory tiers (BUILD-SPEC requirement 2: "hard limit 50% of RAM, soft 2%, scratch cap
// 4096 MiB in $XDG_CACHE_HOME via O_TMPFILE, LZ4 for warm tiles").
//
// Every stored tile lives in a Cell, the unit that images share copy-on-write. A cell's pixels are
// in one or more of three tiers:
//   hot  - uncompressed in RAM; the only form code ever reads or writes;
//   warm - LZ4-compressed in RAM;
//   cold - LZ4-compressed in the scratch file (an O_TMPFILE, never linked into the file system).
// Reading a warm or cold cell faults it back in (decompresses into a fresh hot copy and keeps the
// compressed copy, so re-evicting a clean tile costs nothing). Writing drops the compressed copies.
// Compression is lossless, so the tiers are invisible to every result.
//
// Interpretation of the spec's numbers (stated here once):
//   hard limit  = 50% of RAM (or of the cgroup memory limit when lower). hot + warm tile bytes +
//                 explicit Reservations (large dense work buffers) may never exceed it: the
//                 allocation that would is refused with MemoryError, a clean error, instead of the
//                 kernel's OOM killer ending the process.
//   soft limit  = 2% of RAM = the HOT budget. At the next safe point (the start of every
//                 document-changing op: Document::push_history) the least-recently-used hot tiles
//                 are compressed until hot bytes <= soft. Between safe points hot may exceed soft
//                 (an op's working set is never evicted under it).
//   warm budget = another 2% of RAM for compressed tiles kept in RAM; beyond it the LRU compressed
//                 tiles spill to the scratch file, up to the 4096 MiB cap. Past the cap tiles stay
//                 warm (still bounded by the hard limit).
// Environment: RASTERLOOM_MEM_HARD_MB / _SOFT_MB / _WARM_MB, RASTERLOOM_SCRATCH_CAP_MB,
// RASTERLOOM_SCRATCH_DIR override the defaults; RASTERLOOM_MEM_TEST=1 sets soft = warm = 0 so every
// safe point compresses AND spills every tile (the golden suite runs once this way).
//
// Threading: cell reads/fault-ins are thread-safe. Eviction (safe_point / trim) must not run while
// another thread holds a reference into a tile: readers on other threads wrap their work in a
// ReadScope (the composite scheduler does), and safe_point takes the scope lock exclusively. On
// the thread that calls safe_point, no tile reference may be held across the call (true by
// construction: it runs between ops).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace rl::mem {

// The clean error for an allocation beyond the hard limit.
class MemoryError : public std::runtime_error {
public:
    explicit MemoryError(const std::string& what) : std::runtime_error(what) {}
};

struct Limits {
    uint64_t hard = 0;         // bytes
    uint64_t soft = 0;         // hot budget, bytes
    uint64_t warm = 0;         // compressed-in-RAM budget, bytes
    uint64_t scratch_cap = 0;  // bytes
    std::string scratch_dir;   // empty = $XDG_CACHE_HOME/rasterloom (or ~/.cache/rasterloom)
};

// Physical RAM (bounded by a cgroup v2 memory.max when one is set), bytes.
uint64_t system_ram();
// Limits derived from system_ram() and the environment (see the header comment).
Limits default_limits();
Limits limits();
void set_limits(const Limits& l);

struct Stats {
    uint64_t hot_bytes = 0;
    uint64_t warm_bytes = 0;      // compressed bytes held in RAM
    uint64_t cold_bytes = 0;      // compressed bytes held in the scratch file
    uint64_t reserved_bytes = 0;  // live Reservations
    uint64_t scratch_file_bytes = 0;
    uint64_t peak_hot_bytes = 0;
    uint64_t peak_charged_bytes = 0;  // max of hot + warm + reserved
    uint64_t cells = 0;
    uint64_t compressions = 0;
    uint64_t spills = 0;
    uint64_t faults = 0;
    uint64_t spill_failures = 0;
};
Stats stats();
void reset_peaks();

// One stored tile's bytes (see the header comment). `bytes` is the tile object's size. A pinned
// cell (the shared uniform tiles) is never evicted, never charged and never written.
class Cell {
public:
    explicit Cell(size_t bytes, bool pinned = false);
    ~Cell();
    Cell(const Cell&) = delete;
    Cell& operator=(const Cell&) = delete;

    // The hot bytes, faulting in when evicted. The pointer stays valid until the next eviction.
    const void* read() const {
        void* p = hot_.load(std::memory_order_acquire);
        if (p) {
            touch();
            return p;
        }
        return fault_in();
    }
    // Writable hot bytes; drops the compressed copies. The caller must be the cell's sole owner.
    void* write() {
        void* p = const_cast<void*>(read());
        if (has_backup_.load(std::memory_order_acquire)) drop_backups();
        return p;
    }

    size_t bytes() const { return bytes_; }
    bool pinned() const { return pinned_; }
    bool is_hot() const { return hot_.load(std::memory_order_acquire) != nullptr; }
    bool is_warm() const;
    bool is_cold() const;

    // Evicts to warm (compressing if no compressed copy exists). Exclusive: see the header.
    void evict();
    // Moves the compressed copy to the scratch file. false when there is nothing to spill or the
    // scratch file is full or unavailable.
    bool spill();

private:
    friend struct Registry;
    void touch() const;
    const void* fault_in() const;
    void drop_backups();

    mutable std::atomic<void*> hot_{nullptr};
    mutable std::atomic<uint64_t> last_use_{0};
    mutable std::atomic<bool> has_backup_{false};
    mutable std::mutex m_;
    mutable std::vector<char> packed_;  // warm copy
    int64_t spill_off_ = -1;            // cold copy (extent in the scratch file)
    uint32_t spill_len_ = 0;
    const uint32_t bytes_;
    const bool pinned_;
    Cell* prev_ = nullptr;  // registry list (unpinned cells only)
    Cell* next_ = nullptr;
};

// Accounts a large non-tile buffer (a dense work plane) against the hard limit for its lifetime.
// Throws MemoryError when it does not fit.
class Reservation {
public:
    Reservation() = default;
    Reservation(uint64_t bytes, const char* what);
    ~Reservation();
    Reservation(Reservation&& o) noexcept : bytes_(o.bytes_) { o.bytes_ = 0; }
    Reservation& operator=(Reservation&& o) noexcept;
    Reservation(const Reservation&) = delete;
    Reservation& operator=(const Reservation&) = delete;
    uint64_t bytes() const { return bytes_; }

private:
    uint64_t bytes_ = 0;
};

// Holds off eviction while tiles are read on this thread (nests; the outermost scope locks).
class ReadScope {
public:
    ReadScope();
    ~ReadScope();
    ReadScope(const ReadScope&) = delete;
    ReadScope& operator=(const ReadScope&) = delete;
};

// A quiescent point: advances the LRU clock and trims to the limits (compress, then spill). Called
// by Document::push_history before every document-changing op, and by the self-test between
// cases. Returns the number of cells compressed or spilled.
size_t safe_point();

// Evicts until hot <= soft and warm <= warm budget (what safe_point does after the clock tick).
size_t trim();

// Test hook: evicts (and, with `spill`, spills) every unpinned cell. Exclusive, like trim.
size_t evict_all(bool spill);

}  // namespace rl::mem
