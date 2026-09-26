// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/selftest/selftest.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fnmatch.h>
#include <fstream>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>

#include "core/base/error.hpp"
#include "core/base/mutation.hpp"
#include "core/composite/render.hpp"
#include "core/io/export_png.hpp"
#include "core/io/png.hpp"
#include "core/script/engine.hpp"
#include "core/selftest/embedded.hpp"
#include "core/tile/memory.hpp"

namespace rl::selftest {

namespace {

using Json = nlohmann::json;

struct StatBound {
    std::string key;  // "all", "selection", "saved-selection", "<what>:<id>"
    size_t max_allocated = 0;
    std::optional<size_t> total;
};

struct Case {
    std::string name;
    std::string kind;  // render | error
    std::string equal_to;
    std::vector<StatBound> stats;
    const EmbeddedFile* script = nullptr;
    const EmbeddedFile* png = nullptr;
    int w = 0, h = 0;
};

std::vector<Case> load_cases() {
    const EmbeddedFile* mf = find_embedded("manifest.json");
    if (!mf) throw std::runtime_error("embedded corpus has no manifest.json");
    const Json m = Json::parse(std::string(mf->data, mf->size));
    std::vector<Case> out;
    for (const Json& c : m.at("cases")) {
        Case k;
        k.name = c.at("name").get<std::string>();
        k.kind = c.at("kind").get<std::string>();
        if (c.contains("equal_to") && !c["equal_to"].is_null()) k.equal_to = c["equal_to"].get<std::string>();
        if (c.contains("stats") && !c["stats"].is_null()) {
            for (auto it = c["stats"].begin(); it != c["stats"].end(); ++it) {
                StatBound b;
                b.key = it.key();
                b.max_allocated = it.value().at("max_allocated").get<size_t>();
                if (it.value().contains("total") && !it.value()["total"].is_null())
                    b.total = it.value()["total"].get<size_t>();
                k.stats.push_back(b);
            }
        }
        k.script = find_embedded(k.name + ".json");
        if (!k.script) throw std::runtime_error("embedded corpus lacks " + k.name + ".json");
        if (k.kind == "render") {
            k.png = find_embedded(k.name + ".png");
            if (!k.png) throw std::runtime_error("embedded corpus lacks " + k.name + ".png");
            k.w = c.at("w").get<int>();
            k.h = c.at("h").get<int>();
        } else if (k.kind != "error") {
            throw std::runtime_error("embedded case " + k.name + " has unknown kind " + k.kind);
        }
        out.push_back(std::move(k));
    }
    std::sort(out.begin(), out.end(), [](const Case& a, const Case& b) { return a.name < b.name; });
    return out;
}

// `--stats` keys exactly as rasterloom-cli prints them and tests/tools/run_goldens.py parses them.
std::map<std::string, std::pair<size_t, size_t>> stats_of(const Document& doc) {
    std::map<std::string, std::pair<size_t, size_t>> got;
    size_t alloc = 0, total = 0;
    for (const auto& s : doc.tile_stats()) {
        const std::string key = s.node_id.empty() ? s.what : s.what + ":" + s.node_id;
        got[key] = {s.allocated, s.total};
        alloc += s.allocated;
        total += s.total;
    }
    got["all"] = {alloc, total};
    return got;
}

std::string check_stats(const Case& c, const Document& doc) {
    if (c.stats.empty()) return {};
    const auto got = stats_of(doc);
    std::string bad;
    for (const StatBound& b : c.stats) {
        auto it = got.find(b.key);
        if (it == got.end()) {
            bad += b.key + ": no such tile plane; ";
            continue;
        }
        if (it->second.first > b.max_allocated)
            bad += b.key + ": allocated=" + std::to_string(it->second.first) + " > max " +
                   std::to_string(b.max_allocated) + "; ";
        if (b.total && it->second.second != *b.total)
            bad += b.key + ": total=" + std::to_string(it->second.second) + " != " + std::to_string(*b.total) + "; ";
    }
    return bad.empty() ? std::string() : "tile stats: " + bad;
}

std::string compare(const io::RgbaBuffer& want, const io::RgbaBuffer& got) {
    if (want.w != got.w || want.h != got.h)
        return "size " + std::to_string(got.w) + "x" + std::to_string(got.h) + " != expected " + std::to_string(want.w) +
               "x" + std::to_string(want.h);
    size_t diff = 0, first = 0;
    for (size_t i = 0; i < want.px.size(); ++i) {
        const Rgba8 a = want.px[i], b = got.px[i];
        if (a.r != b.r || a.g != b.g || a.b != b.b || a.a != b.a) {
            if (diff == 0) first = i;
            ++diff;
        }
    }
    if (diff == 0) return {};
    const int x = static_cast<int>(first % static_cast<size_t>(want.w));
    const int y = static_cast<int>(first / static_cast<size_t>(want.w));
    const Rgba8 a = want.px[first], b = got.px[first];
    char buf[160];
    std::snprintf(buf, sizeof buf, "%zu pixel(s) differ; first at (%d,%d): expected (%d,%d,%d,%d) got (%d,%d,%d,%d)", diff,
                  x, y, a.r, a.g, a.b, a.a, b.r, b.g, b.b, b.a);
    return buf;
}

void mkdirs(const std::string& path) {
    std::string cur;
    std::stringstream ss(path);
    std::string part;
    if (!path.empty() && path[0] == '/') cur = "/";
    while (std::getline(ss, part, '/')) {
        if (part.empty()) continue;
        cur += part + "/";
        ::mkdir(cur.c_str(), 0755);
    }
}

void dump_failure(const std::string& dir, const Case& c, const io::RgbaBuffer* got) {
    if (dir.empty()) return;
    std::string safe = c.name;
    std::replace(safe.begin(), safe.end(), '/', '_');
    const std::string d = dir + "/" + safe;
    mkdirs(d);
    try {
        if (got) io::write_png(d + "/actual.png", *got);
        if (c.png) {
            std::ofstream f(d + "/expected.png", std::ios::binary);
            f.write(c.png->data, static_cast<std::streamsize>(c.png->size));
        }
        std::ofstream s(d + "/script.json", std::ios::binary);
        s.write(c.script->data, static_cast<std::streamsize>(c.script->size));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "selftest: cannot dump %s: %s\n", c.name.c_str(), e.what());
    }
}

std::string xml_escape(const std::string& s) {
    std::string o;
    for (char ch : s) {
        switch (ch) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20 && ch != '\n' && ch != '\t') o += '?';
                else o += ch;
        }
    }
    return o;
}

