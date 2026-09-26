// SPDX-License-Identifier: GPL-3.0-or-later
//
// The embedded self-test (BUILD-SPEC requirement 14 and <verification>): every golden of
// tests/scripts, frozen by tools/freeze_goldens.py into tests/goldens/ (the stripped script plus
// the PNG the independent NumPy reference renders), is compiled into this library. `--selftest`
// renders each script in-process with the core and requires
//   - render cases: decoded RGBA byte-equal to the embedded reference PNG;
//   - expect-error cases: the script is rejected (ScriptError);
//   - equal_to pairs: the two renders are byte-equal;
//   - tile-statistics bounds (the golden-level catch for mutation 13).
// Exit status 0 only when every case passes; so `--selftest --selftest-mutate=N` must be non-zero
// for every mutation id N (the mutation gate, run by CTest selftest_mutation_loop).
//
// The library has no Qt dependency; both rasterloom-cli and the GUI binary call run_main().
#pragma once

#include <string>
#include <vector>

namespace rl::selftest {

struct Options {
    std::vector<int> mutations;  // activated for the whole run (empty = none)
    std::string junit_xml;       // write a JUnit XML report here when non-empty
    std::string filter;          // glob on case names ("compositing/*"); empty = all
    std::string dump_dir;        // write actual.png / expected.png of failing cases here
    bool fail_fast = false;      // stop at the first failing case
    bool quiet = false;          // print only failures and the summary
};

struct CaseResult {
    std::string name;
    std::string kind;  // "render" | "error"
    bool passed = false;
    std::string message;  // failure reason (empty on pass)
    double ms = 0.0;
};

struct Report {
    std::vector<CaseResult> cases;
    int failed = 0;
    double seconds = 0.0;
};

// Number of embedded cases (0 would mean a broken build; run() then fails).
size_t case_count();

// Runs the embedded cases. Mutations in `o.mutations` are activated for the duration and the
// previous set is restored. Never throws for a failing case; throws std::runtime_error only when
// the embedded corpus itself is unreadable.
Report run(const Options& o);

// Writes `r` as JUnit XML. Returns false (and prints to stderr) on an I/O error.
bool write_junit(const Report& r, const std::string& path, const std::string& suite_name);

// Command-line entry shared by both binaries. Recognised arguments (all others are ignored, so the
// caller may pass its whole argv): --selftest, --selftest-mutate=N[,M] / --selftest-mutate N,
// --junit-xml=F / --junit-xml F, --selftest-filter=GLOB, --selftest-dump=DIR,
// --selftest-fail-fast, --selftest-quiet, --deterministic, --mutate=N (merged with the
// mutate list), and RASTERLOOM_MUTATE from the environment. Prints a summary line
// "selftest: P passed, F failed" and returns the process exit code: 0 all passed, 1 any failure,
// 2 usage error.
int run_main(int argc, char** argv, const char* prog);

// True when argv contains --selftest or --selftest-mutate[=...].
bool requested(int argc, char** argv);

}  // namespace rl::selftest
