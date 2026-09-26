// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/selftest/bench.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>

#include "core/base/error.hpp"
#include "core/io/export_png.hpp"
#include "core/script/engine.hpp"
#include "core/script/fields.hpp"
#include "core/script/registry.hpp"
#include "core/tile/memory.hpp"

namespace rl::bench {

namespace {

using Clock = std::chrono::steady_clock;

struct Def {
    std::string name;
    std::string about;
    std::function<std::string()> setup;  // render script (JSON text) building the document
    std::string op;                      // one op (JSON text), or "render" = full-canvas composite
};

std::string canvas(int w, int h, const char* bg = "#ffffffff") {
    return "{\"canvas\":{\"w\":" + std::to_string(w) + ",\"h\":" + std::to_string(h) + ",\"bg\":\"" + bg + "\"},\"ops\":[";
}

// One full-canvas layer "p": a horizontal gradient with uniform noise on top (so no tile is flat).
std::string photo_like(int w, int h) {
    return canvas(w, h) +
           "{\"op\":\"add_layer\",\"id\":\"p\",\"fill\":\"gradient\",\"from\":\"#d04020ff\",\"to\":\"#2060c0ff\",\"dir\":\"h\"},"
           "{\"op\":\"filter_add_noise\",\"layer\":\"p\",\"amount\":30.0,\"distribution\":\"uniform\",\"seed\":1}],"
           "\"out\":\"png8\"}";
}

std::string ten_layers(int w, int h) {
    static const char* modes[10] = {"norm", "mul", "scrn", "over", "sLit", "dark", "lite", "diff", "hue", "lum"};
    static const char* from[10] = {"#202020ff", "#ff8040ff", "#4080ffc0", "#ffffff80", "#80ff40ff",
                                   "#c0c0ffff", "#101010ff", "#ff00ffa0", "#00ffffff", "#ffff0080"};
    static const char* to[10] = {"#f0f0f0ff", "#4080ffff", "#ff804060", "#000000ff", "#8000ff80",
                                 "#ffc0c0ff", "#606060ff", "#00ff00ff", "#ff0000c0", "#0000ffff"};
    std::string s = canvas(w, h);
    for (int i = 0; i < 10; ++i) {
        const std::string id = "L" + std::to_string(i);
        if (i) s += ",";
        s += "{\"op\":\"add_layer\",\"id\":\"" + id + "\",\"fill\":\"gradient\",\"from\":\"" + from[i] + "\",\"to\":\"" + to[i] +
             "\",\"dir\":\"" + (i % 2 ? "v" : "h") + "\"}";
        if (i) {
            s += ",{\"op\":\"set_blend\",\"layer\":\"" + id + "\",\"mode\":\"" + modes[i] + "\"}";
            s += ",{\"op\":\"set_opacity\",\"layer\":\"" + id + "\",\"value\":0.8}";
        }
    }
    return s + "],\"out\":\"png8\"}";
}

// Size 200, default spacing 25% = 50 px: a boustrophedon path of exactly 49,950 px -> 1000 dabs.
std::string stroke_1000_dabs() {
    std::string s = "{\"op\":\"brush_stroke\",\"layer\":\"p\",\"size\":200.0,\"hardness\":0.8,\"color\":\"#203040\",\"samples\":[";
    double len = 0.0, t = 0.0;
    double x = 200.0, y = 200.0;
    bool first = true;
    auto emit = [&](double px, double py) {
        char buf[128];
        std::snprintf(buf, sizeof buf, "%s{\"x\":%.1f,\"y\":%.1f,\"pressure\":1.0,\"t_ms\":%.1f}", first ? "" : ",", px, py, t);
        s += buf;
        first = false;
        t += 4.0;
    };
    emit(x, y);
    const double target = 49950.0;
    int dir = 1;
    while (len < target) {
        // horizontal run of up to 3600 px in 100 px samples, then a 200 px step down
        for (int k = 0; k < 36 && len < target; ++k) {
            const double step = std::min(100.0, target - len);
            x += dir * step;
            len += step;
            emit(x, y);
        }
        if (len >= target) break;
        const double step = std::min(200.0, target - len);
        y += step;
        len += step;
        emit(x, y);
        dir = -dir;
    }
    return s + "]}";
}

const std::vector<Def>& defs() {
    static const std::vector<Def> d = {
        {"composite_4000x3000_x10", "full-canvas composite of 10 gradient layers (9 blend modes, opacity 0.8)",
         [] { return ten_layers(4000, 3000); }, "render"},
        {"gaussian_r50_4000x3000", "filter_gaussian_blur radius 50 on a 4000x3000 layer",
         [] { return photo_like(4000, 3000); }, "{\"op\":\"filter_gaussian_blur\",\"layer\":\"p\",\"radius\":50.0}"},
        {"brush_1000dabs_size200", "one brush_stroke, 1000 dabs of size 200 (spacing 25%) on 4000x3000",
         [] { return canvas(4000, 3000) + "{\"op\":\"add_layer\",\"id\":\"p\"}],\"out\":\"png8\"}"; }, stroke_1000_dabs()},
        {"image_size_4000x3000_to_2000x1500", "image_size 4000x3000 -> 2000x1500",
         [] { return photo_like(4000, 3000); }, "{\"op\":\"image_size\",\"w\":2000,\"h\":1500}"},
        {"gaussian_r50_8000x8000", "large image: filter_gaussian_blur radius 50 on 8000x8000",
         [] { return photo_like(8000, 8000); }, "{\"op\":\"filter_gaussian_blur\",\"layer\":\"p\",\"radius\":50.0}"},
        {"motion_d30_8000x8000", "large image: filter_motion_blur distance 30, angle 30 on 8000x8000",
         [] { return photo_like(8000, 8000); },
         "{\"op\":\"filter_motion_blur\",\"layer\":\"p\",\"angle\":30.0,\"distance\":30.0}"},
        {"image_size_8000_to_5000", "large image: image_size 8000x8000 -> 5000x5000",
         [] { return photo_like(8000, 8000); }, "{\"op\":\"image_size\",\"w\":5000,\"h\":5000}"},
        {"rotate_15_8000x8000", "large image: rotate_canvas 15 degrees on 8000x8000",
         [] { return photo_like(8000, 8000); }, "{\"op\":\"rotate_canvas\",\"angle\":15.0}"},
    };
    return d;
}

long proc_status_kib(const char* key) {
    std::ifstream f("/proc/self/status");
    std::string line;
    const size_t n = std::strlen(key);
    while (std::getline(f, line)) {
        if (line.compare(0, n, key) == 0 && line.size() > n && line[n] == ':') return std::atol(line.c_str() + n + 1);
    }
    return -1;
}

bool reset_peak() {
    std::ofstream f("/proc/self/clear_refs");
    if (!f) return false;
    f << "5";
    f.flush();
    return static_cast<bool>(f);
}

}  // namespace

std::vector<CaseInfo> cases() {
    std::vector<CaseInfo> v;
    for (const Def& d : defs()) v.push_back({d.name, d.about});
    return v;
}

std::string case_setup(const std::string& name) {
    for (const Def& d : defs())
        if (d.name == name) return d.setup();
    return {};
}

std::string case_op(const std::string& name) {
    for (const Def& d : defs())
        if (d.name == name) return d.op;
    return {};
}

Result run_case(const std::string& name, int repeat) {
    Result r;
    r.name = name;
    auto it = std::find_if(defs().begin(), defs().end(), [&](const Def& d) { return d.name == name; });
    if (it == defs().end()) {
        r.error = "unknown case";
        return r;
    }
    try {
        for (int rep = 0; rep < std::max(1, repeat); ++rep) {
            const auto s0 = Clock::now();
            script::ScriptResult sr = script::run_script_text(it->setup());
            Document& doc = *sr.doc;
            if (rep == 0) r.setup_ms = std::chrono::duration<double, std::milli>(Clock::now() - s0).count();
            mem::safe_point();  // as the engine would before the next op
            if (rep == 0) {
                r.rss_before_kib = proc_status_kib("VmRSS");
                r.peak_is_step = reset_peak();
            }
            const auto t0 = Clock::now();
            if (it->op == "render") {
                const io::RgbaBuffer img = io::render_document(doc.state());
                if (img.px.empty()) throw std::runtime_error("empty render");
            } else {
                const script::Json op = script::Json::parse(it->op);
                const std::string opname = op.at("op").get<std::string>();
                const script::OpSpec* spec = script::default_registry().find(opname);
                if (!spec) throw std::runtime_error("op " + opname + " not registered");
                script::OpContext ctx{doc, 0, "bench (" + opname + ")"};
                script::Fields f(op, ctx.label);
                f.consume("op");
                if (spec->records_history) doc.push_history();
                spec->run(ctx, f);
                f.finish();
            }
            r.ms.push_back(std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
            if (rep == 0) r.peak_kib = proc_status_kib("VmHWM");
        }
    } catch (const std::exception& e) {
        r.error = e.what();
    }
    return r;
}

bool requested(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--bench") == 0 || std::strcmp(argv[i], "--bench-list") == 0) return true;
    return false;
}

int run_main(int argc, char** argv) {
    std::vector<std::string> names;
    int repeat = 1;
    std::string json_path;
    bool list = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&](const char* flag) -> std::string {
            const std::string f(flag);
            if (a.rfind(f + "=", 0) == 0) return a.substr(f.size() + 1);
            if (a == f && i + 1 < argc) return argv[++i];
            return {};
        };
        if (a == "--bench-list") list = true;
        else if (a.rfind("--bench-case", 0) == 0) names.push_back(val("--bench-case"));
        else if (a.rfind("--bench-repeat", 0) == 0) repeat = std::max(1, std::atoi(val("--bench-repeat").c_str()));
        else if (a.rfind("--bench-json", 0) == 0) json_path = val("--bench-json");
    }
    if (list) {
        for (const CaseInfo& c : cases()) std::printf("%s\t%s\n", c.name.c_str(), c.about.c_str());
        return 0;
    }
    if (names.empty())
        for (const CaseInfo& c : cases()) names.push_back(c.name);
    std::ostringstream js;
    js << "[";
    int failed = 0;
    for (size_t k = 0; k < names.size(); ++k) {
        const Result r = run_case(names[k], repeat);
        if (!r.error.empty()) {
            std::printf("%-36s ERROR %s\n", r.name.c_str(), r.error.c_str());
            ++failed;
        } else {
            std::vector<double> s = r.ms;
            std::sort(s.begin(), s.end());
            std::printf("%-36s min %9.1f ms  median %9.1f ms  (n=%zu)  step peak RSS %7.1f MiB%s  (doc before %7.1f MiB)\n",
                        r.name.c_str(), s.front(), s[s.size() / 2], s.size(), r.peak_kib / 1024.0,
                        r.peak_is_step ? "" : " [process peak]", r.rss_before_kib / 1024.0);
        }
        std::fflush(stdout);
        js << (k ? "," : "") << "\n {\"name\":\"" << r.name << "\",\"ms\":[";
        for (size_t i = 0; i < r.ms.size(); ++i) js << (i ? "," : "") << r.ms[i];
        js << "],\"setup_ms\":" << r.setup_ms << ",\"rss_before_kib\":" << r.rss_before_kib << ",\"peak_kib\":" << r.peak_kib
           << ",\"peak_is_step\":" << (r.peak_is_step ? "true" : "false") << ",\"error\":\"";
        for (char ch : r.error) js << ((ch == '"' || ch == '\\') ? ' ' : ch);
        js << "\"}";
    }
    js << "\n]\n";
    if (!json_path.empty()) {
        std::ofstream f(json_path);
        f << js.str();
    }
    return failed ? 1 : 0;
}

}  // namespace rl::bench
