#!/usr/bin/env python3
"""run_goldens.py - render every golden script with the C++ CLI and the NumPy reference and
require byte-identical RGBA output.

    run_goldens.py --cli PATH [--ref tests/reference/refcomp.py] [--scripts tests/scripts]
                   [--out tests/output] [--filter GLOB ...] [--mutate N] [--jobs K]
                   [--expect-error-list [FILE]] [--single] [--keep-all] [--no-ref-cache]
                   [--timeout SEC] [--verbose]

Both renderers are black boxes:
    C++: CLI --render-script S.json --out cpp.png [--mutate=N]   (exit non-zero = script error)
    ref: python3 REF S.json ref.png                              (exit non-zero = script error)
Every renderer subprocess runs with QT_QPA_PLATFORM=offscreen.

Discovery: every *.json under --scripts, recursively, except the top-level `smoke/` directory and
any directory whose name starts with '.'. The relative path without `.json` is the golden's name
(e.g. `compositing/blend_mul_opq`); --filter globs match that name or its basename.

Harness keys (tests/scripts/README.md): `"expect"` and `"equal_to"` are stripped from the top-level
object before either renderer sees the script (verbatim re-serialisation, see below).

Equal-to: a script with `"equal_to": "<stem>"` must, besides matching the reference, produce the
same decoded RGBA as the CLI output of `<stem>.json` in the same directory (same run, same
--mutate). The target is added to the run even when --filter excludes it. A failure is
FAIL kind "equal_to" and keeps cpp.png, equal_to-target.png and diff-equal_to.png.

Tile statistics: tests/scripts/stats/_expectations.json (files named `_*.json` are manifests, never
goldens) maps golden names to upper bounds on allocated tiles. Those goldens are rendered with
`--stats`; when the pixels match but a bound is exceeded (or a stats line is missing) the golden
is FAIL kind "stats". This is what makes mutation 13 (dense tile grid) visible: it never changes
a pixel.

Expected errors: a script whose top-level JSON object has `"expect": "error"` must make BOTH
renderers exit non-zero and write no PNG. Because C9 makes any unknown field a script error, the
harness removes the `expect` key before handing the script to either renderer (it writes the
stripped copy to <out>/<name>/script.json, preserving every other token verbatim); otherwise the
error test would pass for the wrong reason. `--expect-error-list FILE` additionally marks every
golden whose name matches a glob line of FILE as expect-error (for scripts that cannot carry the
key, e.g. invalid JSON). `--expect-error-list` without FILE lists the expect-error goldens that
discovery finds and exits.

Determinism-only mode (--determinism-only): no reference; the CLI renders every golden twice and
the two runs must agree (byte-identical PNGs, or the same clean rejection). This is the CTest
`goldens_determinism` check; it stays meaningful for domains whose ops the CLI cannot render yet.

Normal mode, per golden:
  1. CLI renders twice; the two PNG files must be byte-identical (determinism) unless --single.
  2. The reference renders (cached in <out>/.refcache/ by a hash of the script bytes, the reference
     directory's *.py files and the interpreter, so mutation loops do not re-run it).
  3. pixcmp: decoded RGBA must be identical.
  PASS removes <out>/<name>/ (unless --keep-all); FAIL/ERROR keep ref.png, cpp.png, diff.png and
  the renderer logs there. Writes <out>/report.json. Exit 0 iff every golden PASSed (and at least
  one golden ran).
  Status meanings: FAIL = the two renderers disagree, the CLI is nondeterministic, or an
  expect-error script was rendered; ERROR = a renderer failed on a script expected to render, a
  renderer crashed on a signal or timed out, or the harness could not run it.

Mutation mode (--mutate N): N is passed to the CLI only (`--mutate=N`), the double render is
skipped, results go to <out>/mutate-N/ (report.json there, at most --keep-failures failing golden
directories kept, default 3). A golden FAILs when the mutated CLI disagrees with the reference or
errors on a script the reference renders. If the CLI exits with code 3 and prints
"mutation N not implemented", the id is PENDING (probed once, before the full run).
Exit codes: 0 = caught (>= 1 golden failed), 1 = not caught, 3 = PENDING,
4 = SUSPECT (the CLI errored on every script and no pixel mismatch was seen, which is what an
unsupported flag looks like), 2 = harness/usage error.
"""
from __future__ import annotations

import argparse
import fnmatch
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent
REPO = TOOLS_DIR.parents[1]
sys.path.insert(0, str(TOOLS_DIR))
import pixcmp  # noqa: E402

PENDING_RE = re.compile(r"mutation\s+(-?\d+)\s+not\s+implemented", re.IGNORECASE)
CACHE_VERSION = b"rasterloom-refcache-v1"
EXIT_PENDING = 3
EXIT_SUSPECT = 4


