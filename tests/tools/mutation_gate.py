#!/usr/bin/env python3
"""mutation_gate.py - prove the golden suite is not vacuous: every injected core defect must turn
at least one golden red.

    mutation_gate.py --cli PATH [--ids 0-45] [--ref tests/reference/refcomp.py]
                     [--scripts tests/scripts] [--out tests/output] [--filter GLOB ...]
                     [--jobs K] [--no-baseline] [--allow-pending] [--timeout SEC]

1. Baseline: the goldens are run once without a mutation (single render). Only goldens that PASS
   the baseline can catch a mutation; a golden that already fails would "catch" every mutation
   for free. Baseline failures are printed. (--no-baseline skips this and uses every golden.)
2. For each id: run_goldens with --mutate id over the baseline-passing goldens. The id is
   CAUGHT iff >= 1 golden fails - by pixels ("mismatch"), by a CLI error ("cli-error"), by an
   "equal_to" pair diverging ("equal_to") or by tile statistics exceeding
   tests/scripts/stats/_expectations.json ("stats"; the only way to see mutation 13). The
   per-kind counts are printed and stored in mutation_gate.json. Include `stats/*` in --filter
   when gating id 13. PENDING = the CLI exited 3 with "mutation N not implemented".
   SUSPECT = the CLI errored on every script with no pixel mismatch (looks like the flag itself
   is rejected) and is not counted as caught.
3. A table is printed and <out>/mutation_gate.json written.

Exit: 0 all ids CAUGHT; 1 any id UNCAUGHT or SUSPECT (prints "SUITE IS VACUOUS FOR MUTATION n");
2 only PENDING ids remain uncaught (0 with --allow-pending); 5 harness/usage error.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS_DIR))
import run_goldens as rg  # noqa: E402


def parse_ids(spec: str) -> list[int]:
    ids: list[int] = []
    for part in spec.split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part[1:]:
            a, b = part.split("-", 1) if not part.startswith("-") else (part, part)
            lo, hi = int(a), int(b)
            if hi < lo:
                raise ValueError(f"bad id range {part!r}")
            ids.extend(range(lo, hi + 1))
        else:
            ids.append(int(part))
    seen = set()
    return [i for i in ids if not (i in seen or seen.add(i))]


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__.split("\n", 1)[1])
    ap.add_argument("--cli", required=True)
    ap.add_argument("--ids", default="0-45", help="e.g. 0-45 or 0-6,14,16-23")
    ap.add_argument("--ref", default=str(rg.REPO / "tests/reference/refcomp.py"))
    ap.add_argument("--scripts", default=str(rg.REPO / "tests/scripts"))
    ap.add_argument("--out", default=str(rg.REPO / "tests/output"))
    ap.add_argument("--filter", action="append", metavar="GLOB")
    ap.add_argument("--expect-error-list", default=None, metavar="FILE")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    ap.add_argument("--timeout", type=float, default=600.0)
    ap.add_argument("--python", default=sys.executable, help="interpreter for the reference")
    ap.add_argument("--no-baseline", action="store_true")
    ap.add_argument("--allow-pending", action="store_true")
    args = ap.parse_args(argv)

    try:
        ids = parse_ids(args.ids)
    except ValueError as e:
        ap.error(str(e))
    try:
        items = rg.discover(Path(args.scripts), args.filter,
                            rg._load_expect_globs(args.expect_error_list))
    except ValueError as e:
        print(f"mutation_gate: {e}", file=sys.stderr)
        return 5
    if not items:
        print("mutation_gate: no golden scripts matched", file=sys.stderr)
        return 5
    if not Path(args.ref).exists():
        print(f"mutation_gate: reference not found: {args.ref}", file=sys.stderr)
        return 5

    catchers = items
    baseline = None
    if not args.no_baseline:
        cfg = rg.make_cfg(args.cli, args.ref, args.out, jobs=args.jobs, timeout=args.timeout,
                          determinism=False, python=args.python)
        cfg["run_out"] = str(Path(cfg["out"]) / "mutate-baseline")
        baseline = rg.run_suite(cfg, items)
        print("baseline " + rg.summary_line(baseline))
        ok = {r["name"] for r in baseline["results"] if r["status"] == "PASS"}
        for r in baseline["results"]:
            if r["status"] != "PASS":
                first = r["reason"].splitlines()[0] if r["reason"] else ""
                print(f"  baseline {r['status']} {r['name']}: {first}")
        catchers = [i for i in items if i["name"] in ok]
        print(f"{len(catchers)} of {len(items)} goldens pass the baseline and can catch "
              f"mutations")
        if not catchers:
            print("mutation_gate: no golden passes the baseline; every mutation is uncaught")

    rows = []
    for mid in ids:
        if catchers:
            cfg = rg.make_cfg(args.cli, args.ref, args.out, mutate=mid, jobs=args.jobs,
                              timeout=args.timeout, python=args.python)
            rep = rg.run_suite(cfg, catchers)
            m = rep["mutation"]
            verdict, failed, names = m["verdict"], m["failed"], m["failed_names"]
            by_kind = m.get("by_kind", {})
        else:
            verdict, failed, names, by_kind = "UNCAUGHT", 0, [], {}
        rows.append({"id": mid, "verdict": verdict, "failed": failed, "of": len(catchers),
                     "first_failed": names[:4], "by_kind": by_kind})
        kinds = " ".join(f"{k} {n}" for k, n in by_kind.items())
        print(f"mutation {mid:3d}: {verdict:8} {failed:4d}/{len(catchers)}  "
              + (f"[{kinds}]  " if kinds else "") + ", ".join(names[:4]), flush=True)

    print()
    print(f"{'id':>4}  {'verdict':8}  {'failed':>9}  first failing goldens")
    print(f"{'-' * 4}  {'-' * 8}  {'-' * 9}  {'-' * 40}")
    for r in rows:
        print(f"{r['id']:>4}  {r['verdict']:8}  {str(r['failed']) + '/' + str(r['of']):>9}  "
              + ", ".join(r["first_failed"]))
    caught = [r["id"] for r in rows if r["verdict"] == "CAUGHT"]
    pending = [r["id"] for r in rows if r["verdict"] == "PENDING"]
    vacuous = [r for r in rows if r["verdict"] in ("UNCAUGHT", "SUSPECT")]
    print()
    for r in vacuous:
        extra = " (cli errored on every script: flag rejected?)" if r["verdict"] == "SUSPECT" else ""
        print(f"SUITE IS VACUOUS FOR MUTATION {r['id']}{extra}")
    print(f"MUTATION GATE: {len(caught)} caught, {len(vacuous)} uncaught, {len(pending)} pending "
          f"(of {len(rows)})" + (f"; pending ids: {pending}" if pending else ""))
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    (out / "mutation_gate.json").write_text(json.dumps({
        "cli": args.cli, "ids": ids, "rows": rows,
        "baseline_counts": baseline["counts"] if baseline else None,
        "catchers": len(catchers)}, indent=1))
    if vacuous:
        return 1
    if pending and not args.allow_pending:
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
