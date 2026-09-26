// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/tile/memory.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <lz4.h>
#include <map>
#include <memory>
#include <new>
#include <shared_mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace rl::mem {

namespace {

constexpr uint64_t kMiB = 1024ull * 1024ull;

// ---- accounting ------------------------------------------------------------------------------------
std::atomic<uint64_t> g_hot{0}, g_warm{0}, g_cold{0}, g_reserved{0}, g_charged{0};
std::atomic<uint64_t> g_peak_hot{0}, g_peak_charged{0};
std::atomic<uint64_t> g_cells{0}, g_compressions{0}, g_spills{0}, g_faults{0}, g_spill_failures{0};
std::atomic<uint64_t> g_epoch{1};
std::atomic<uint64_t> g_hard{0};  // mirror of limits().hard for the allocation fast path

void bump_peak(std::atomic<uint64_t>& peak, uint64_t v) {
    uint64_t cur = peak.load(std::memory_order_relaxed);
    while (v > cur && !peak.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
    }
}

std::string mib(uint64_t b) { return std::to_string((b + kMiB - 1) / kMiB) + " MiB"; }

void ensure_limits();

// Charges `bytes` against the hard limit or throws MemoryError.
void charge(uint64_t bytes, const char* what) {
    ensure_limits();
    const uint64_t hard = g_hard.load(std::memory_order_relaxed);
    uint64_t cur = g_charged.load(std::memory_order_relaxed);
    do {
        if (cur + bytes > hard) {
            throw MemoryError(std::string("out of memory: ") + what + " needs " + mib(bytes) + ", " + mib(cur) +
                              " already in use, hard limit " + mib(hard) +
                              " (50% of RAM; RASTERLOOM_MEM_HARD_MB overrides)");
        }
    } while (!g_charged.compare_exchange_weak(cur, cur + bytes, std::memory_order_relaxed));
    bump_peak(g_peak_charged, cur + bytes);
}
void charge_nothrow(uint64_t bytes) {
    const uint64_t v = g_charged.fetch_add(bytes, std::memory_order_relaxed) + bytes;
    bump_peak(g_peak_charged, v);
}
void uncharge(uint64_t bytes) { g_charged.fetch_sub(bytes, std::memory_order_relaxed); }

void add_hot(uint64_t b) { bump_peak(g_peak_hot, g_hot.fetch_add(b, std::memory_order_relaxed) + b); }

// ---- limits -------------------------------------------------------------------------------------------
std::mutex g_limits_m;
Limits g_limits;
bool g_limits_set = false;

bool env_mb(const char* name, uint64_t& out) {
    const char* v = std::getenv(name);
    if (!v || !*v) return false;
    char* end = nullptr;
    errno = 0;
    const unsigned long long n = std::strtoull(v, &end, 10);
    if (errno != 0 || !end || *end != '\0') return false;
    out = static_cast<uint64_t>(n) * kMiB;
    return true;
}

void ensure_limits() {
    if (g_hard.load(std::memory_order_acquire) != 0) return;
    std::lock_guard<std::mutex> lk(g_limits_m);
    if (!g_limits_set) {
        g_limits = default_limits();
        g_limits_set = true;
    }
    g_hard.store(g_limits.hard, std::memory_order_release);
}

// ---- scratch file --------------------------------------------------------------------------------------
struct Scratch {
    std::mutex m;
    int fd = -1;
    bool tried = false;
    uint64_t end = 0;                     // file high-water mark in use
    std::map<uint64_t, uint64_t> holes;  // offset -> length of free extents below `end`

    static void mkdirs(const std::string& path) {
        std::string cur;
        size_t pos = 0;
        while (pos != std::string::npos) {
            pos = path.find('/', pos + 1);
            cur = path.substr(0, pos);
            if (!cur.empty()) ::mkdir(cur.c_str(), 0700);
        }
    }