# --------------------------------------------------------------------------------------------
# Verbatim JSON re-serialisation (used only to strip the "expect" key)
# --------------------------------------------------------------------------------------------
class _Raw:
    """A JSON number token kept as its exact source text."""
    __slots__ = ("text",)

    def __init__(self, text: str):
        self.text = text


def _loads_verbatim(text: str):
    return json.loads(text, object_pairs_hook=lambda pairs: ("__obj__", pairs),
                      parse_float=_Raw, parse_int=_Raw, parse_constant=_Raw)


def _dumps_verbatim(v) -> str:
    if isinstance(v, tuple) and len(v) == 2 and v[0] == "__obj__":
        return "{" + ", ".join(json.dumps(k) + ": " + _dumps_verbatim(x) for k, x in v[1]) + "}"
    if isinstance(v, list):
        return "[" + ", ".join(_dumps_verbatim(x) for x in v) + "]"
    if isinstance(v, _Raw):
        return v.text
    return json.dumps(v)  # str, bool, None


HARNESS_KEYS = ("expect", "equal_to")


def strip_harness_keys(text: str) -> str:
    """Remove every top-level harness key ("expect", "equal_to"; tests/scripts/README.md),
    keeping all other tokens (numbers, duplicate keys, key order) exactly as written."""
    tree = _loads_verbatim(text)
    tag, pairs = tree
    return _dumps_verbatim((tag, [(k, v) for k, v in pairs if k not in HARNESS_KEYS])) + "\n"


strip_expect = strip_harness_keys  # historical name, kept for callers


# --------------------------------------------------------------------------------------------
# Tile-statistics expectations (tests/scripts/stats/_expectations.json)
# --------------------------------------------------------------------------------------------
STATS_MANIFEST = Path("stats") / "_expectations.json"
STATS_LINE_RE = re.compile(
    r"^stats: (?P<what>[A-Za-z-]+)(?: '(?P<id>[^']*)')? allocated=(?P<a>\d+) total=(?P<t>\d+)\s*$")


def load_stats_manifest(scripts_dir: Path) -> dict:
    """{golden name: {stat key: {"max_allocated": int, "total": int|None}}}.

    Manifest shape: {"expectations": {"<golden name>": {"<key>": N | {"max_allocated": N,
    "total": T}}}} where <key> is "all", "selection", "saved-selection" or "<what>:<node id>"
    (e.g. "pixels:dot", "mask:dot"), matching `rasterloom-cli --stats` lines."""
    p = scripts_dir / STATS_MANIFEST
    if not p.exists():
        return {}
    raw = json.loads(p.read_text())
    out: dict = {}
    for name, keys in raw.get("expectations", {}).items():
        if not isinstance(keys, dict) or not keys:
            raise ValueError(f"{p}: expectations for {name!r} must be a non-empty object")
        exp = {}
        for k, v in keys.items():
            if isinstance(v, bool) or not isinstance(v, (int, dict)):
                raise ValueError(f"{p}: {name}.{k}: expected int or object")
            if isinstance(v, int):
                v = {"max_allocated": v}
            if set(v) - {"max_allocated", "total"} or not isinstance(v.get("max_allocated"), int):
                raise ValueError(f"{p}: {name}.{k}: needs int max_allocated (and optional total)")
            exp[k] = {"max_allocated": v["max_allocated"], "total": v.get("total")}
        out[name] = exp
    return out


def parse_stats(stdout: str) -> dict:
    """`--stats` lines -> {key: (allocated, total)}; key as in load_stats_manifest."""
    got = {}
    for line in stdout.splitlines():
        m = STATS_LINE_RE.match(line.strip())
        if m:
            key = m["what"] + (":" + m["id"] if m["id"] is not None else "")
            got[key] = (int(m["a"]), int(m["t"]))
    return got


def check_stats(expect: dict, got: dict) -> list[str]:
    bad = []
    for key, e in sorted(expect.items()):
        if key not in got:
            bad.append(f"{key}: no '--stats' line (got {sorted(got) or 'none'})")
            continue
        a, t = got[key]
        if a > e["max_allocated"]:
            bad.append(f"{key}: allocated={a} > max {e['max_allocated']} (total={t})")
        if e["total"] is not None and t != e["total"]:
            bad.append(f"{key}: total={t} != expected {e['total']}")
    return bad


# --------------------------------------------------------------------------------------------
# Discovery
# --------------------------------------------------------------------------------------------
def _load_expect_globs(path: str | None) -> list[str]:
    if not path:
        return []
    globs = []
    for line in Path(path).read_text().splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            globs.append(line[:-5] if line.endswith(".json") else line)
    return globs