class MutationScope {
public:
    explicit MutationScope(const std::vector<int>& ids) : saved_(mut::active_list()) { mut::set_active(ids); }
    ~MutationScope() { mut::set_active(saved_); }
    MutationScope(const MutationScope&) = delete;
    MutationScope& operator=(const MutationScope&) = delete;

private:
    std::vector<int> saved_;
};

}  // namespace

size_t case_count() {
    try {
        return load_cases().size();
    } catch (const std::exception&) {
        return 0;
    }
}

Report run(const Options& o) {
    const auto t0 = std::chrono::steady_clock::now();
    const std::vector<Case> cases = load_cases();
    MutationScope ms(o.mutations);

    std::set<std::string> selected;
    for (const Case& c : cases)
        if (o.filter.empty() || fnmatch(o.filter.c_str(), c.name.c_str(), 0) == 0) selected.insert(c.name);
    // equal_to targets are needed even when the filter excludes them.
    for (const Case& c : cases)
        if (selected.count(c.name) && !c.equal_to.empty()) selected.insert(c.equal_to);
    std::set<std::string> keep;  // renders needed for equal_to
    for (const Case& c : cases)
        if (selected.count(c.name) && !c.equal_to.empty()) {
            keep.insert(c.name);
            keep.insert(c.equal_to);
        }

    Report rep;
    std::map<std::string, io::RgbaBuffer> kept;
    std::map<std::string, size_t> index;
    for (const Case& c : cases) {
        if (!selected.count(c.name)) continue;
        CaseResult r;
        r.name = c.name;
        r.kind = c.kind;
        const auto c0 = std::chrono::steady_clock::now();
        std::optional<io::RgbaBuffer> got;
        try {
            script::ScriptResult sr = script::run_script_text(std::string(c.script->data, c.script->size));
            if (c.kind == "error") {
                r.message = "expected a script error, but the script rendered";
            } else {
                got = io::render_document(sr.doc->state());
                const io::RgbaBuffer want = io::decode_png(
                    std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(c.png->data),
                                         reinterpret_cast<const uint8_t*>(c.png->data) + c.png->size));
                r.message = compare(want, *got);
                if (r.message.empty()) r.message = check_stats(c, *sr.doc);
            }
        } catch (const ScriptError& e) {
            if (c.kind != "error") r.message = std::string("script error: ") + e.what();
        } catch (const std::exception& e) {
            r.message = std::string(c.kind == "error" ? "internal error instead of a script error: " : "internal error: ") +
                        e.what();
        }
        // Tier the memory between cases exactly as between ops (RASTERLOOM_MEM_TEST exercises it).
        mem::safe_point();
        r.passed = r.message.empty();
        r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c0).count();
        if (!r.passed) dump_failure(o.dump_dir, c, got ? &*got : nullptr);
        if (got && keep.count(c.name)) kept[c.name] = std::move(*got);
        index[c.name] = rep.cases.size();
        rep.cases.push_back(std::move(r));
        if (o.fail_fast && !rep.cases.back().passed) break;
    }
    // equal_to post-pass.
    for (const Case& c : cases) {
        if (c.equal_to.empty() || !index.count(c.name)) continue;
        CaseResult& r = rep.cases[index[c.name]];
        if (!r.passed) continue;
        auto a = kept.find(c.name), b = kept.find(c.equal_to);
        if (b == kept.end()) {
            r.passed = false;
            r.message = "equal_to " + c.equal_to + ": target did not render";
            continue;
        }
        const std::string d = compare(b->second, a->second);
        if (!d.empty()) {
            r.passed = false;
            r.message = "equal_to " + c.equal_to + ": " + d;
        }
    }
    for (const CaseResult& r : rep.cases)
        if (!r.passed) ++rep.failed;
    rep.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return rep;
}

