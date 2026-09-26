#!/usr/bin/env python3
"""Rasterloom reference renderer (the oracle).

    python3 tests/reference/refcomp.py SCRIPT.json OUT.png

Exit 0 and write OUT.png (8-bit RGBA, bytes exactly the final composite) on success.
Exit 2 and write nothing on a script error (C9); exit 3 on an internal error (a reference bug).
Written from docs/math/ only; see tests/reference/README.md.
"""

import os
import sys
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from rc.core import ScriptError           # noqa: E402
from rc.runner import render_script_text  # noqa: E402


def write_png(rgba, path):
    import numpy as np
    from PIL import Image
    tmp = path + ".tmp-refcomp"
    Image.fromarray(np.ascontiguousarray(rgba)).save(tmp, format="PNG")
    os.replace(tmp, path)


def main(argv):
    if len(argv) != 3:
        sys.stderr.write("usage: refcomp.py SCRIPT.json OUT.png\n")
        return 64
    try:
        with open(argv[1], "rb") as fh:
            text = fh.read().decode("utf-8")
    except (OSError, UnicodeDecodeError) as e:
        sys.stderr.write("refcomp: cannot read script: %s\n" % e)
        return 2
    try:
        rgba = render_script_text(text)
    except ScriptError as e:
        sys.stderr.write("refcomp: script error: %s\n" % e)
        return 2
    except Exception:
        traceback.print_exc()
        sys.stderr.write("refcomp: INTERNAL ERROR (reference bug)\n")
        return 3
    write_png(rgba, argv[2])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