def _matches(name: str, globs: list[str]) -> bool:
    base = name.rsplit("/", 1)[-1]
    return any(fnmatch.fnmatchcase(name, g) or fnmatch.fnmatchcase(base, g) for g in globs)


def _make_item(scripts_dir: Path, p: Path, expect_globs: list[str], stats: dict) -> dict:
    name = p.relative_to(scripts_dir).with_suffix("").as_posix()
    item = {"name": name, "path": str(p), "expect_error": False, "has_expect_key": False,
            "harness_error": None, "equal_to": None, "stats": stats.get(name),
            "keep_cli_png": False, "dependency": False}
    try:
        obj = json.loads(p.read_text())
    except Exception:  # noqa: BLE001 - invalid JSON is the renderers' problem
        obj = None
    if isinstance(obj, dict) and "expect" in obj:
        item["has_expect_key"] = True
        if obj["expect"] == "error":
            item["expect_error"] = True
        else:
            item["harness_error"] = f'unknown "expect" value {obj["expect"]!r} (only "error")'
    if isinstance(obj, dict) and "equal_to" in obj:
        item["has_expect_key"] = True
        stem = obj["equal_to"]
        if not isinstance(stem, str) or not stem or "/" in stem or stem.endswith(".json"):
            item["harness_error"] = f'"equal_to" must be a bare script stem, got {stem!r}'
        else:
            parent = p.relative_to(scripts_dir).parent.as_posix()
            item["equal_to"] = stem if parent == "." else f"{parent}/{stem}"
            item["keep_cli_png"] = True
            if item["equal_to"] == name:
                item["harness_error"] = '"equal_to" names the script itself'
    if _matches(name, expect_globs):
        item["expect_error"] = True
    if item["expect_error"] and item["equal_to"]:
        item["harness_error"] = '"equal_to" on an expect-error script is meaningless'
    return item


def _discoverable(scripts_dir: Path, p: Path) -> bool:
    parts = p.relative_to(scripts_dir).parts
    return not (parts[0] == "smoke" or p.name.startswith("_")
                or any(part.startswith(".") for part in parts[:-1]))


def manifest_unknown_names(scripts_dir: Path) -> list[str]:
    names = set(load_stats_manifest(scripts_dir))
    return sorted(n for n in names if not (scripts_dir / (n + ".json")).is_file())


def discover(scripts_dir: Path, filters: list[str] | None, expect_globs: list[str]) -> list[dict]:
    """Every golden under scripts_dir (see module doc), filtered; the targets of any selected
    script's "equal_to" are added even when the filter excludes them (flagged "dependency"),
    because the assertion needs their CLI output. `_*.json` files are harness manifests."""
    stats = load_stats_manifest(scripts_dir)
    all_paths = {p.relative_to(scripts_dir).with_suffix("").as_posix(): p
                 for p in sorted(scripts_dir.rglob("*.json")) if _discoverable(scripts_dir, p)}
    items = [_make_item(scripts_dir, p, expect_globs, stats) for name, p in all_paths.items()
             if not filters or _matches(name, filters)]
    have = {i["name"]: i for i in items}
    todo = list(items)
    while todo:
        it = todo.pop()
        tgt = it["equal_to"]
        if not tgt or it["harness_error"]:
            continue
        if tgt not in all_paths:
            it["harness_error"] = f'"equal_to" target {tgt}.json not found'
            continue
        if tgt not in have:
            dep = _make_item(scripts_dir, all_paths[tgt], expect_globs, stats)
            dep["dependency"] = True
            have[tgt] = dep
            items.append(dep)
            todo.append(dep)
        have[tgt]["keep_cli_png"] = True
    items.sort(key=lambda i: i["name"])
    return items


# --------------------------------------------------------------------------------------------
# Reference cache
# --------------------------------------------------------------------------------------------
def ref_code_hash(ref: Path) -> str:
    """Hash of every *.py file under the reference's directory (bytes only; never interpreted)."""
    h = hashlib.sha256()
    root = ref.resolve().parent
    for f in sorted(root.rglob("*.py")):
        if "__pycache__" in f.parts:
            continue
        h.update(f.relative_to(root).as_posix().encode() + b"\0")
        h.update(f.read_bytes())
        h.update(b"\0")
    return h.hexdigest()


def cache_key(script_bytes: bytes, code_hash: str, python: str) -> str:
    h = hashlib.sha256()
    for part in (CACHE_VERSION, code_hash.encode(), python.encode(), script_bytes):
        h.update(part)
        h.update(b"\0")
    return h.hexdigest()


# --------------------------------------------------------------------------------------------
# Running one renderer
# --------------------------------------------------------------------------------------------
def _env() -> dict:
    env = dict(os.environ)
    env["QT_QPA_PLATFORM"] = "offscreen"
    return env


