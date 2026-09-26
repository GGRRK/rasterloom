#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""elfdeps.py - ELF helpers for the AppImage scripts (readelf-based, no third-party modules).

    elfdeps.py prune APPDIR     remove bundled usr/lib libraries that no bundled ELF needs any more
                                (after build-appimage.sh deleted the excluded host libraries,
                                their private dependencies, e.g. glib's libpcre2-8, are orphans)
    elfdeps.py needed APPDIR    print every DT_NEEDED soname that is neither bundled nor a known
                                host library, i.e. what the target system must provide
"""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

# Never pruned: the Qt/ADS/ICU core is loaded through plugins and Qt's own dlopen() calls.
KEEP_PREFIXES = ("libQt6", "libqtadvanceddocking", "libicu")


def is_elf(p: Path) -> bool:
    try:
        with p.open("rb") as f:
            return f.read(4) == b"\x7fELF"
    except OSError:
        return False


def needed(p: Path) -> list[str]:
    cp = subprocess.run(["readelf", "-d", "--wide", str(p)], stdout=subprocess.PIPE,
                        stderr=subprocess.DEVNULL, text=True)
    out = []
    for line in cp.stdout.splitlines():
        if "(NEEDED)" in line and "[" in line:
            out.append(line.split("[", 1)[1].split("]", 1)[0])
    return out


def elves(appdir: Path) -> list[Path]:
    return sorted(p for p in appdir.rglob("*") if p.is_file() and not p.is_symlink() and is_elf(p))


def prune(appdir: Path) -> int:
    libdir = appdir / "usr/lib"
    removed = []
    while True:
        all_needed = set()
        for e in elves(appdir):
            all_needed.update(needed(e))
        orphans = [p for p in sorted(libdir.iterdir())
                   if p.is_file() and ".so" in p.name and not p.name.startswith(KEEP_PREFIXES)
                   and p.name not in all_needed]
        if not orphans:
            break
        for p in orphans:
            p.unlink()
            removed.append(p.name)
    for n in removed:
        print(f"pruned orphan usr/lib/{n}")
    print(f"elfdeps prune: {len(removed)} orphan(s) removed")
    return 0


def host_needed(appdir: Path) -> int:
    bundled = {p.name for p in (appdir / "usr/lib").iterdir()}
    names = set()
    for e in elves(appdir):
        names.update(needed(e))
    for n in sorted(names - bundled):
        print(n)
    return 0


def main(argv: list[str]) -> int:
    if len(argv) != 3 or argv[1] not in ("prune", "needed"):
        print(__doc__, file=sys.stderr)
        return 2
    appdir = Path(argv[2])
    return prune(appdir) if argv[1] == "prune" else host_needed(appdir)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