bool write_junit(const Report& r, const std::string& path, const std::string& suite_name) {
    std::ofstream x(path, std::ios::binary);
    if (!x) {
        std::fprintf(stderr, "selftest: cannot write %s\n", path.c_str());
        return false;
    }
    char tbuf[64];
    std::snprintf(tbuf, sizeof tbuf, "%.3f", r.seconds);
    x << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<testsuites>\n<testsuite name=\"" << xml_escape(suite_name)
      << "\" tests=\"" << r.cases.size() << "\" failures=\"" << r.failed << "\" errors=\"0\" time=\"" << tbuf << "\">\n";
    for (const CaseResult& c : r.cases) {
        const auto slash = c.name.find('/');
        const std::string cls = slash == std::string::npos ? "selftest" : "selftest." + c.name.substr(0, slash);
        std::snprintf(tbuf, sizeof tbuf, "%.3f", c.ms / 1000.0);
        x << "  <testcase classname=\"" << xml_escape(cls) << "\" name=\"" << xml_escape(c.name) << "\" time=\"" << tbuf
          << "\"";
        if (c.passed) {
            x << "/>\n";
        } else {
            x << ">\n    <failure message=\"" << xml_escape(c.message) << "\"/>\n  </testcase>\n";
        }
    }
    x << "</testsuite>\n</testsuites>\n";
    x.flush();
    if (!x) {
        std::fprintf(stderr, "selftest: write error on %s\n", path.c_str());
        return false;
    }
    return true;
}