def run_renderer(cmd: list[str], out_png: Path, log: Path, timeout: float) -> dict:
    if out_png.exists():
        out_png.unlink()
    t0 = time.monotonic()
    try:
        cp = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=_env(),
                            timeout=timeout, cwd=str(REPO))
        rc, out, err, timed_out = cp.returncode, cp.stdout, cp.stderr, False
    except subprocess.TimeoutExpired as e:
        rc, out, err, timed_out = None, e.stdout or b"", e.stderr or b"", True
    except OSError as e:
        rc, out, err, timed_out = None, b"", str(e).encode(), False
    dt = time.monotonic() - t0
    text = (out or b"").decode("utf-8", "replace") + (err or b"").decode("utf-8", "replace")
    log.write_text("$ " + " ".join(cmd) + f"\n# rc={rc} timeout={timed_out} {dt:.2f}s\n" + text)
    return {"rc": rc, "timed_out": timed_out, "png": out_png.exists(), "output": text[-2000:],
            "seconds": round(dt, 3), "stdout": (out or b"").decode("utf-8", "replace")}


def _describe(r: dict) -> str:
    if r["timed_out"]:
        return "timed out"
    if r["rc"] is None:
        return "could not start: " + r["output"].strip()[-200:]
    if r["rc"] < 0:
        return f"killed by signal {-r['rc']}"
    last = r["output"].strip().splitlines()[-1:] or [""]
    return f"exit {r['rc']}" + (f" ({last[0][:160]})" if last[0] else "")


def _clean_error(r: dict) -> bool:
    """A proper C9 script error: normal non-zero exit, no PNG written."""
    return r["rc"] is not None and r["rc"] > 0 and not r["png"] and not r["timed_out"]


def run_ref_cached(cfg: dict, script_path: Path, script_bytes: bytes, workdir: Path) -> dict:
    ref_png = workdir / "ref.png"
    log = workdir / "ref.log"
    cache = Path(cfg["out"]) / ".refcache"
    key = cache_key(script_bytes, cfg["ref_hash"], cfg["python"])
    cpng, cerr = cache / f"{key}.png", cache / f"{key}.err.json"
    if cfg["ref_cache"]:
        if cpng.exists():
            shutil.copyfile(cpng, ref_png)
            log.write_text(f"# cached {cpng}\n")
            return {"rc": 0, "timed_out": False, "png": True, "output": "", "seconds": 0.0,
                    "cached": True}
        if cerr.exists():
            r = json.loads(cerr.read_text())
            r["cached"] = True
            log.write_text(f"# cached {cerr}\n" + r["output"])
            return r
    r = run_renderer([cfg["python"], cfg["ref"], str(script_path), str(ref_png)], ref_png, log,
                     cfg["timeout"])
    r["cached"] = False
    if cfg["ref_cache"] and not r["timed_out"] and r["rc"] is not None:
        cache.mkdir(parents=True, exist_ok=True)
        tmp = cache / f".{key}.{os.getpid()}.tmp"
        if r["rc"] == 0 and r["png"]:
            shutil.copyfile(ref_png, tmp)
            os.replace(tmp, cpng)
        elif r["rc"] != 0:
            tmp.write_text(json.dumps(r))
            os.replace(tmp, cerr)
    return r


def cli_cmd(cfg: dict, script: Path, out_png: Path, stats: bool = False) -> list[str]:
    cmd = [cfg["cli"], "--render-script", str(script), "--out", str(out_png)]
    if stats:
        cmd.append("--stats")
    if cfg["mutate"] is not None:
        cmd.append(f"--mutate={cfg['mutate']}")
    return cmd


def pending_message(r: dict, mutate) -> bool:
    if mutate is None or r["rc"] != 3:
        return False
    m = PENDING_RE.search(r["output"])
    return bool(m) and int(m.group(1)) == int(mutate)


