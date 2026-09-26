// SPDX-License-Identifier: GPL-3.0-or-later
//
// rasterloom-cli --bench: fixed performance cases run in-process. Each case builds its document
// with a setup render script, then times ONE step (an op through the script registry, exactly as
// the engine runs it, or the full-canvas composite) and measures the peak resident set of that step
// (VmHWM after resetting it through /proc/self/clear_refs). tools/bench.py runs every case in a
// fresh process and writes the table used in docs/PERF.md.
#pragma once

#include <string>
#include <vector>

namespace rl::bench {

struct CaseInfo {
    std::string name;
    std::string about;
};

std::vector<CaseInfo> cases();

// The setup render script and the timed op (JSON text, or "render") of a case ("" if unknown).
std::string case_setup(const std::string& name);
std::string case_op(const std::string& name);

struct Result {
    std::string name;
    std::vector<double> ms;  // one per repeat
    double setup_ms = 0.0;
    long rss_before_kib = -1;  // VmRSS right before the timed step (setup document live)
    long peak_kib = -1;        // VmHWM during the timed step (-1 when unavailable)
    bool peak_is_step = false;  // true when clear_refs reset worked (peak belongs to the step)
    std::string error;
};

// Runs one case `repeat` times (setup is rebuilt each time; peak memory is taken from the first).
Result run_case(const std::string& name, int repeat);

// Entry for rasterloom-cli: --bench [--bench-case NAME]... [--bench-repeat N] [--bench-list]
// [--bench-json FILE]. Prints one line per case (and JSON when asked). Exit 0 unless a case failed.
int run_main(int argc, char** argv);

bool requested(int argc, char** argv);

}  // namespace rl::bench