    bool open_locked() {
        if (fd >= 0) return true;
        if (tried) return false;
        tried = true;
        std::string dir = limits().scratch_dir;
        if (dir.empty()) {
            if (const char* x = std::getenv("XDG_CACHE_HOME"); x && *x == '/')
                dir = std::string(x) + "/rasterloom";
            else if (const char* h = std::getenv("HOME"); h && *h)
                dir = std::string(h) + "/.cache/rasterloom";
            else
                dir = "/tmp/rasterloom";
        }
        mkdirs(dir);
#ifdef O_TMPFILE
        fd = ::open(dir.c_str(), O_TMPFILE | O_RDWR | O_CLOEXEC, 0600);
#endif
        if (fd < 0) {  // file systems without O_TMPFILE: create + unlink at once
            std::string tmpl = dir + "/scratch-XXXXXX";
            std::vector<char> buf(tmpl.begin(), tmpl.end());
            buf.push_back('\0');
            fd = ::mkostemp(buf.data(), O_CLOEXEC);
            if (fd >= 0) ::unlink(buf.data());
        }
        return fd >= 0;
    }

    int64_t alloc(uint64_t len) {
        std::lock_guard<std::mutex> lk(m);
        if (!open_locked()) return -1;
        for (auto it = holes.begin(); it != holes.end(); ++it) {
            if (it->second >= len) {
                const uint64_t off = it->first, rest = it->second - len;
                holes.erase(it);
                if (rest) holes.emplace(off + len, rest);
                return static_cast<int64_t>(off);
            }
        }
        if (end + len > limits().scratch_cap) return -1;
        const uint64_t off = end;
        end += len;
        return static_cast<int64_t>(off);
    }

    void release(uint64_t off, uint64_t len) {
        std::lock_guard<std::mutex> lk(m);
        auto it = holes.emplace(off, len).first;
        auto nx = std::next(it);
        if (nx != holes.end() && it->first + it->second == nx->first) {
            it->second += nx->second;
            holes.erase(nx);
        }
        if (it != holes.begin()) {
            auto pv = std::prev(it);
            if (pv->first + pv->second == it->first) {
                pv->second += it->second;
                holes.erase(it);
                it = pv;
            }
        }
        if (it->first + it->second == end) {
            end = it->first;
            holes.erase(it);
            if (end == 0 && fd >= 0) (void)::ftruncate(fd, 0);
        }
    }

    bool write(uint64_t off, const char* p, size_t n) {
        while (n) {
            const ssize_t w = ::pwrite(fd, p, n, static_cast<off_t>(off));
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) return false;
            p += w;
            n -= static_cast<size_t>(w);
            off += static_cast<uint64_t>(w);
        }
        return true;
    }

    bool read(uint64_t off, char* p, size_t n) const {
        while (n) {
            const ssize_t r = ::pread(fd, p, n, static_cast<off_t>(off));
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) return false;
            p += r;
            n -= static_cast<size_t>(r);
            off += static_cast<uint64_t>(r);
        }
        return true;
    }

    uint64_t size() {
        std::lock_guard<std::mutex> lk(m);
        return end;
    }
};

Scratch& scratch() {
    static Scratch* s = new Scratch();  // never destroyed: cells may outlive static destructors
    return *s;
}

// ---- eviction exclusion ----------------------------------------------------------------------------------
std::shared_mutex& quiesce() {
    static std::shared_mutex* q = new std::shared_mutex();
    return *q;
}
thread_local int t_read_depth = 0;

void* alloc_bytes(size_t n) { return ::operator new(n, std::align_val_t(64)); }
void free_bytes(void* p) { ::operator delete(p, std::align_val_t(64)); }

}  // namespace

// ---- registry ------------------------------------------------------------------------------------------------
struct Registry {
    std::mutex m;
    Cell* head = nullptr;
    Cell* tail = nullptr;

    void add(Cell* c) {
        std::lock_guard<std::mutex> lk(m);
        c->prev_ = tail;
        c->next_ = nullptr;
        if (tail) tail->next_ = c;
        else head = c;
        tail = c;
    }
    void remove(Cell* c) {
        std::lock_guard<std::mutex> lk(m);
        if (c->prev_) c->prev_->next_ = c->next_;
        else head = c->next_;
        if (c->next_) c->next_->prev_ = c->prev_;
        else tail = c->prev_;
        c->prev_ = c->next_ = nullptr;
    }

