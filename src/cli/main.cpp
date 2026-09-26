// SPDX-License-Identifier: GPL-3.0-or-later
//
// rasterloom-cli: the headless driver (BUILD-SPEC req 1 / req 14). No Qt, no display.
//
//   rasterloom-cli --render-script X.json --out Y.png [--stats] [--mutate=N[,M...]] [--save-doc DOC]
//   rasterloom-cli --convert IN OUT        (formats by content / by OUT's extension)
//   rasterloom-cli --render-file IN --out Y.png   (open any format, write the composite PNG)
//   rasterloom-cli --dump-tree IN          (layer tree JSON on stdout)
//   rasterloom-cli --version
//   rasterloom-cli --list-mutations
//   rasterloom-cli --selftest [--selftest-mutate=N] [--junit-xml=F]   (embedded golden corpus)
//   rasterloom-cli --bench [--bench-case NAME] [--bench-repeat N] [--bench-json F] [--bench-list]
//   --deterministic (any mode): force the single-threaded row-major composite scheduler
//
// Mutations: --mutate=N (comma-separated list allowed) and/or RASTERLOOM_MUTATE=N; both are merged.
// Exit codes: 0 success, 1 script error (no PNG written), 2 usage error, 3 I/O error,
// 4 internal error (no PNG written), 5 out of memory (the hard limit refused an allocation; no PNG).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "core/base/error.hpp"
#include "core/base/mutation.hpp"
#include "core/base/version.hpp"
#include "core/composite/render.hpp"
#include "core/io/export_png.hpp"
#include "core/io/file_io.hpp"
#include "core/io/io_error.hpp"
#include "core/script/engine.hpp"
#include "core/selftest/bench.hpp"
#include "core/selftest/selftest.hpp"
#include "core/tile/memory.hpp"

