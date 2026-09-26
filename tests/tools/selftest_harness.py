#!/usr/bin/env python3
"""selftest_harness.py - prove run_goldens / mutation_gate / pixcmp / contact_sheet detect what
they claim to, using stand-in renderers (no real CLI or reference needed).

    python3 tests/tools/selftest_harness.py [--keep]

Builds a throw-away scripts tree plus two fake renderers in a temp dir, runs the tools as
subprocesses, and asserts on exit codes, statuses and files. Exit 0 iff every check passes.
"""
from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
import run_goldens as rg  # noqa: E402

FAKE_COMMON = r'''
import hashlib, json, os, sys, time, zlib, struct
def png(path, w, h, px):
    raw = b"".join(b"\0" + bytes(px[y*w*4:(y+1)*w*4]) for y in range(h))
    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    data = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) \
        + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")
    open(path, "wb").write(data)
def render(script_path, out, role, mutate):
    obj = json.load(open(script_path))
    if set(obj) - {"canvas", "ops", "out"}:
        print("script error: unknown top-level field", file=sys.stderr); sys.exit(1)
    fake = {}
    for op in obj["ops"]:
        if op.get("op") == "bogus":
            print("script error: unknown op bogus", file=sys.stderr); sys.exit(1)
        if op.get("op") == "fake":
            fake.update(op)
    w, h = obj["canvas"]["w"], obj["canvas"]["h"]
    core = json.dumps({"canvas": obj["canvas"], "ops": [o for o in obj["ops"] if o.get("op") != "fake"]},
                      sort_keys=True).encode()
    d = hashlib.sha256(core).digest()
    px = bytearray((d * (w * h * 4 // len(d) + 1))[: w * h * 4])
    for i in range(3, len(px), 4):
        px[i] = 255
    if role == "cli":
        if mutate is not None:
            if mutate == 99:
                print(f"mutation {mutate} not implemented", file=sys.stderr); sys.exit(3)
            if mutate == 5:
                print("unknown option --mutate", file=sys.stderr); sys.exit(2)
            if mutate in fake.get("catch", []):
                px[0] ^= 1
        mode = fake.get("cli")
        if mode == "fail":
            print("cli crashed politely", file=sys.stderr); sys.exit(1)
        if mode == "differ":
            px[5 * 4 + 1] ^= 7
        if mode == "nondet":
            px[2] = time.time_ns() & 255
            px[6] = (time.time_ns() >> 8) & 255
    else:
        with open(os.environ["FAKE_REF_COUNTER"], "a") as f:
            f.write(os.path.basename(script_path) + "\n")
    png(out, w, h, px)
'''

FAKE_CLI = "#!" + sys.executable + "\n" + FAKE_COMMON + r'''
args = sys.argv[1:]
mutate = None
for a in args:
    if a.startswith("--mutate="):
        mutate = int(a.split("=", 1)[1])
s = args[args.index("--render-script") + 1]
o = args[args.index("--out") + 1]
render(s, o, "cli", mutate)
if "--stats" in args:
    fk = {}
    for op in json.load(open(s))["ops"]:
        if op.get("op") == "fake":
            fk.update(op)
    total = fk.get("total", 4)
    alloc = total if mutate == 13 else fk.get("alloc", 1)
    print(f"stats: pixels 'a' allocated={alloc} total={total}")
    print(f"stats: selection allocated=0 total={total}")
    print(f"stats: all allocated={alloc} total={2 * total}")
'''

FAKE_REF = FAKE_COMMON + r'''
render(sys.argv[1], sys.argv[2], "ref", None)
'''


def script(ops, **extra):
    d = {"canvas": {"w": 16, "h": 12, "bg": "#00000000"}, "ops": ops, "out": "png8"}
    d.update(extra)
    return d


LAYER = {"op": "add_layer", "id": "a", "fill": "solid", "color": "#FF0000FF"}

FAILS: list[str] = []


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        FAILS.append(what)