    // Exclusive trim: callers hold the quiesce lock uniquely. Holding `m` for the whole pass keeps
    // every listed cell alive (a destructor must unlink first).
    size_t trim_to(uint64_t soft, uint64_t warm) {
        std::lock_guard<std::mutex> lk(m);
        size_t n = 0;
        if (g_hot.load() > soft) {
            std::vector<std::pair<uint64_t, Cell*>> v;
            for (Cell* c = head; c; c = c->next_)
                if (c->is_hot()) v.emplace_back(c->last_use_.load(std::memory_order_relaxed), c);
            std::stable_sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            for (auto& e : v) {
                if (g_hot.load() <= soft) break;
                e.second->evict();
                ++n;
            }
        }
        if (g_warm.load() > warm) {
            std::vector<std::pair<uint64_t, Cell*>> v;
            for (Cell* c = head; c; c = c->next_)
                if (c->is_warm()) v.emplace_back(c->last_use_.load(std::memory_order_relaxed), c);
            std::stable_sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            for (auto& e : v) {
                if (g_warm.load() <= warm) break;
                if (!e.second->spill()) break;  // scratch full or unavailable: the rest stays warm
                ++n;
            }
        }
        return n;
    }
};

namespace {
Registry& registry() {
    static Registry* r = new Registry();
    return *r;
}
}  // namespace

// ---- limits (public) --------------------------------------------------------------------------------------
uint64_t system_ram() {
    const long pages = ::sysconf(_SC_PHYS_PAGES);
    const long psize = ::sysconf(_SC_PAGE_SIZE);
    uint64_t ram = (pages > 0 && psize > 0) ? static_cast<uint64_t>(pages) * static_cast<uint64_t>(psize) : 4096 * kMiB;
    // cgroup v2: /proc/self/cgroup "0::/path" -> /sys/fs/cgroup/path/memory.max
    std::ifstream cg("/proc/self/cgroup");
    std::string line;
    while (std::getline(cg, line)) {
        if (line.rfind("0::", 0) != 0) continue;
        std::ifstream mm("/sys/fs/cgroup" + line.substr(3) + "/memory.max");
        std::string v;
        if (mm >> v && v != "max") {
            char* end = nullptr;
            const unsigned long long n = std::strtoull(v.c_str(), &end, 10);
            if (end && *end == '\0' && n > 0) ram = std::min<uint64_t>(ram, n);
        }
    }
    return ram;
}

Limits default_limits() {
    const uint64_t ram = system_ram();
    Limits l;
    l.hard = ram / 2;
    l.soft = ram / 50;
    l.warm = ram / 50;
    l.scratch_cap = 4096 * kMiB;
    env_mb("RASTERLOOM_MEM_HARD_MB", l.hard);
    env_mb("RASTERLOOM_MEM_SOFT_MB", l.soft);
    env_mb("RASTERLOOM_MEM_WARM_MB", l.warm);
    env_mb("RASTERLOOM_SCRATCH_CAP_MB", l.scratch_cap);
    if (const char* d = std::getenv("RASTERLOOM_SCRATCH_DIR"); d && *d) l.scratch_dir = d;
    if (const char* t = std::getenv("RASTERLOOM_MEM_TEST"); t && std::strcmp(t, "1") == 0) {
        l.soft = 0;
        l.warm = 0;
    }
    return l;
}

Limits limits() {
    ensure_limits();
    std::lock_guard<std::mutex> lk(g_limits_m);
    return g_limits;
}

void set_limits(const Limits& l) {
    std::lock_guard<std::mutex> lk(g_limits_m);
    g_limits = l;
    g_limits_set = true;
    g_hard.store(l.hard == 0 ? 1 : l.hard, std::memory_order_release);
}

