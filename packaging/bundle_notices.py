#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""bundle_notices.py - THIRD-PARTY-NOTICES.md for exactly what an AppDir bundles.

    bundle_notices.py --appdir AppDir --qmake QMAKE --build-dir BUILD --out FILE
                      [--manifest-out FILE] [--components packaging/bundle-components.yaml]

1. Lists every shared library in AppDir/usr/lib and every Qt plugin in AppDir/usr/plugins.
2. Classifies each by packaging/bundle-components.yaml; an unclassified library is an error.
3. For each library finds the host file that was bundled and reads its version and license text
   from the host: pacman (Arch) or dpkg (Debian/Ubuntu) for distribution packages, `qmake -query`
   for an aqtinstall Qt (license: COPYING.LESSER), the build tree for ADS, and
   packaging/licenses/ICU-<major>.txt for the ICU that ships inside an aqtinstall Qt.
4. Writes a data/third_party.yaml-format manifest and runs tools/gen_third_party_notices.py on it,
   so the notice file has the project's one format and the generator's missing-text checks.
Exit 0 ok, 1 on any unclassified library, unresolved version or missing license text.
"""
from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

try:
    import yaml
except ImportError:  # pragma: no cover
    print("bundle_notices: PyYAML is required", file=sys.stderr)
    sys.exit(2)

REPO = Path(__file__).resolve().parents[1]
PKG = REPO / "packaging"


def run(cmd: list[str]) -> str | None:
    try:
        cp = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
                            timeout=60)
    except (OSError, subprocess.TimeoutExpired):
        return None
    return cp.stdout if cp.returncode == 0 else None


def ldconfig_map() -> dict[str, list[Path]]:
    out: dict[str, list[Path]] = {}
    text = run(["ldconfig", "-p"]) or run(["/sbin/ldconfig", "-p"]) or ""
    for line in text.splitlines():
        m = re.match(r"\s*(\S+) \((libc6,x86-64[^)]*)\) => (\S+)", line)
        if m:
            out.setdefault(m.group(1), []).append(Path(m.group(3)))
    return out


class Host:
    """Package-manager lookups for one host (Arch or Debian-family)."""

    def __init__(self) -> None:
        self.kind = "pacman" if shutil.which("pacman") else "dpkg" if shutil.which("dpkg") else None

    def owner(self, path: Path) -> str | None:
        cands = [path, path.resolve()]
        # usr-merge: dpkg records /lib/x86_64-linux-gnu/... for files that live in /usr/lib/...
        s = str(path.resolve())
        if s.startswith("/usr/lib/"):
            cands.append(Path(s[len("/usr"):]))
        for c in cands:
            if self.kind == "pacman":
                o = run(["pacman", "-Qqo", str(c)])
                if o:
                    return o.split()[0]
            elif self.kind == "dpkg":
                o = run(["dpkg", "-S", str(c)])
                if o:
                    return o.split(":")[0].strip()
        return None

    def version(self, pkg: str) -> str | None:
        if self.kind == "pacman":
            o = run(["pacman", "-Q", pkg])
            return o.split()[1] if o else None
        o = run(["dpkg-query", "-W", "-f=${Version}", pkg])
        return o.strip() if o else None

    def license_files(self, pkg: str) -> list[Path]:
        if self.kind == "dpkg":
            p = Path(f"/usr/share/doc/{pkg}/copyright")
            return [p] if p.is_file() else []
        files = []
        for line in (run(["pacman", "-Qlq", pkg]) or "").splitlines():
            p = Path(line)
            if line.startswith("/usr/share/licenses/") and p.is_file():
                files.append(p)
        files.sort()
        # Many packages ship only a notice (e.g. an exception) and rely on the common SPDX text
        # in the `licenses` package: add each SPDX id's text unless a package file already
        # carries it (same first line).
        info = run(["pacman", "-Qi", pkg]) or ""
        m = re.search(r"^Licenses\s*:\s*(.+)$", info, re.M)
        ids = re.findall(r"[A-Za-z0-9.+-]+", m.group(1)) if m else []
        have = " ".join(p.read_text(encoding="utf-8", errors="replace") for p in files)
        for i in ids:
            p = Path(f"/usr/share/licenses/spdx/{i}.txt")
            if i in ("AND", "OR", "WITH") or not p.is_file():
                continue
            title = next((ln.strip() for ln in p.read_text(errors="replace").splitlines()
                          if ln.strip()), "")
            if title and title not in have:
                files.append(p)
                have += title
        return files


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__.split("\n", 1)[1])
    ap.add_argument("--appdir", required=True, type=Path)
    ap.add_argument("--qmake", required=True)
    ap.add_argument("--build-dir", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--manifest-out", type=Path)
    ap.add_argument("--components", type=Path, default=PKG / "bundle-components.yaml")
    args = ap.parse_args(argv)

    spec = yaml.safe_load(args.components.read_text())
    comps = spec["bundled"]
    qt_version = (run([args.qmake, "-query", "QT_VERSION"]) or "").strip()
    qt_libs = Path((run([args.qmake, "-query", "QT_INSTALL_LIBS"]) or "").strip())
    host = Host()
    ldmap = ldconfig_map()

    # 1. what the AppDir bundles
    libdir = args.appdir / "usr/lib"
    libs = sorted(p.name for p in libdir.iterdir() if ".so" in p.name and not p.is_symlink())
    plugins = sorted(str(p.relative_to(args.appdir / "usr/plugins"))
                     for p in (args.appdir / "usr/plugins").rglob("*.so"))
    items = [(n, n) for n in libs] + [(f"plugin:{p}", p) for p in plugins]

    # 2. classify
    problems: list[str] = []
    members: dict[str, list[str]] = {c["name"]: [] for c in comps}
    for key, shown in items:
        hit = next((c for c in comps if any(re.fullmatch(rx, key) for rx in c["files"])), None)
        if hit is None:
            problems.append(f"{shown}: bundled but not classified in {args.components.name}")
            continue
        members[hit["name"]].append(shown)

    # 3. versions and license texts from the host
    tmp = Path(tempfile.mkdtemp(prefix="rl-notices-"))
    manifest = []
    for c in comps:
        files = members[c["name"]]
        if not files:
            continue
        entry = {k: c[k] for k in ("name", "spdx", "url")}
        libs_only = [f for f in files if "/" not in f]
        n_plugins = len(files) - len(libs_only)
        shipped = ", ".join(libs_only) + (f" + {n_plugins} Qt plugins" if n_plugins else "")
        entry["used_for"] = f"{c['used_for']} (bundled: {shipped})"
        version, lic_files, origin = None, [], ""
        if c["name"] == "Qt":
            qpkg = host.owner(Path(shutil.which(args.qmake) or args.qmake))
            version = qt_version or None
            origin = f"{host.kind} package {qpkg}" if qpkg else "official Qt binaries (aqtinstall)"
            lic_files = [REPO / "COPYING.LESSER"]
        elif c["name"] == "Qt-Advanced-Docking-System":
            m = re.search(r"\.so\.(\d+\.\d+\.\d+)$", " ".join(files))
            version = m.group(1) if m else None
            lic = args.build_dir / "_deps/qtads-src/LICENSE"
            lic_files = [lic] if lic.is_file() else [REPO / "data/licenses/LGPL-2.1.txt"]
            origin = "built from source by this build"
        else:
            # Resolve every bundled file of the component on the host: one component can span
            # several distribution packages (xcb-util-*, libxkbcommon + -x11).
            found: dict[str, tuple[str | None, list[Path]]] = {}
            for name in libs_only:
                cands = list(ldmap.get(name, [])) + [qt_libs / name]
                src = next((p for p in cands if p.exists()), None)
                pkg = host.owner(src) if src is not None else None
                if src is None:
                    problems.append(f"{name}: cannot find the host file that was bundled")
                elif pkg is not None:
                    if pkg not in found:
                        found[pkg] = (host.version(pkg), host.license_files(pkg))
                elif src.parent == qt_libs or qt_libs in src.parents:
                    # not a distribution file: shipped inside a Qt install (aqtinstall's ICU)
                    m = re.search(r"\.so\.(\d+)", name)
                    major = m.group(1) if m else "?"
                    mv = re.search(r"\.so\.([\d.]+)$", src.resolve().name)
                    lic = PKG / f"licenses/ICU-{major}.txt"
                    key = f"shipped with Qt {qt_version}"
                    if key not in found:
                        found[key] = (mv.group(1) if mv else major,
                                      [lic] if c["name"] == "ICU" and lic.is_file() else [])
                else:
                    problems.append(f"{name}: {src} is not owned by any {host.kind} package")
            if found:
                if any(v is None for v, _ in found.values()):
                    version = None
                elif len(found) == 1:
                    (k, (v, _)), = found.items()
                    version = v
                    origin = k if k.startswith("shipped") else f"{host.kind} package {k}"
                else:
                    version = "; ".join(f"{k} {v}" for k, (v, _) in found.items())
                    origin = f"{host.kind} packages"
                for _, (_, lf) in found.items():
                    for f in lf:
                        if f not in lic_files:
                            lic_files.append(f)
                if any(not lf for _, lf in found.values()):
                    lic_files = []
        if version is None:
            problems.append(f"{c['name']}: version unresolved")
        if not lic_files:
            problems.append(f"{c['name']}: no license text found ({origin})")
        entry["version"] = f"{version} ({origin})" if version else None
        if len(lic_files) > 1:
            combined = tmp / (re.sub(r"[^A-Za-z0-9]+", "_", c["name"]) + ".txt")
            combined.write_text("\n\n".join(
                f"==== {p} ====\n\n" + p.read_text(encoding="utf-8", errors="replace").rstrip()
                for p in lic_files) + "\n")
            lic_files = [combined]
        entry["license_files"] = [str(p) for p in lic_files]
        manifest.append(entry)
    for c in spec.get("compiled_in", []):
        manifest.append(dict(c, used_for=c["used_for"]))

    if problems:
        for p in problems:
            print("ERROR " + p)
        print("bundle_notices: nothing written")
        return 1
    mpath = args.manifest_out or (tmp / "manifest.yaml")
    header = ("# Generated by packaging/bundle_notices.py from the AppDir; the input of\n"
              "# tools/gen_third_party_notices.py for this bundle. Do not edit.\n")
    mpath.write_text(header + yaml.safe_dump({"components": manifest}, sort_keys=False, width=100))
    args.out.parent.mkdir(parents=True, exist_ok=True)
    rc = subprocess.call([sys.executable, str(REPO / "tools/gen_third_party_notices.py"),
                          "--data", str(mpath), "--out", str(args.out)])
    text = args.out.read_text() if args.out.is_file() else ""
    # Combined texts come from a temporary directory: show only their file name.
    if str(tmp) in text:
        args.out.write_text(text.replace(str(tmp) + "/", "combined license texts: "))
    print(f"bundle_notices: {len(libs)} libraries + {len(plugins)} Qt plugins in "
          f"{len(manifest)} components")
    return rc


if __name__ == "__main__":
    sys.exit(main())
