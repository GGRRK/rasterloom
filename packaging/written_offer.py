#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""written_offer.py - WRITTEN-OFFER.txt for one AppImage build (BUILD-SPEC requirement 15).

    written_offer.py --qmake QMAKE --build-dir BUILD --glibc-floor X.Y --out FILE

Records what a recipient needs to exercise the LGPL-3.0 (Qt), LGPL-2.1 (Qt-Advanced-Docking-
System) and GPL (Rasterloom, JBIG-KIT) source rights for this exact bundle: the Rasterloom git
commit, the Qt source tarball URL for the bundled Qt version and its configure options (verbatim
from the Qt install's config_*.opt files when present - aqtinstall/official binaries - otherwise
the distribution recipe that built it), the ADS source, and a verbatim relink recipe.
The source location defaults to https://github.com/GGRRK/rasterloom; set RL_SOURCE_URL to the
public repository before a release. No absolute build paths are written.
"""
from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]


def run(cmd: list[str], cwd: Path | None = None) -> str:
    try:
        cp = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                            text=True, timeout=60)
        return cp.stdout.strip() if cp.returncode == 0 else ""
    except (OSError, subprocess.TimeoutExpired):
        return ""


def git_sha() -> str:
    sha = os.environ.get("GITHUB_SHA") or run(["git", "rev-parse", "HEAD"], REPO)
    if not sha:
        return "unknown (not built from a git checkout)"
    dirty = run(["git", "status", "--porcelain", "--untracked-files=no"], REPO)
    return sha + (" (plus uncommitted changes: a local test build, not a release)" if dirty else "")


def cache_value(build: Path, key: str) -> str:
    cache = build / "CMakeCache.txt"
    if cache.is_file():
        m = re.search(rf"^{re.escape(key)}:[A-Z]+=(.*)$", cache.read_text(errors="replace"), re.M)
        if m:
            return m.group(1).strip()
    return ""


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--qmake", required=True)
    ap.add_argument("--build-dir", required=True, type=Path)
    ap.add_argument("--glibc-floor", required=True)
    ap.add_argument("--out", required=True, type=Path)
    args = ap.parse_args(argv)

    qt_ver = run([args.qmake, "-query", "QT_VERSION"])
    if not re.fullmatch(r"\d+\.\d+\.\d+", qt_ver):
        print(f"written_offer: cannot read the Qt version from {args.qmake}", file=sys.stderr)
        return 1
    mm = ".".join(qt_ver.split(".")[:2])
    qt_prefix = Path(run([args.qmake, "-query", "QT_INSTALL_PREFIX"]))
    qt_archive = f"https://download.qt.io/archive/qt/{mm}/{qt_ver}"
    src_url = os.environ.get("RL_SOURCE_URL", "https://github.com/GGRRK/rasterloom")
    version = re.search(r"VERSION (\d+\.\d+\.\d+)", (REPO / "CMakeLists.txt").read_text()).group(1)
    ads_ver, ads_url, ads_sha = "", "", ""
    src_cmake = (REPO / "src/CMakeLists.txt").read_text()
    if m := re.search(r"set\(RL_ADS_VERSION ([\d.]+)\)", src_cmake):
        ads_ver = m.group(1)
    if m := re.search(r"URL (https://\S+)", src_cmake):
        ads_url = m.group(1).replace("${RL_ADS_VERSION}", ads_ver)
    if m := re.search(r"URL_HASH SHA256=([0-9a-f]{64})", src_cmake):
        ads_sha = m.group(1)
    compiler = cache_value(args.build_dir, "CMAKE_CXX_COMPILER")
    compiler_ver = run([compiler, "--version"]).splitlines()[0] if compiler else ""
    build_type = cache_value(args.build_dir, "CMAKE_BUILD_TYPE")

    # Qt configure options: verbatim from the install when it records them.
    opts = []
    for mod in ("qtbase", "qtsvg", "qtwayland"):
        f = qt_prefix / f"config_{mod}.opt"
        if f.is_file():
            lines = [ln for ln in f.read_text(errors="replace").splitlines() if ln.strip()]
            # Qt's own CI paths (e.g. its OpenSSL root) are Qt's build machine, not ours.
            opts.append(f"  {mod} ({f.name}, {len(lines)} options):\n" +
                        "\n".join(f"    {ln}" for ln in lines))
    if opts:
        qt_origin = ("Official Qt binaries installed with aqtinstall (built by The Qt Company on "
                     "RHEL 8.10 / glibc 2.28). Configure options, verbatim from the install's "
                     "config_*.opt files:")
        qt_opts = "\n".join(opts)
    else:
        owner = ""
        qm = shutil.which(args.qmake) or args.qmake
        if shutil.which("pacman"):
            o = run(["pacman", "-Qqo", qm])
            if o:
                owner = f"{o} {run(['pacman', '-Q', o]).split()[-1]}"
                pkgver = owner.split()[-1]
                qt_origin = (f"Arch Linux package {owner} (personal test build; the release "
                             "AppImage bundles the official Qt binaries instead). Its configure "
                             "options are the cmake invocation in the package recipe:")
                qt_opts = ("  https://gitlab.archlinux.org/archlinux/packaging/packages/qt6-base/"
                           f"-/blob/{pkgver}/PKGBUILD\n  (and the qt6-svg / qt6-wayland recipes at "
                           "the same place for their versions)")
        elif shutil.which("dpkg"):
            o = run(["dpkg", "-S", qm]).split(":")[0]
            if o:
                owner = o
                qt_origin = (f"Debian/Ubuntu package {o}. Its configure options are in the source "
                             "package's debian/rules (apt-get source " + o + "):")
                qt_opts = "  debian/rules of the source package"
        if not owner:
            qt_origin = "A Qt build of unknown origin; its configure options are not recorded."
            qt_opts = "  (not recorded)"

    text = f"""WRITTEN OFFER FOR SOURCE CODE - Rasterloom {version} AppImage