Stats stats() {
    Stats s;
    s.hot_bytes = g_hot.load();
    s.warm_bytes = g_warm.load();
    s.cold_bytes = g_cold.load();
    s.reserved_bytes = g_reserved.load();
    s.scratch_file_bytes = scratch().size();
    s.peak_hot_bytes = g_peak_hot.load();
    s.peak_charged_bytes = g_peak_charged.load();
    s.cells = g_cells.load();
    s.compressions = g_compressions.load();
    s.spills = g_spills.load();
    s.faults = g_faults.load();
    s.spill_failures = g_spill_failures.load();
    return s;
}

void reset_peaks() {
    g_peak_hot.store(g_hot.load());
    g_peak_charged.store(g_charged.load());
}

// ---- Cell ------------------------------------------------------------------------------------------------------
Cell::Cell(size_t bytes, bool pinned) : bytes_(static_cast<uint32_t>(bytes)), pinned_(pinned) {
    if (!pinned_) charge(bytes_, "a tile");
    void* p = nullptr;
    try {
        p = alloc_bytes(bytes_);
    } catch (const std::bad_alloc&) {
        if (!pinned_) uncharge(bytes_);
        throw MemoryError("out of memory: the system refused a " + std::to_string(bytes_) + "-byte tile");
    }
    hot_.store(p, std::memory_order_release);
    last_use_.store(g_epoch.load(std::memory_order_relaxed), std::memory_order_relaxed);
    if (!pinned_) {
        add_hot(bytes_);
        g_cells.fetch_add(1, std::memory_order_relaxed);
        registry().add(this);
    }
}

Cell::~Cell() {
    if (!pinned_) registry().remove(this);
    if (void* p = hot_.load(std::memory_order_acquire)) {
        free_bytes(p);
        if (!pinned_) {
            g_hot.fetch_sub(bytes_, std::memory_order_relaxed);
            uncharge(bytes_);
        }
    }
    if (!pinned_) {
        drop_backups();
        g_cells.fetch_sub(1, std::memory_order_relaxed);
    }
}

void Cell::touch() const {
    const uint64_t e = g_epoch.load(std::memory_order_relaxed);
    if (last_use_.load(std::memory_order_relaxed) != e) last_use_.store(e, std::memory_order_relaxed);
}

bool Cell::is_warm() const {
    std::lock_guard<std::mutex> lk(m_);
    return !packed_.empty() && spill_off_ < 0;
}

bool Cell::is_cold() const {
    std::lock_guard<std::mutex> lk(m_);
    return spill_off_ >= 0;
}

const void* Cell::fault_in() const {
    std::lock_guard<std::mutex> lk(m_);
    if (void* p = hot_.load(std::memory_order_acquire)) return p;
    charge(bytes_, "a tile read back from compressed storage");
    char* dst = nullptr;
    try {
        dst = static_cast<char*>(alloc_bytes(bytes_));
    } catch (const std::bad_alloc&) {
        uncharge(bytes_);
        throw MemoryError("out of memory: the system refused a " + std::to_string(bytes_) + "-byte tile");
    }
    int got = -1;
    if (!packed_.empty()) {
        got = LZ4_decompress_safe(packed_.data(), dst, static_cast<int>(packed_.size()), static_cast<int>(bytes_));
    } else if (spill_off_ >= 0) {
        std::vector<char> buf(spill_len_);
        if (scratch().read(static_cast<uint64_t>(spill_off_), buf.data(), buf.size()))
            got = LZ4_decompress_safe(buf.data(), dst, static_cast<int>(buf.size()), static_cast<int>(bytes_));
    }
    if (got != static_cast<int>(bytes_)) {
        free_bytes(dst);
        uncharge(bytes_);
        throw std::runtime_error("tile storage corrupt: a compressed tile could not be restored");
    }
    add_hot(bytes_);
    g_faults.fetch_add(1, std::memory_order_relaxed);
    hot_.store(dst, std::memory_order_release);
    touch();
    return dst;
}