# --------------------------------------------------------------------------------------------
# One golden
# --------------------------------------------------------------------------------------------
def run_one(cfg: dict, item: dict) -> dict:
    name = item["name"]
    workdir = Path(cfg["run_out"]) / name
    if workdir.exists():
        shutil.rmtree(workdir)
    workdir.mkdir(parents=True)
    res = {"name": name, "expect_error": item["expect_error"], "status": "ERROR", "reason": "",
           "kind": None, "cli": None, "cli2": None, "ref": None, "compare": None,
           "dir": str(workdir), "equal_to": item.get("equal_to"),
           "dependency": item.get("dependency", False), "stats": None}
    try:
        if item["harness_error"]:
            res["reason"] = item["harness_error"]
            return res
        src = Path(item["path"])
        raw = src.read_bytes()
        if item["has_expect_key"]:
            try:
                text = strip_harness_keys(raw.decode("utf-8"))
            except Exception as e:  # noqa: BLE001
                res["reason"] = f"harness could not strip the harness keys: {e}"
                return res
            script = workdir / "script.json"
            script.write_text(text)
            script_bytes = text.encode("utf-8")
        else:
            script, script_bytes = src, raw

        cpp = workdir / "cpp.png"
        if cfg.get("determinism_only"):
            return _determinism_only(cfg, script, workdir, res)
        want_stats = bool(item.get("stats"))
        c1 = run_renderer(cli_cmd(cfg, script, cpp, want_stats), cpp, workdir / "cpp.log",
                          cfg["timeout"])
        cli_stdout = c1.pop("stdout", "")
        res["cli"] = c1
        if pending_message(c1, cfg["mutate"]):
            res["status"], res["reason"] = "PENDING", "mutation not implemented in the CLI"
            return res
        ref = run_ref_cached(cfg, script, script_bytes, workdir)
        res["ref"] = ref

        if item["expect_error"]:
            bad = []
            for who, r in (("cli", c1), ("ref", ref)):
                if r["timed_out"] or r["rc"] is None or r["rc"] < 0:
                    res["status"], res["reason"] = "ERROR", f"{who} {_describe(r)}"
                    res["kind"] = f"{who}-crash"
                    return res
                if r["rc"] == 0:
                    bad.append(f"{who} rendered a script expected to error")
                elif r["png"]:
                    bad.append(f"{who} exited {r['rc']} but wrote a PNG (C9: no PNG on error)")
            if bad:
                res["status"], res["reason"] = "FAIL", "; ".join(bad)
                res["kind"] = "cli-rendered-error-script" if c1["rc"] == 0 else "error-contract"
            else:
                res["status"], res["reason"] = "PASS", "both renderers rejected the script"
            return res

        # --- script expected to render ---
        if ref["timed_out"] or ref["rc"] != 0 or not ref["png"]:
            res["status"], res["reason"], res["kind"] = "ERROR", "ref " + _describe(ref) + \
                ("" if ref["png"] or ref["rc"] != 0 else " (exit 0 but no PNG)"), "ref-error"
            return res
        if c1["timed_out"] or c1["rc"] != 0 or not c1["png"]:
            why = "cli " + _describe(c1) + ("" if c1["png"] or c1["rc"] != 0
                                            else " (exit 0 but no PNG)")
            if cfg["mutate"] is not None and not c1["timed_out"]:
                res["status"], res["reason"], res["kind"] = "FAIL", why, "cli-error"
            else:
                res["status"], res["reason"], res["kind"] = "ERROR", why, "cli-error"
            return res

        if cfg["mutate"] is None and cfg["determinism"]:
            cpp2 = workdir / "cpp2.png"
            c2 = run_renderer(cli_cmd(cfg, script, cpp2, want_stats), cpp2,
                              workdir / "cpp2.log", cfg["timeout"])
            c2.pop("stdout", None)
            res["cli2"] = c2
            if c2["timed_out"] or c2["rc"] != 0 or not c2["png"]:
                res["status"], res["reason"] = "FAIL", "second cli render failed: " + _describe(c2)
                res["kind"] = "nondeterministic"
                return res
            if cpp.read_bytes() != cpp2.read_bytes():
                cmp2 = pixcmp.compare(str(cpp), str(cpp2), str(workdir / "diff-determinism.png"))
                res["status"], res["kind"] = "FAIL", "nondeterministic"
                res["reason"] = ("cli output differs between two runs (PNG bytes); decoded: "
                                 + pixcmp.format_report(cmp2, "run1", "run2").splitlines()[0])
                return res

        cmp = pixcmp.compare(str(workdir / "ref.png"), str(cpp), str(workdir / "diff.png"))
        res["compare"] = {k: v for k, v in cmp.items() if k != "first"}
        res["compare"]["first"] = [[x, y, list(a), list(b)] for x, y, a, b in cmp["first"]]
        if cmp["error"]:
            res["status"], res["reason"], res["kind"] = "ERROR", cmp["error"], "decode"
        elif cmp["identical"]:
            res["status"], res["reason"] = "PASS", "identical"
            if want_stats:
                got = parse_stats(cli_stdout)
                res["stats"] = {k: {"allocated": a, "total": t} for k, (a, t) in got.items()}
                bad = check_stats(item["stats"], got)
                if bad:
                    res["status"], res["kind"] = "FAIL", "stats"
                    res["reason"] = "pixels identical but tile statistics exceed the manifest " \
                        "(tests/scripts/stats/_expectations.json): " + "; ".join(bad)
        else:
            res["status"], res["kind"] = "FAIL", "mismatch"
            res["reason"] = pixcmp.format_report(cmp, "ref", "cpp")
        return res
    except Exception as e:  # noqa: BLE001 - the harness never dies on one golden
        res["status"], res["reason"], res["kind"] = "ERROR", f"harness exception: {e!r}", "harness"
        return res
    finally:
        if item.get("keep_cli_png") and (workdir / "cpp.png").exists():
            eq = Path(cfg["run_out"]) / ".equal_to" / (name + ".png")
            eq.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(workdir / "cpp.png", eq)
        for r in (res.get("ref"), res.get("cli"), res.get("cli2")):
            if isinstance(r, dict):
                r.pop("stdout", None)
        if res["status"] in ("PASS", "PENDING") and not cfg["keep_all"]:
            shutil.rmtree(workdir, ignore_errors=True)
            res["dir"] = None


