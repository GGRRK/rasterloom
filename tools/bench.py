#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""bench.py - run the rasterloom-cli --bench cases, each in a fresh process, and print a Markdown
table (the numbers in docs/PERF.md).

    tools/bench.py [--cli build/src/rasterloom-cli] [--case NAME ...] [--repeat N]
                   [--large-repeat N] [--cpu N] [--json OUT.json] [--markdown OUT.md]

Every case runs in its own process so one case's heap never inflates the next case's peak RSS.
Within the process the CLI builds the document with the case's setup script, resets the kernel's
peak-RSS counter (/proc/self/clear_refs), then times one step. Reported per case: the minimum and
median wall time of the timed step over --repeat runs (--large-repeat for the 8000x8000 cases),
the resident set before the step (the setup document, incl. its history) and the peak during it.
--cpu N pins every case to logical CPU N (sched_setaffinity): on hybrid CPUs this keeps all
runs on one core type, which the published numbers need. The machine line reports the CPU model
only.
"""
from __future__ import annotations

import argparse
import json
import os
import platform
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent


def cpu_model() -> str:
    try:
        for line in Path("/proc/cpuinfo").read_text().splitlines():
            if line.startswith("model name"):
                return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor() or "unknown CPU"


def run_case(cli: str, name: str, repeat: int, cpu: int | None = None) -> dict:
    with tempfile.TemporaryDirectory(prefix="rl-bench-") as td:
        out = Path(td) / "r.json"
        p = subprocess.run([cli, "--bench", "--bench-case", name, "--bench-repeat", str(repeat),
                            "--bench-json", str(out)], capture_output=True, text=True,
                           preexec_fn=(lambda: os.sched_setaffinity(0, {cpu})) if cpu is not None else None)
        if p.returncode != 0 or not out.exists():
            return {"name": name, "error": (p.stdout + p.stderr).strip()[-400:] or f"exit {p.returncode}"}
        return json.loads(out.read_text())[0]


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--cli", default=str(REPO / "build/src/rasterloom-cli"))
    ap.add_argument("--case", action="append", help="case name (default: all)")
    ap.add_argument("--repeat", type=int, default=5, help="runs per 4000x3000-class case")
    ap.add_argument("--large-repeat", type=int, default=1, help="runs per 8000x8000 case")
    ap.add_argument("--cpu", type=int, help="pin every case to this logical CPU")
    ap.add_argument("--json")
    ap.add_argument("--markdown")
    a = ap.parse_args(argv)

    listing = subprocess.run([a.cli, "--bench-list"], capture_output=True, text=True, check=True).stdout
    about = dict(line.split("\t", 1) for line in listing.splitlines() if "\t" in line)
    names = a.case or list(about)
    rows, results, bad = [], [], 0
    for n in names:
        rep = a.large_repeat if "8000" in n else a.repeat
        r = run_case(a.cli, n, rep, a.cpu)
        results.append(r)
        if r.get("error"):
            bad += 1
            print(f"{n}: ERROR {r['error']}", file=sys.stderr)
            continue
        ms = r["ms"]
        peak = r["peak_kib"] / 1024.0 if r["peak_kib"] >= 0 else float("nan")
        before = r["rss_before_kib"] / 1024.0 if r["rss_before_kib"] >= 0 else float("nan")
        rows.append(f"| `{n}` | {about.get(n, '')} | {min(ms):.0f} | {statistics.median(ms):.0f} | {len(ms)} "
                    f"| {before:.0f} | {peak:.0f}{'' if r['peak_is_step'] else ' (process)'} |")
        print(rows[-1], flush=True)
    pin = f", pinned to logical CPU {a.cpu}" if a.cpu is not None else ""
    md = [f"Machine: {cpu_model()}, {os.cpu_count()} logical CPUs{pin}; composite scheduler: "
          f"deterministic (1 thread).", "",
          "| case | what | min ms | median ms | runs | RSS before MiB | peak RSS MiB |",
          "|---|---|---:|---:|---:|---:|---:|", *rows]
    text = "\n".join(md) + "\n"
    print()
    print(text)
    if a.markdown:
        Path(a.markdown).write_text(text)
    if a.json:
        Path(a.json).write_text(json.dumps({"cpu": cpu_model(), "results": results}, indent=1))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