def run(cmd, env=None):
    cp = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                        env=env)
    return cp.returncode, cp.stdout


def main() -> int:
    keep = "--keep" in sys.argv
    tmp = Path(tempfile.mkdtemp(prefix="rl-harness-selftest-"))
    try:
        cli = tmp / "fake-cli"
        cli.write_text(FAKE_CLI)
        cli.chmod(0o755)
        refdir = tmp / "ref"
        refdir.mkdir()
        ref = refdir / "refcomp.py"
        ref.write_text(FAKE_REF)
        counter = tmp / "ref-calls.txt"
        counter.write_text("")
        env = dict(os.environ, FAKE_REF_COUNTER=str(counter))
        s = tmp / "scripts"
        (s / "dom").mkdir(parents=True)
        (s / "smoke").mkdir()
        files = {
            "dom/ok1": script([LAYER, {"op": "fake", "catch": [1]}]),
            "dom/ok2": script([LAYER, {"op": "set_opacity", "layer": "a", "value": 0.5}]),
            "dom/differ": script([LAYER, {"op": "fake", "cli": "differ", "catch": [2]}]),
            "dom/err": script([{"op": "bogus"}], expect="error"),
            "dom/err_wrong": script([LAYER], expect="error"),
            "dom/cli_fails": script([LAYER, {"op": "fake", "cli": "fail"}]),
            "dom/nondet": script([LAYER, {"op": "fake", "cli": "nondet"}]),
            "smoke/skipme": script([{"op": "bogus"}]),
        }
        for k, v in files.items():
            (s / f"{k}.json").write_text(json.dumps(v))
        out = tmp / "out"
        base = [sys.executable, str(TOOLS / "run_goldens.py"), "--cli", str(cli), "--ref",
                str(ref), "--scripts", str(s), "--out", str(out), "--jobs", "4"]

        # ---- pixcmp -------------------------------------------------------------------------
        from PIL import Image
        a, b, c = tmp / "a.png", tmp / "b.png", tmp / "c.png"
        Image.new("RGBA", (4, 3), (1, 2, 3, 255)).save(a)
        Image.new("RGB", (4, 3), (1, 2, 3)).save(b)          # RGB decodes to the same RGBA
        im = Image.new("RGBA", (4, 3), (1, 2, 3, 255))
        im.putpixel((2, 1), (1, 9, 3, 255))
        im.save(c)
        rc, o = run([sys.executable, str(TOOLS / "pixcmp.py"), str(a), str(b)])
        check(rc == 0, "pixcmp: RGBA vs RGB with same pixels -> identical (exit 0)")
        rc, o = run([sys.executable, str(TOOLS / "pixcmp.py"), str(a), str(c), "--diff",
                     str(tmp / "d.png")])
        check(rc == 1 and "1 of 12 pixels differ" in o and "max per-channel delta 7" in o
              and "(2,1)" in o and (tmp / "d.png").exists(),
              "pixcmp: one differing pixel reported with count, delta, coords, diff mask")
        Image.new("RGBA", (5, 3)).save(tmp / "e.png")
        rc, o = run([sys.executable, str(TOOLS / "pixcmp.py"), str(a), str(tmp / "e.png")])
        check(rc == 1 and "SIZE MISMATCH" in o, "pixcmp: size mismatch -> exit 1")
        rc, o = run([sys.executable, str(TOOLS / "pixcmp.py"), str(a), str(tmp / "nope.png")])
        check(rc == 2, "pixcmp: unreadable file -> exit 2")

        # ---- strip_expect keeps tokens verbatim ------------------------------------------
        t = '{"expect":"error","canvas":{"w":1,"h":1},"ops":[{"op":"x","v":0.1000000000000000055511,' \
            '"n":1e-3,"big":100000000000000000000,"dup":1,"dup":2}],"out":"png8"}'
        st = rg.strip_expect(t)
        check('"expect"' not in st and "0.1000000000000000055511" in st and "1e-3" in st
              and "100000000000000000000" in st and st.count('"dup"') == 2,
              "strip_expect: removes expect, keeps number text and duplicate keys")

        # ---- full normal run --------------------------------------------------------------
        rc, o = run(base, env)
        rep = json.loads((out / "report.json").read_text())
        st = {r["name"]: r["status"] for r in rep["results"]}
        check(rc == 1, "run_goldens: failures -> non-zero exit")
        check(st == {"dom/ok1": "PASS", "dom/ok2": "PASS", "dom/err": "PASS",
                     "dom/differ": "FAIL", "dom/err_wrong": "FAIL", "dom/nondet": "FAIL",
                     "dom/cli_fails": "ERROR"}, f"run_goldens: statuses {st}")
        check("smoke/skipme" not in st, "run_goldens: smoke/ skipped")
        check("SUMMARY: 7 goldens  PASS 3  FAIL 3  ERROR 1" in o, "run_goldens: summary line")
        dd = out / "dom" / "differ"
        check(all((dd / f).exists() for f in ("ref.png", "cpp.png", "diff.png")),
              "run_goldens: failing golden keeps ref.png/cpp.png/diff.png")
        check(not (out / "dom" / "ok1").exists(), "run_goldens: passing golden dir removed")
        r_nd = next(r for r in rep["results"] if r["name"] == "dom/nondet")
        check(r_nd["kind"] == "nondeterministic", "run_goldens: nondeterminism detected")
        r_ew = next(r for r in rep["results"] if r["name"] == "dom/err_wrong")
        check("cli rendered" in r_ew["reason"],
              "run_goldens: expect key stripped (valid expect-error script is rendered -> FAIL)")
        calls1 = counter.read_text().count("\n")

        # ---- cache: second run must not call the reference again -------------------------
        rc, o = run(base + ["--filter", "ok*"], env)
        check(rc == 0 and "PASS 2" in o, "run_goldens --filter ok*: all pass -> exit 0")
        check(counter.read_text().count("\n") == calls1, "reference cache hit (no new ref calls)")
        rc, o = run(base + ["--filter", "ok*", "--no-ref-cache"], env)
        check(counter.read_text().count("\n") == calls1 + 2, "--no-ref-cache re-runs the ref")

        rc, o = run(base + ["--expect-error-list"], env)
        check(rc == 0 and "dom/err" in o and "2 expect-error goldens" in o,
              "--expect-error-list (no FILE) lists expect-error goldens")
        lst = tmp / "errs.txt"
        lst.write_text("# extra\nok2\n")
        rc, o = run(base + ["--filter", "ok*", "--expect-error-list", str(lst)], env)
        rep2 = {r["name"]: r for r in json.loads((out / "report.json").read_text())["results"]}
        check(rc == 1 and rep2["dom/ok2"]["expect_error"] and rep2["dom/ok2"]["status"] == "FAIL"
              and rep2["dom/ok1"]["status"] == "PASS",
              "--expect-error-list FILE marks ok2 as expect-error (rendered -> FAIL)")

        # ---- mutation mode ---------------------------------------------------------------
        mb = base + ["--filter", "ok*", "--filter", "differ"]
        rc, o = run(mb + ["--mutate", "1"], env)
        check(rc == 0 and "CAUGHT" in o, "--mutate 1: caught (exit 0)")
        rc, o = run(base + ["--filter", "ok*", "--mutate", "3"], env)
        check(rc == 1 and "UNCAUGHT" in o, "--mutate 3 (no effect): uncaught (exit 1)")
        rc, o = run(mb + ["--mutate", "99"], env)
        check(rc == 3 and "PENDING" in o, "--mutate 99: cli exit 3 'not implemented' -> PENDING")
        rc, o = run(base + ["--filter", "ok*", "--mutate", "5"], env)
        check(rc == 4 and "SUSPECT" in o, "--mutate 5: cli rejects flag everywhere -> SUSPECT")
        check((out / "mutate-1" / "report.json").exists(), "mutation report in out/mutate-N/")

        gate = [sys.executable, str(TOOLS / "mutation_gate.py"), "--cli", str(cli), "--ref",
                str(ref), "--scripts", str(s), "--out", str(out), "--jobs", "4"]
        rc, o = run(gate + ["--ids", "1,2,99"], env)
        check(rc == 1 and "SUITE IS VACUOUS FOR MUTATION 2" in o and "PENDING" in o,
              "mutation_gate: id 2 only 'caught' by a baseline-failing golden -> vacuous")
        rc, o = run(gate + ["--ids", "1,99"], env)
        check(rc == 2, "mutation_gate: only pending left -> exit 2")
        rc, o = run(gate + ["--ids", "1,99", "--allow-pending"], env)
        check(rc == 0, "mutation_gate --allow-pending -> exit 0")
        rc, o = run(gate + ["--ids", "1"], env)
        check(rc == 0 and "1 caught, 0 uncaught" in o, "mutation_gate: all caught -> exit 0")

        # ---- determinism-only (no reference) ----------------------------------------------
        calls_before = counter.read_text().count("\n")
        rc, o = run(base + ["--determinism-only"], env)
        repd = {r["name"]: r["status"] for r in
                json.loads((out / "report.json").read_text())["results"]}
        check(rc == 1 and repd["dom/nondet"] == "FAIL" and repd["dom/cli_fails"] == "PASS"
              and repd["dom/err"] == "PASS" and repd["dom/ok1"] == "PASS"
              and counter.read_text().count("\n") == calls_before,
              "--determinism-only: nondeterminism FAILs, identical rejections PASS, no ref calls")

        # ---- equal_to and tile statistics (second tree) ------------------------------------
        s2 = tmp / "scripts2"
        (s2 / "dom").mkdir(parents=True)
        (s2 / "stats").mkdir()
        files2 = {
            "dom/target": script([LAYER]),
            "dom/eq_pass": script([LAYER, {"op": "fake", "note": "same pixels"}],
                                  equal_to="target"),
            "dom/eq_fail": script([LAYER, {"op": "set_visible", "layer": "a", "value": True}],
                                  equal_to="target"),
            "dom/eq_missing": script([LAYER], equal_to="nope"),
            "stats/s_ok": script([LAYER, {"op": "fake", "alloc": 1, "total": 4}]),
            "stats/s_over": script([LAYER, {"op": "fake", "alloc": 3, "total": 4}]),
        }
        for k, v in files2.items():
            (s2 / f"{k}.json").write_text(json.dumps(v))
        man = {"expectations": {"stats/s_ok": {"pixels:a": {"max_allocated": 1, "total": 4},
                                               "all": 1},
                                "stats/s_over": {"pixels:a": 2}}}
        (s2 / "stats" / "_expectations.json").write_text(json.dumps(man))
        out2 = tmp / "out2"
        base2 = [sys.executable, str(TOOLS / "run_goldens.py"), "--cli", str(cli), "--ref",
                 str(ref), "--scripts", str(s2), "--out", str(out2), "--jobs", "4"]
        st2 = rg.strip_harness_keys('{"equal_to":"B05","canvas":{"w":1,"h":1},"ops":[],'
                                    '"out":"png8","expect":"error"}')
        check('"equal_to"' not in st2 and '"expect"' not in st2 and '"canvas"' in st2,
              "strip_harness_keys: removes equal_to and expect")
        rc, o = run(base2 + ["--filter", "eq_pass"], env)
        rep3 = {r["name"]: r for r in json.loads((out2 / "report.json").read_text())["results"]}
        check(rc == 0 and set(rep3) == {"dom/eq_pass", "dom/target"}
              and rep3["dom/target"]["dependency"] and rep3["dom/eq_pass"]["status"] == "PASS"
              and "equal to dom/target" in rep3["dom/eq_pass"]["reason"],
              "equal_to: equal pair passes; filtered-out target is pulled in as a dependency")
        check(not (out2 / ".equal_to").exists() and not (out2 / "dom" / "eq_pass").exists(),
              "equal_to: scratch PNGs removed after a pass")
        rc, o = run(base2 + ["--filter", "eq_fail"], env)
        rep3 = {r["name"]: r for r in json.loads((out2 / "report.json").read_text())["results"]}
        r_ef = rep3["dom/eq_fail"]
        check(rc == 1 and r_ef["status"] == "FAIL" and r_ef["kind"] == "equal_to"
              and "FAIL  dom/eq_fail: equal_to dom/target" in o
              and all((out2 / "dom" / "eq_fail" / f).exists() for f in
                      ("cpp.png", "equal_to-target.png", "diff-equal_to.png")),
              "equal_to: differing pair FAILs (kind equal_to) and keeps both PNGs + diff")
        rc, o = run(base2 + ["--filter", "eq_missing"], env)
        rep3 = {r["name"]: r for r in json.loads((out2 / "report.json").read_text())["results"]}
        check(rc == 1 and rep3["dom/eq_missing"]["status"] == "ERROR"
              and "not found" in rep3["dom/eq_missing"]["reason"],
              "equal_to: missing target -> ERROR")
        rc, o = run(base2 + ["--filter", "stats/*"], env)
        rep3 = {r["name"]: r for r in json.loads((out2 / "report.json").read_text())["results"]}
        check(rc == 1 and rep3["stats/s_ok"]["status"] == "PASS"
              and rep3["stats/s_ok"]["stats"]["pixels:a"] == {"allocated": 1, "total": 4}
              and rep3["stats/s_over"]["status"] == "FAIL"
              and rep3["stats/s_over"]["kind"] == "stats"
              and "allocated=3 > max 2" in rep3["stats/s_over"]["reason"],
              "stats: within-bound passes, over-bound FAILs (kind stats) with pixels identical")
        rc, o = run(base2 + ["--filter", "s_ok", "--mutate", "13"], env)
        rep3 = json.loads((out2 / "mutate-13" / "report.json").read_text())
        check(rc == 0 and rep3["mutation"]["verdict"] == "CAUGHT"
              and rep3["mutation"]["by_kind"] == {"stats": 1},
              "stats: pixel-invisible mutation 13 (dense tiles) is CAUGHT via --stats")
        gate2 = [sys.executable, str(TOOLS / "mutation_gate.py"), "--cli", str(cli), "--ref",
                 str(ref), "--scripts", str(s2), "--out", str(out2), "--jobs", "4",
                 "--filter", "s_ok"]
        rc, o = run(gate2 + ["--ids", "13"], env)
        check(rc == 0 and "1 caught, 0 uncaught" in o and "stats 1" in o,
              "mutation_gate: id 13 counted as caught through the stats manifest")
        man["expectations"]["stats/ghost"] = {"all": 0}
        (s2 / "stats" / "_expectations.json").write_text(json.dumps(man))
        rc, o = run(base2 + ["--filter", "s_ok"], env)
        check(rc == 2 and "stats/ghost" in o, "stats: manifest naming a missing golden -> exit 2")

        # ---- contact sheet ---------------------------------------------------------------
        rc, o = run([sys.executable, str(TOOLS / "contact_sheet.py"), "--scripts", str(s),
                     "--out", str(out), "--ref", str(ref)], env)
        check(rc == 0 and (out / "sheets" / "dom.png").exists() and "0 missing" in o,
              "contact_sheet: domain sheet from the ref cache, no missing tiles")
    finally:
        if keep:
            print(f"kept {tmp}")
        else:
            shutil.rmtree(tmp, ignore_errors=True)
    print(f"\nselftest_harness: {'FAILED ' + str(len(FAILS)) if FAILS else 'all checks passed'}")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
