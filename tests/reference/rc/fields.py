"""Strict JSON field reader (C9, doc 10 §11 common rules).

A FieldReader wraps one JSON object. Every accessor marks its key as used; after an op runs,
`check_unused()` (called by the runner, recursively through child readers) turns any key that
was never read into a script error. So an op that only reads the fields belonging to the chosen
variant (e.g. `from`/`to` only when `fill == "gradient"`) automatically rejects the others.
"""

import re

from .core import ScriptError

REQUIRED = object()          # sentinel: the field has no default
SEED_LIMIT = 2 ** 53         # seeds: JSON integers in [0, 2^53)

_COLOR_RE = re.compile(r"^#([0-9A-Fa-f]{6}|[0-9A-Fa-f]{8})$")


def is_int(v):
    """A JSON integer token (Python int, never bool). `1.0` is NOT an int (see README)."""
    return isinstance(v, int) and not isinstance(v, bool)


def is_number(v):
    return (isinstance(v, (int, float))) and not isinstance(v, bool)


def parse_color(s, where="colour"):
    """"#RRGGBB" (alpha FF) or "#RRGGBBAA", case-insensitive -> (r, g, b, a) ints. Not canonicalised."""
    if not isinstance(s, str) or not _COLOR_RE.match(s):
        raise ScriptError("%s: bad colour %r" % (where, s))
    hx = s[1:]
    if len(hx) == 6:
        hx += "FF"
    return tuple(int(hx[i:i + 2], 16) for i in (0, 2, 4, 6))


class FieldReader:
    def __init__(self, obj, path="ops"):
        if not isinstance(obj, dict):
            raise ScriptError("%s: expected a JSON object" % path)
        self.data = obj
        self.path = path
        self.used = set()
        self.children = []

    # -- bookkeeping ------------------------------------------------------------------------
    def has(self, name):
        """True if the field is present (does NOT mark it used)."""
        return name in self.data

    def mark_used(self, name):
        self.used.add(name)

    def _get(self, name, default):
        self.used.add(name)
        if name in self.data:
            return True, self.data[name]
        if default is REQUIRED:
            raise ScriptError("%s: missing required field %r" % (self.path, name))
        return False, default

    def check_unused(self):
        extra = sorted(k for k in self.data if k not in self.used)
        if extra:
            raise ScriptError("%s: unknown field(s) %s" % (self.path, ", ".join(map(repr, extra))))
        for c in self.children:
            c.check_unused()

    def err(self, msg):
        return ScriptError("%s: %s" % (self.path, msg))

    # -- typed accessors --------------------------------------------------------------------
    def str(self, name, default=REQUIRED, choices=None, nonempty=False):
        present, v = self._get(name, default)
        if not present:
            return v
        if not isinstance(v, str):
            raise self.err("%r must be a string" % name)
        if nonempty and v == "":
            raise self.err("%r must be non-empty" % name)
        if choices is not None and v not in choices:
            raise self.err("%r: %r is not one of %s" % (name, v, sorted(choices)))
        return v

    def int(self, name, default=REQUIRED, lo=None, hi=None):
        """Integer in [lo, hi] inclusive (either bound may be None)."""
        present, v = self._get(name, default)
        if not present:
            return v
        if not is_int(v):
            raise self.err("%r must be an integer" % name)
        if (lo is not None and v < lo) or (hi is not None and v > hi):
            raise self.err("%r = %r out of range [%s, %s]" % (name, v, lo, hi))
        return v

    def seed(self, name, default=REQUIRED):
        return self.int(name, default, 0, SEED_LIMIT - 1)

    def num(self, name, default=REQUIRED, lo=None, hi=None, lo_open=False, hi_open=False):
        """A JSON number as binary64 (Python float() is correctly rounded), range-checked, never clamped."""
        present, v = self._get(name, default)
        if not present:
            return v
        if not is_number(v):
            raise self.err("%r must be a number" % name)
        try:
            v = float(v)
        except OverflowError:
            raise self.err("%r out of range" % name)
        if v != v or v in (float("inf"), float("-inf")):
            raise self.err("%r must be finite" % name)
        if lo is not None and (v < lo or (lo_open and v == lo)):
            raise self.err("%r = %r out of range" % (name, v))
        if hi is not None and (v > hi or (hi_open and v == hi)):
            raise self.err("%r = %r out of range" % (name, v))
        return v

    def unit_num(self, name, default=REQUIRED):
        """A double in [0.0, 1.0] (opacity, fill, ...)."""
        return self.num(name, default, 0.0, 1.0)

    def bool(self, name, default=REQUIRED):
        present, v = self._get(name, default)
        if not present:
            return v
        if not isinstance(v, bool):
            raise self.err("%r must be true or false" % name)
        return v

    def color(self, name, default=REQUIRED):
        """Colour string -> (r, g, b, a) ints (a default like "#000000FF" is parsed too)."""
        present, v = self._get(name, default)
        return parse_color(v, "%s.%s" % (self.path, name))

    def grey(self, name, default=REQUIRED):
        """Grey value: JSON integer 0..255."""
        return self.int(name, default, 0, 255)

    def rect(self, name, w, h):
        """[x, y, w, h] integers, w, h >= 1; default the whole canvas [0, 0, W, H]."""
        present, v = self._get(name, [0, 0, w, h])
        if not isinstance(v, list) or len(v) != 4 or not all(is_int(e) for e in v):
            raise self.err("%r must be [x, y, w, h] integers" % name)
        if v[2] < 1 or v[3] < 1:
            raise self.err("%r: w and h must be >= 1" % name)
        return tuple(v)

    def raw(self, name, default=REQUIRED):
        """The raw JSON value (caller validates it)."""
        return self._get(name, default)[1]

    def obj(self, name, default=REQUIRED):
        """A nested JSON object as a child FieldReader (checked for unknown keys with the parent).

        With a default of `{}` an absent field yields an empty reader."""
        present, v = self._get(name, default)
        child = FieldReader(v, "%s.%s" % (self.path, name))
        self.children.append(child)
        return child

    def obj_list(self, name, default=REQUIRED):
        """A JSON array of objects -> list of child FieldReaders (each checked for unknown keys)."""
        present, v = self._get(name, default)
        if not isinstance(v, list):
            raise self.err("%r must be an array" % name)
        out = []
        for i, e in enumerate(v):
            c = FieldReader(e, "%s.%s[%d]" % (self.path, name, i))
            self.children.append(c)
            out.append(c)
        return out