def _determinism_only(cfg: dict, script: Path, workdir: Path, res: dict) -> dict:
    """CLI-only: render twice; PASS iff both runs agree (identical PNG bytes, or the same clean
    rejection: same exit code, no PNG, same stderr). No reference, no pixel oracle."""
    runs = []
    for tag in ("cpp", "cpp2"):
        png = workdir / f"{tag}.png"
        r = run_renderer(cli_cmd(cfg, script, png), png, workdir / f"{tag}.log", cfg["timeout"])
        r.pop("stdout", None)
        runs.append((r, png))
    (c1, p1), (c2, p2) = runs
    res["cli"], res["cli2"] = c1, c2
    for r in (c1, c2):
        if r["timed_out"] or r["rc"] is None or r["rc"] < 0:
            res["status"], res["reason"], res["kind"] = "ERROR", "cli " + _describe(r), "cli-crash"
            return res
    if c1["rc"] == 0 and c2["rc"] == 0 and c1["png"] and c2["png"]:
        if p1.read_bytes() == p2.read_bytes():
            res["status"], res["reason"] = "PASS", "two renders byte-identical"
        else:
            cmp2 = pixcmp.compare(str(p1), str(p2), str(workdir / "diff-determinism.png"))
            res["status"], res["kind"] = "FAIL", "nondeterministic"
            res["reason"] = ("cli output differs between two runs (PNG bytes); decoded: "
                             + pixcmp.format_report(cmp2, "run1", "run2").splitlines()[0])
        return res
    if _clean_error(c1) and _clean_error(c2) and c1["rc"] == c2["rc"] \
            and c1["output"] == c2["output"]:
        res["status"] = "PASS"
        res["reason"] = "both runs rejected the script identically: " + _describe(c1)
        return res
    res["status"], res["kind"] = "FAIL", "nondeterministic"
    res["reason"] = f"runs disagree: run1 {_describe(c1)}; run2 {_describe(c2)}"
    return res


def _star(args):
    return run_one(*args)


# --------------------------------------------------------------------------------------------
# Suite
# --------------------------------------------------------------------------------------------
def make_cfg(cli: str, ref: str, out: str, mutate=None, jobs=None, timeout=600.0,
             determinism=True, keep_all=False, ref_cache=True, python=None,
             keep_failures=None, determinism_only=False) -> dict:
    ref_p = Path(ref)
    out_p = Path(out)
    run_out = out_p if mutate is None else out_p / f"mutate-{mutate}"
    return {
        "cli": str(Path(cli).resolve()) if os.sep in cli else cli,
        "ref": str(ref_p.resolve()),
        "python": python or sys.executable,
        "out": str(out_p.resolve()),
        "run_out": str(run_out.resolve()),
        "mutate": mutate,
        "jobs": jobs or os.cpu_count() or 1,
        "timeout": timeout,
        "determinism": determinism and mutate is None,
        "determinism_only": determinism_only and mutate is None,
        "keep_all": keep_all,
        "ref_cache": ref_cache,
        "ref_hash": ref_code_hash(ref_p) if ref_p.exists() else "missing",
        "keep_failures": keep_failures if keep_failures is not None else
        (None if mutate is None else 3),
    }


def probe_pending(cfg: dict, items: list[dict]) -> dict | None:
    """In mutation mode, run the CLI once to learn whether the hook exists."""
    if cfg["mutate"] is None or not items:
        return None
    item = next((i for i in items if not i["expect_error"] and not i["harness_error"]), items[0])
    probe_dir = Path(cfg["run_out"]) / ".probe"
    probe_dir.mkdir(parents=True, exist_ok=True)
    src = Path(item["path"])
    if item["has_expect_key"]:
        script = probe_dir / "script.json"
        script.write_text(strip_expect(src.read_text()))
    else:
        script = src
    r = run_renderer(cli_cmd(cfg, script, probe_dir / "cpp.png"), probe_dir / "cpp.png",
                     probe_dir / "cpp.log", cfg["timeout"])
    shutil.rmtree(probe_dir, ignore_errors=True)
    return r if pending_message(r, cfg["mutate"]) else None