void Cell::drop_backups() {
    std::lock_guard<std::mutex> lk(m_);
    if (!packed_.empty()) {
        g_warm.fetch_sub(packed_.size(), std::memory_order_relaxed);
        uncharge(packed_.size());
        std::vector<char>().swap(packed_);
    }
    if (spill_off_ >= 0) {
        scratch().release(static_cast<uint64_t>(spill_off_), spill_len_);
        g_cold.fetch_sub(spill_len_, std::memory_order_relaxed);
        spill_off_ = -1;
        spill_len_ = 0;
    }
    has_backup_.store(false, std::memory_order_release);
}

void Cell::evict() {
    if (pinned_) return;
    std::lock_guard<std::mutex> lk(m_);
    void* p = hot_.load(std::memory_order_acquire);
    if (!p) return;
    if (packed_.empty() && spill_off_ < 0) {
        std::vector<char> buf(static_cast<size_t>(LZ4_compressBound(static_cast<int>(bytes_))));
        const int n = LZ4_compress_default(static_cast<const char*>(p), buf.data(), static_cast<int>(bytes_),
                                           static_cast<int>(buf.size()));
        if (n <= 0) return;  // cannot happen with a compressBound-sized buffer; stay hot
        packed_.assign(buf.begin(), buf.begin() + n);
        g_warm.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
        charge_nothrow(static_cast<uint64_t>(n));
        g_compressions.fetch_add(1, std::memory_order_relaxed);
        has_backup_.store(true, std::memory_order_release);
    }
    hot_.store(nullptr, std::memory_order_release);
    free_bytes(p);
    g_hot.fetch_sub(bytes_, std::memory_order_relaxed);
    uncharge(bytes_);
}

bool Cell::spill() {
    if (pinned_) return false;
    std::lock_guard<std::mutex> lk(m_);
    if (packed_.empty() || spill_off_ >= 0) return false;
    const int64_t off = scratch().alloc(packed_.size());
    if (off < 0) return false;
    if (!scratch().write(static_cast<uint64_t>(off), packed_.data(), packed_.size())) {
        scratch().release(static_cast<uint64_t>(off), packed_.size());
        g_spill_failures.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    spill_off_ = off;
    spill_len_ = static_cast<uint32_t>(packed_.size());
    g_cold.fetch_add(spill_len_, std::memory_order_relaxed);
    g_warm.fetch_sub(packed_.size(), std::memory_order_relaxed);
    uncharge(packed_.size());
    std::vector<char>().swap(packed_);
    g_spills.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// ---- Reservation ------------------------------------------------------------------------------------------------
Reservation::Reservation(uint64_t bytes, const char* what) : bytes_(0) {
    charge(bytes, what);
    g_reserved.fetch_add(bytes, std::memory_order_relaxed);
    bytes_ = bytes;
}

Reservation::~Reservation() {
    if (bytes_) {
        g_reserved.fetch_sub(bytes_, std::memory_order_relaxed);
        uncharge(bytes_);
    }
}

Reservation& Reservation::operator=(Reservation&& o) noexcept {
    if (this != &o) {
        if (bytes_) {
            g_reserved.fetch_sub(bytes_, std::memory_order_relaxed);
            uncharge(bytes_);
        }
        bytes_ = o.bytes_;
        o.bytes_ = 0;
    }
    return *this;
}

// ---- ReadScope / safe points ---------------------------------------------------------------------------------------
ReadScope::ReadScope() {
    if (t_read_depth++ == 0) quiesce().lock_shared();
}
ReadScope::~ReadScope() {
    if (--t_read_depth == 0) quiesce().unlock_shared();
}

size_t trim() {
    if (t_read_depth > 0) return 0;  // this thread is reading tiles: not a quiescent point
    const Limits l = limits();
    if (g_hot.load() <= l.soft && g_warm.load() <= l.warm) return 0;
    std::unique_lock<std::shared_mutex> q(quiesce());
    return registry().trim_to(l.soft, l.warm);
}

size_t safe_point() {
    g_epoch.fetch_add(1, std::memory_order_relaxed);
    return trim();
}

size_t evict_all(bool spill) {
    if (t_read_depth > 0) return 0;
    std::unique_lock<std::shared_mutex> q(quiesce());
    return registry().trim_to(0, spill ? 0 : UINT64_MAX);
}

}  // namespace rl::mem
