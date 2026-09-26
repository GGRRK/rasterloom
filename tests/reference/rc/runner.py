"""Script runner: parse (C9), execute ops with C10 history, render the output."""

import json

from .composite import render_document
from .core import ScriptError
from .fields import FieldReader, parse_color
from .model import Document
from .registry import OPS, load_all, op

HISTORY_DEPTH = 1000     # C10: render scripts run with history depth 1000

_active_history = None   # the running script's history (list of snapshots), for `undo`


def _reject_constant(name):
    raise ScriptError("invalid JSON number %s" % name)


def _no_duplicate_keys(pairs):
    out = {}
    for k, v in pairs:
        if k in out:
            raise ScriptError("duplicate JSON key %r" % k)
        out[k] = v
    return out


def parse_json(text):
    try:
        return json.loads(text, parse_constant=_reject_constant,
                          object_pairs_hook=_no_duplicate_keys)
    except ValueError as e:     # json.JSONDecodeError (and bad UTF-8) is a script error
        raise ScriptError("invalid JSON: %s" % e)


@op("undo", history=False)
def undo(doc, f):
    steps = f.int("steps", 1, 1, None)
    hist = _active_history
    if hist is None or steps > len(hist):
        raise ScriptError("undo: only %d history record(s) available, %d requested"
                          % (0 if hist is None else len(hist), steps))
    snap = None
    for _ in range(steps):
        snap = hist.pop()
    doc.restore(snap)


def run_script(script):
    """Execute a parsed script (dict); returns the final document."""
    global _active_history
    load_all()
    top = FieldReader(script, "script")
    canvas = top.obj("canvas")
    w = canvas.int("w", lo=1, hi=16384)
    h = canvas.int("h", lo=1, hi=16384)
    bg = canvas.color("bg", "#00000000")
    if bg[3] == 0:
        bg = (0, 0, 0, 0)
    out = top.str("out")
    if out != "png8":
        raise ScriptError("script.out must be \"png8\"")
    ops_raw = top.raw("ops")
    if not isinstance(ops_raw, list):
        raise ScriptError("script.ops must be an array")
    top.check_unused()          # script + canvas keys; each op object is checked after it runs
    ops = [FieldReader(o, "ops[%d]" % i) for i, o in enumerate(ops_raw)]

    doc = Document(w, h, bg)
    keep_history = any(isinstance(o.data.get("op"), str) and o.data.get("op") == "undo" for o in ops)
    history = []
    _active_history = history
    try:
        for i, f in enumerate(ops):
            f.path = "ops[%d]" % i
            name = f.str("op")
            spec = OPS.get(name)
            if spec is None:
                raise ScriptError("%s: unknown op %r" % (f.path, name))
            f.path = "ops[%d] (%s)" % (i, name)
            snap = doc.snapshot() if (spec.history and keep_history) else None
            spec.handler(doc, f)
            f.check_unused()
            if doc.w > 16384 or doc.h > 16384:
                raise ScriptError("%s: result larger than 16384 px per axis" % f.path)
            if snap is not None:
                history.append(snap)
                if len(history) > HISTORY_DEPTH:
                    history.pop(0)
    finally:
        _active_history = None
    return doc


def render_script_text(text):
    """JSON text -> final RGBA uint8 (h, w, 4) array (the `png8` output)."""
    script = parse_json(text)
    if not isinstance(script, dict):
        raise ScriptError("script must be a JSON object")
    doc = run_script(script)
    return render_document(doc, onto_bg=True)