def check_equal_to(cfg: dict, results: list[dict]) -> None:
    """Post-pass: every PASSing script with "equal_to" must have the same decoded RGBA as the CLI
    output of the named script (same run, same --mutate). Failures keep both PNGs and a diff."""
    run_out = Path(cfg["run_out"])
    eqdir = run_out / ".equal_to"
    by_name = {r["name"]: r for r in results}
    for r in results:
        tgt = r.get("equal_to")
        if not tgt or r["status"] != "PASS":
            continue
        mine, theirs = eqdir / (r["name"] + ".png"), eqdir / (tgt + ".png")
        t = by_name.get(tgt)
        if not theirs.exists() or not mine.exists():
            r["status"], r["kind"] = "FAIL", "equal_to"
            r["reason"] = (f"equal_to {tgt}: no CLI output to compare "
                           f"(target status {t['status'] if t else 'not run'})")
            continue
        workdir = run_out / r["name"]
        workdir.mkdir(parents=True, exist_ok=True)
        cmp = pixcmp.compare(str(theirs), str(mine), str(workdir / "diff-equal_to.png"))
        r["equal_to_compare"] = {"identical": cmp["identical"], "error": cmp["error"]}
        if cmp["identical"] and not cmp["error"]:
            r["reason"] = f"identical; equal to {tgt}"
            if not r.get("dir"):
                shutil.rmtree(workdir, ignore_errors=True)
            continue
        shutil.copyfile(theirs, workdir / "equal_to-target.png")
        shutil.copyfile(mine, workdir / "cpp.png")
        r["dir"] = str(workdir)
        r["status"], r["kind"] = "FAIL", "equal_to"
        r["reason"] = (f"equal_to {tgt}: " + (cmp["error"] or pixcmp.format_report(
            cmp, tgt, r["name"]).splitlines()[0]))
    if not cfg["keep_all"]:
        shutil.rmtree(eqdir, ignore_errors=True)


def run_suite(cfg: dict, items: list[dict], progress=None) -> dict:
    """Run the goldens; returns the report dict (also written to <run_out>/report.json)."""
    run_out = Path(cfg["run_out"])
    run_out.mkdir(parents=True, exist_ok=True)
    shutil.rmtree(run_out / ".equal_to", ignore_errors=True)
    t0 = time.monotonic()
    pend = probe_pending(cfg, items)
    results: list[dict] = []
    if pend is None:
        with ProcessPoolExecutor(max_workers=cfg["jobs"]) as ex:
            for r in ex.map(_star, [(cfg, it) for it in items], chunksize=1):
                results.append(r)
                if progress:
                    progress(r)
    results.sort(key=lambda r: r["name"])
    if not cfg.get("determinism_only"):
        check_equal_to(cfg, results)
    counts = {s: 0 for s in ("PASS", "FAIL", "ERROR", "PENDING")}
    for r in results:
        counts[r["status"]] += 1
    report = {
        "generated": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "cli": cfg["cli"], "ref": cfg["ref"], "mutate": cfg["mutate"],
        "determinism_checked": cfg["determinism"], "scripts": len(items),
        "counts": counts, "seconds": round(time.monotonic() - t0, 2),
        "pending": pend is not None,
        "pending_message": (pend["output"].strip()[-300:] if pend else None),
        "results": results,
    }
    if cfg["mutate"] is not None:
        fails = [r for r in results if r["status"] == "FAIL"]
        mism = [r for r in fails if r["kind"] != "cli-error"]
        cli_err = [r for r in results if r["kind"] == "cli-error"]
        expected_render = [r for r in results if not r["expect_error"]]
        suspect = (not mism and cli_err and len(cli_err) == len(expected_render))
        report["mutation"] = {
            "failed": len(fails), "pixel_or_contract_failures": len(mism),
            "cli_error_failures": len([r for r in fails if r["kind"] == "cli-error"]),
            "failed_names": [r["name"] for r in fails],
            "by_kind": {k: sum(1 for r in fails if r["kind"] == k)
                        for k in sorted({r["kind"] for r in fails})},
            "verdict": ("PENDING" if pend else "SUSPECT" if suspect else
                        "CAUGHT" if fails else "UNCAUGHT"),
        }
        kf = cfg["keep_failures"]
        if kf is not None:
            for r in fails[kf:]:
                if r["dir"]:
                    shutil.rmtree(r["dir"], ignore_errors=True)
                    r["dir"] = None
    (run_out / "report.json").write_text(json.dumps(report, indent=1))
    return report