==============================================================

Rasterloom is licensed under the GNU General Public License, version 3 or later (LICENSE). This
AppImage also contains libraries under the GNU Lesser General Public License (Qt: LGPL-3.0, see
COPYING.LESSER; Qt-Advanced-Docking-System: LGPL-2.1-or-later) and under the GNU GPL (JBIG-KIT,
GPL-2.0-or-later, when libtiff pulls it in). THIRD-PARTY-NOTICES.md lists every bundled library,
its version, license and full license text.

For at least three years from the date this build was distributed, and for as long as we offer
spare parts or support for it, any third party may obtain the complete corresponding source code
for everything in this AppImage - Rasterloom itself and every bundled library at the exact version
shipped - on a medium customarily used for software interchange, for no more than the cost of
physically performing the transfer. Ask by opening an issue at
  {src_url}/issues
quoting the git commit below. The same source is also available online, free of charge:

1. Rasterloom
   Source repository: {src_url}
   Git commit of this build: {git_sha()}
   Build: CMake {build_type or '?'}, {compiler_ver or 'compiler not recorded'},
          flags -march=x86-64-v2 -O2 -fno-fast-math -ffp-contract=off (cmake/RasterloomFlags.cmake)
   The AppImage was produced by packaging/build-appimage.sh from that commit.

2. Qt {qt_ver} (LGPL-3.0-only), dynamically linked: usr/lib/libQt6*.so.6 and usr/plugins/
   Complete source (all modules):
     {qt_archive}/single/qt-everywhere-src-{qt_ver}.tar.xz
   The modules this bundle uses, individually:
     {qt_archive}/submodules/qtbase-everywhere-src-{qt_ver}.tar.xz
     {qt_archive}/submodules/qtsvg-everywhere-src-{qt_ver}.tar.xz
     {qt_archive}/submodules/qtwayland-everywhere-src-{qt_ver}.tar.xz
   Build configuration: {qt_origin}
{qt_opts}

3. Qt-Advanced-Docking-System {ads_ver} (LGPL-2.1-or-later), shared library usr/lib/libqtadvanceddocking-qt6.so.*
   Source: {ads_url}
   SHA-256: {ads_sha}
   Built unmodified by Rasterloom's CMake (FetchContent) with BUILD_STATIC=OFF, BUILD_EXAMPLES=OFF.

4. Every other library in usr/lib is an unmodified binary from the build host's distribution
   package named, with its exact version, in THIRD-PARTY-NOTICES.md. Its source is that
   distribution's source package at that version (Ubuntu/Debian: `apt-get source <package>=<version>`;
   Arch Linux: the package recipe at gitlab.archlinux.org/archlinux/packaging/packages/<package>
   at the tag of that version), or a copy from us under the offer above.

RELINKING RASTERLOOM AGAINST A MODIFIED QT (or ADS)
---------------------------------------------------
Rasterloom never links Qt statically. The loader resolves Qt from usr/lib inside the AppImage, so
using a modified Qt needs no Rasterloom object files. Either of these works; the commands are
meant to be run verbatim, with /path/to/your-qt replaced by the prefix of your Qt build.

A. Swap the libraries inside this AppImage (same Qt minor version {mm}, ABI compatible):

   ./Rasterloom-x86_64.AppImage --appimage-extract            # unpacks to ./squashfs-root
   cp -a /path/to/your-qt/lib/libQt6*.so.6* squashfs-root/usr/lib/
   cp -a /path/to/your-qt/plugins/. squashfs-root/usr/plugins/    # optional: your plugins too
   ./squashfs-root/AppRun --version --verbose                  # reports the Qt it now runs on
   ./squashfs-root/AppRun                                      # run it
   # optional: repack with appimagetool (https://github.com/AppImage/appimagetool)
   appimagetool squashfs-root Rasterloom-custom-x86_64.AppImage

   For a modified ADS, replace squashfs-root/usr/lib/libqtadvanceddocking-qt6.so.* the same way.

B. Rebuild Rasterloom from source against your Qt and package it the same way we do:

   git clone {src_url}.git rasterloom && cd rasterloom
   git checkout {git_sha().split(' ')[0]}
   cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/path/to/your-qt
   cmake --build build
   QMAKE=/path/to/your-qt/bin/qmake packaging/build-appimage.sh --build-dir build --no-build

   (Requirements: CMake >= 3.21, Ninja, GCC >= 11, libpng, libjpeg-turbo, libtiff, zlib and
   nlohmann-json development files; see README.md. The AppImage it writes requires glibc >=
   {args.glibc_floor} when built on the same system as this one.)
"""
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(text)
    home = os.path.expanduser("~")
    leaks = [ln for ln in text.splitlines() if home != "/" and home in ln]
    if leaks:
        print("written_offer: refusing a home-directory path in the offer:\n  " +
              "\n  ".join(leaks), file=sys.stderr)
        return 1
    print(f"wrote {args.out} (Qt {qt_ver}, ADS {ads_ver})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