namespace {

void usage(std::ostream& os) {
    os << "usage: rasterloom-cli --render-script SCRIPT.json --out OUT.png [--stats] [--mutate=N[,M...]]\n"
          "       rasterloom-cli --convert IN OUT      (.orp .ora .psd .psb .png .jpg .tif)\n"
          "       rasterloom-cli --render-file IN --out OUT.png\n"
          "       rasterloom-cli --dump-tree IN\n"
          "       (--render-script also takes --save-doc DOC to save the resulting document)\n"
          "       rasterloom-cli --version\n"
          "       rasterloom-cli --list-mutations\n"
          "       rasterloom-cli --selftest [--selftest-mutate=N] [--junit-xml=F]\n"
          "       rasterloom-cli --bench [--bench-case NAME] [--bench-repeat N] [--bench-json F] [--bench-list]\n"
          "       --deterministic: force the single-threaded row-major composite scheduler\n"
          "environment: RASTERLOOM_MUTATE=N[,M...] also activates mutations\n";
}

void print_stats(const rl::Document& doc) {
    size_t alloc = 0, total = 0;
    for (const auto& s : doc.tile_stats()) {
        std::cout << "stats: " << s.what;
        if (!s.node_id.empty()) std::cout << " '" << s.node_id << "'";
        std::cout << " allocated=" << s.allocated << " total=" << s.total << "\n";
        alloc += s.allocated;
        total += s.total;
    }
    std::cout << "stats: all allocated=" << alloc << " total=" << total << "\n";
}

void print_warnings(const std::vector<std::string>& w) {
    for (const std::string& m : w) std::cerr << "rasterloom-cli: warning: " << m << "\n";
}

// --convert / --render-file / --dump-tree. Exit codes: 0 ok, 3 I/O error, 4 internal error.
int run_file_mode(const std::string& mode, const std::string& in, const std::string& out) {
    try {
        rl::io::OpenResult r = rl::io::open_document(in);
        print_warnings(r.warnings);
        if (mode == "dump") {
            std::cout << rl::io::dump_tree_json(r.doc->state());
        } else if (mode == "convert") {
            print_warnings(rl::io::save_document(r.doc->state(), out));
        } else {
            rl::io::write_document_png(r.doc->state(), out);
        }
    } catch (const rl::io::IoError& e) {
        std::cerr << "rasterloom-cli: " << e.what() << "\n";
        return 3;
    } catch (const rl::mem::MemoryError& e) {
        std::cerr << "rasterloom-cli: " << e.what() << "\n";
        return 5;
    } catch (const std::exception& e) {
        std::cerr << "rasterloom-cli: internal error: " << e.what() << "\n";
        return 4;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (rl::selftest::requested(argc, argv)) return rl::selftest::run_main(argc, argv, "rasterloom-cli");
    if (rl::bench::requested(argc, argv)) return rl::bench::run_main(argc, argv);

    std::string script_path, out_path, save_doc, convert_in, convert_out, render_file, dump_tree;
    bool stats = false, version = false, list_mut = false;
    std::vector<int> mutations;

    try {
        if (const char* env = std::getenv("RASTERLOOM_MUTATE"); env && *env) {
            for (int id : rl::mut::parse_list(env)) mutations.push_back(id);
        }
    } catch (const std::exception& e) {
        std::cerr << "rasterloom-cli: RASTERLOOM_MUTATE: " << e.what() << "\n";
        return 2;
    }

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* flag) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "rasterloom-cli: " << flag << " needs a value\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--render-script") {
            script_path = next("--render-script");
        } else if (a.rfind("--render-script=", 0) == 0) {
            script_path = a.substr(16);
        } else if (a == "--out") {
            out_path = next("--out");
        } else if (a.rfind("--out=", 0) == 0) {
            out_path = a.substr(6);
        } else if (a == "--save-doc") {
            save_doc = next("--save-doc");
        } else if (a == "--convert") {
            convert_in = next("--convert");
            convert_out = next("--convert");
        } else if (a == "--render-file") {
            render_file = next("--render-file");
        } else if (a == "--dump-tree") {
            dump_tree = next("--dump-tree");
        } else if (a == "--stats") {
            stats = true;
        } else if (a == "--deterministic") {
            rl::composite::force_deterministic();
        } else if (a == "--version") {
            version = true;
        } else if (a == "--list-mutations") {
            list_mut = true;
        } else if (a == "--mutate" || a.rfind("--mutate=", 0) == 0) {
            const std::string v = (a == "--mutate") ? next("--mutate") : a.substr(9);
            try {
                for (int id : rl::mut::parse_list(v)) mutations.push_back(id);
            } catch (const std::exception& e) {
                std::cerr << "rasterloom-cli: --mutate: " << e.what() << "\n";
                return 2;
            }
        } else if (a == "-h" || a == "--help") {
            usage(std::cout);
            return 0;
        } else {
            std::cerr << "rasterloom-cli: unknown argument '" << a << "'\n";
            usage(std::cerr);
            return 2;
        }
    }

    if (version) {
        std::cout << "rasterloom-cli " << rl::kVersion << "\n";
        return 0;
    }
    if (list_mut) {
        for (int id = 0; id < rl::mut::kCount; ++id)
            std::cout << id << "\t" << rl::mut::owner(id) << "\t" << rl::mut::description(id) << "\n";
        return 0;
    }
    if (!convert_in.empty()) return run_file_mode("convert", convert_in, convert_out);
    if (!dump_tree.empty()) return run_file_mode("dump", dump_tree, "");
    if (!render_file.empty()) {
        if (out_path.empty()) {
            usage(std::cerr);
            return 2;
        }
        return run_file_mode("render", render_file, out_path);
    }
    if (script_path.empty() || out_path.empty()) {
        usage(std::cerr);
        return 2;
    }

    rl::mut::set_active(mutations);  // once, before any document exists
    if (!mutations.empty()) {
        std::cerr << "rasterloom-cli: MUTATIONS ACTIVE:";
        for (int id : rl::mut::active_list()) std::cerr << " " << id;
        std::cerr << "\n";
    }

    rl::script::ScriptResult res;
    try {
        res = rl::script::run_script_file(script_path);
    } catch (const rl::ScriptError& e) {
        std::cerr << "rasterloom-cli: script error: " << e.what() << "\n";
        return 1;
    } catch (const rl::mem::MemoryError& e) {
        std::cerr << "rasterloom-cli: " << e.what() << "\n";
        return 5;
    } catch (const std::exception& e) {
        std::cerr << "rasterloom-cli: internal error: " << e.what() << "\n";
        return 4;
    }

    try {
        rl::io::write_document_png(res.doc->state(), out_path);
    } catch (const rl::mem::MemoryError& e) {
        std::cerr << "rasterloom-cli: " << e.what() << "\n";
        return 5;
    } catch (const std::exception& e) {
        std::cerr << "rasterloom-cli: " << e.what() << "\n";
        return 3;
    }
    if (stats) print_stats(*res.doc);
    if (!save_doc.empty()) {
        try {
            print_warnings(rl::io::save_document(res.doc->state(), save_doc));
        } catch (const std::exception& e) {
            std::cerr << "rasterloom-cli: " << e.what() << "\n";
            return 3;
        }
    }
    return 0;
}