def summary_line(report: dict) -> str:
    c = report["counts"]
    s = (f"SUMMARY: {report['scripts']} goldens  PASS {c['PASS']}  FAIL {c['FAIL']}  "
         f"ERROR {c['ERROR']}")
    if c["PENDING"]:
        s += f"  PENDING {c['PENDING']}"
    if report["mutate"] is not None:
        m = report["mutation"]
        s += f"  | mutation {report['mutate']}: {m['verdict']} ({m['failed']} goldens failed)"
    return s + f"  [{report['seconds']}s]"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__.split("\n", 1)[1])
    ap.add_argument("--cli", help="path to rasterloom-cli")
    ap.add_argument("--ref", default=str(REPO / "tests/reference/refcomp.py"))
    ap.add_argument("--scripts", default=str(REPO / "tests/scripts"))
    ap.add_argument("--out", default=str(REPO / "tests/output"))
    ap.add_argument("--filter", action="append", metavar="GLOB",
                    help="only goldens whose name or basename matches (repeatable)")
    ap.add_argument("--mutate", type=int, metavar="N")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    ap.add_argument("--expect-error-list", nargs="?", const="", default=None, metavar="FILE",
                    help="FILE: extra globs of expect-error goldens; no FILE: list them and exit")
    ap.add_argument("--single", action="store_true", help="skip the determinism double render")
    ap.add_argument("--determinism-only", action="store_true",
                    help="CLI only, no reference: every golden rendered twice must agree")
    ap.add_argument("--keep-all", action="store_true", help="keep outputs of passing goldens")
    ap.add_argument("--keep-failures", type=int, default=None,
                    help="keep at most N failing golden dirs (default: all; 3 with --mutate)")
    ap.add_argument("--no-ref-cache", action="store_true")
    ap.add_argument("--timeout", type=float, default=600.0, help="seconds per render")
    ap.add_argument("--python", default=sys.executable, help="interpreter for the reference")
    ap.add_argument("--verbose", "-v", action="store_true")
    args = ap.parse_args(argv)

    scripts = Path(args.scripts)
    if not scripts.is_dir():
        print(f"run_goldens: scripts dir not found: {scripts}", file=sys.stderr)
        return 2
    expect_globs = _load_expect_globs(args.expect_error_list or None)
    try:
        items = discover(scripts, args.filter, expect_globs)
        unknown = manifest_unknown_names(scripts)
    except ValueError as e:
        print(f"run_goldens: {e}", file=sys.stderr)
        return 2
    if unknown:
        print(f"run_goldens: {scripts / STATS_MANIFEST} names goldens that do not exist: "
              f"{unknown}", file=sys.stderr)
        return 2
    if args.expect_error_list == "":
        errs = [i["name"] for i in items if i["expect_error"]]
        for n in errs:
            print(n)
        print(f"{len(errs)} expect-error goldens of {len(items)}")
        return 0
    if not args.cli:
        ap.error("--cli is required")
    if not items:
        print("run_goldens: no golden scripts matched", file=sys.stderr)
        return 2
    if not args.determinism_only and not Path(args.ref).exists():
        print(f"run_goldens: reference not found: {args.ref}", file=sys.stderr)
        return 2
    if args.determinism_only and args.mutate is not None:
        ap.error("--determinism-only and --mutate are exclusive")

    cfg = make_cfg(args.cli, args.ref, args.out, mutate=args.mutate, jobs=args.jobs,
                   timeout=args.timeout, determinism=not args.single, keep_all=args.keep_all,
                   ref_cache=not args.no_ref_cache, python=args.python,
                   keep_failures=args.keep_failures, determinism_only=args.determinism_only)

    def progress(r):
        if r["status"] != "PASS" or args.verbose:
            first = r["reason"].splitlines()[0] if r["reason"] else ""
            print(f"{r['status']:5} {r['name']}: {first}", flush=True)

    report = run_suite(cfg, items, progress)
    for r in report["results"]:
        if r.get("kind") == "equal_to":  # decided after the parallel pass, so not yet printed
            print(f"{r['status']:5} {r['name']}: {r['reason'].splitlines()[0]}", flush=True)
    if report["pending"]:
        print(f"PENDING: {report['pending_message']}")
    print(summary_line(report))
    print(f"report: {Path(cfg['run_out']) / 'report.json'}")
    if args.mutate is not None:
        v = report["mutation"]["verdict"]
        return {"CAUGHT": 0, "UNCAUGHT": 1, "PENDING": EXIT_PENDING, "SUSPECT": EXIT_SUSPECT}[v]
    c = report["counts"]
    return 0 if c["PASS"] == report["scripts"] and report["scripts"] > 0 else 1


if __name__ == "__main__":
    sys.exit(main())