bool requested(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--selftest") == 0 || std::strncmp(argv[i], "--selftest-mutate", 17) == 0) return true;
    return false;
}

int run_main(int argc, char** argv, const char* prog) {
    Options o;
    auto value = [&](int& i, const char* flag, std::string& out) -> bool {
        const size_t n = std::strlen(flag);
        if (std::strncmp(argv[i], flag, n) != 0) return false;
        if (argv[i][n] == '=') {
            out = argv[i] + n + 1;
            return true;
        }
        if (argv[i][n] == '\0') {
            if (i + 1 >= argc) throw std::invalid_argument(std::string(flag) + " needs a value");
            out = argv[++i];
            return true;
        }
        return false;
    };
    try {
        if (const char* env = std::getenv("RASTERLOOM_MUTATE"); env && *env)
            for (int id : mut::parse_list(env)) o.mutations.push_back(id);
        for (int i = 1; i < argc; ++i) {
            std::string v;
            if (value(i, "--selftest-mutate", v) || value(i, "--mutate", v)) {
                for (int id : mut::parse_list(v)) o.mutations.push_back(id);
            } else if (value(i, "--junit-xml", v)) {
                o.junit_xml = v;
            } else if (value(i, "--selftest-filter", v)) {
                o.filter = v;
            } else if (value(i, "--selftest-dump", v)) {
                o.dump_dir = v;
            } else if (std::strcmp(argv[i], "--selftest-fail-fast") == 0) {
                o.fail_fast = true;
            } else if (std::strcmp(argv[i], "--selftest-quiet") == 0) {
                o.quiet = true;
            } else if (std::strcmp(argv[i], "--deterministic") == 0) {
                composite::force_deterministic();
            }
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s: %s\n", prog, e.what());
        return 2;
    }
    std::sort(o.mutations.begin(), o.mutations.end());
    o.mutations.erase(std::unique(o.mutations.begin(), o.mutations.end()), o.mutations.end());

    Report rep;
    try {
        rep = run(o);
    } catch (const std::exception& e) {
        std::printf("selftest: broken embedded corpus: %s\nselftest: 0 passed, 1 failed\n", e.what());
        return 1;
    }
    std::printf("selftest: %zu embedded cases, scheduler=%s%s\n", rep.cases.size(), composite::scheduler().name(),
                composite::deterministic_forced() ? " (--deterministic)" : "");
    for (const CaseResult& c : rep.cases) {
        if (!c.passed)
            std::printf("FAIL %s: %s\n", c.name.c_str(), c.message.c_str());
        else if (!o.quiet)
            std::printf("ok   %s (%.1f ms)\n", c.name.c_str(), c.ms);
    }
    if (!o.mutations.empty()) {
        std::printf("selftest: MUTATIONS ACTIVE:");
        for (int id : o.mutations) std::printf(" %d (%s)", id, mut::description(id));
        std::printf("\nselftest: mutation caught by %d case(s)%s\n", rep.failed,
                    rep.failed == 0 ? " - SUITE IS VACUOUS FOR THIS MUTATION" : "");
    }
    const mem::Stats ms = mem::stats();
    std::printf("selftest: memory: peak tiles %.1f MiB, compressed %llu, spilled %llu, faults %llu\n",
                static_cast<double>(ms.peak_hot_bytes) / (1024.0 * 1024.0), static_cast<unsigned long long>(ms.compressions),
                static_cast<unsigned long long>(ms.spills), static_cast<unsigned long long>(ms.faults));
    std::printf("selftest: %zu passed, %d failed (%.2f s)\n", rep.cases.size() - static_cast<size_t>(rep.failed), rep.failed,
                rep.seconds);
    if (!o.junit_xml.empty() && !write_junit(rep, o.junit_xml, std::string(prog) + "-selftest")) return 1;
    if (rep.cases.empty()) return 1;
    return rep.failed == 0 ? 0 : 1;
}

}  // namespace rl::selftest
