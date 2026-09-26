#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# build-appimage.sh - build Rasterloom-x86_64.AppImage (+ .sha256) from a CMake build tree.
#
#   packaging/build-appimage.sh [--build-dir DIR] [--out DIR] [--no-build]
#
# Environment:
#   QMAKE            qmake of the Qt to bundle (default: qmake6, else qmake, from PATH)
#   NO_STRIP         1 = do not strip (default 1 on hosts other than Ubuntu/Debian: Arch libraries
#                    use DT_RELR, which linuxdeploy's bundled strip rejects - docs/research/qt-platform.md)
#   RL_GLIBC_FLOOR   the glibc the AppImage promises (default 2.35, the ubuntu:22.04 floor). The
#                    script measures the real floor (highest GLIBC_x.y any bundled ELF needs) and
#                    fails if it is above this - unless RL_PERSONAL_BUILD=1, which raises the
#                    promise to the measured value and says "personal test build" in AppRun.
#   RL_PERSONAL_BUILD  1 = a local test build for this machine only (Arch: floor = host glibc).
#   RL_PLUGIN_QT     tagged (default) | continuous - which pinned linuxdeploy-plugin-qt to use.
#   RL_SIZE_LIMIT_MB hard ceiling, default 350 (BUILD-SPEC <stack>).
#
# Steps: pinned tools (sha256-verified, cached in packaging/_tools, gitignored) -> cmake --install
# (component rasterloom) into AppDir -> linuxdeploy + plugin-qt (xcb, wayland, offscreen platform
# plugins; imageformats; iconengines/svg; driver and core system libraries excluded) -> AppRun
# with the glibc check -> THIRD-PARTY-NOTICES.md + WRITTEN-OFFER.txt from the populated AppDir ->
# uruntime (static, URUNTIME_EXTRACT=2) + appimagetool -> sha256 -> packaging/check-bundle.sh.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
PKG="$ROOT/packaging"
TOOLS="$PKG/_tools"
BUILD="$ROOT/build"
OUT="$PKG/_out"
DO_BUILD=1

while [ $# -gt 0 ]; do
    case "$1" in
        --build-dir) BUILD=$(cd "$2" && pwd); shift 2 ;;
        --out) mkdir -p "$2"; OUT=$(cd "$2" && pwd); shift 2 ;;
        --no-build) DO_BUILD=0; shift ;;
        -h|--help) sed -n '3,25p' "$0"; exit 0 ;;
        *) echo "build-appimage: unknown argument $1" >&2; exit 2 ;;
    esac
done

say() { printf '\n== %s\n' "$*"; }
die() { echo "build-appimage: $*" >&2; exit 1; }

# ---- pinned tools --------------------------------------------------------------------------------
# Tagged releases where they exist (immutable); linuxdeploy-plugin-qt's last tag is 2025-02-13, so
# its `continuous` build is available as an opt-in, pinned by the digest GitHub reported for it.
LINUXDEPLOY_URL=https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20251107-1/linuxdeploy-x86_64.AppImage
LINUXDEPLOY_SHA=c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d
PLUGIN_QT_TAGGED_URL=https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/1-alpha-20250213-1/linuxdeploy-plugin-qt-x86_64.AppImage
PLUGIN_QT_TAGGED_SHA=15106be885c1c48a021198e7e1e9a48ce9d02a86dd0a1848f00bdbf3c1c92724
PLUGIN_QT_CONT_URL=https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage
PLUGIN_QT_CONT_SHA=cfc1055b2b9dbc08412b579f20990b7b41a17b61beaa5847dc9477c96c9e9617   # build of 2026-08-22
URUNTIME_URL=https://github.com/VHSgunzo/uruntime/releases/download/v0.6.1/uruntime-appimage-squashfs-lite-x86_64
URUNTIME_SHA=e5463fa896fc583cab3b03e498fde5d311b6661bb6e184797081dfdb863bab82
APPIMAGETOOL_URL=https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage
APPIMAGETOOL_SHA=ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0

fetch() {  # fetch NAME URL SHA256 -> $TOOLS/NAME, verified
    local f="$TOOLS/$1"
    mkdir -p "$TOOLS"
    if [ ! -f "$f" ] || ! echo "$3  $f" | sha256sum -c --status; then
        echo "fetching $1"
        curl -fsSL --retry 3 -o "$f.part" "$2"
        echo "$3  $f.part" | sha256sum -c --status || { rm -f "$f.part"; die "sha256 mismatch for $1 ($2)"; }
        mv "$f.part" "$f"
    fi
    chmod +x "$f"
}

say "pinned tools"
fetch linuxdeploy-x86_64.AppImage "$LINUXDEPLOY_URL" "$LINUXDEPLOY_SHA"
case "${RL_PLUGIN_QT:-tagged}" in
    tagged) fetch plugin-qt-tagged.AppImage "$PLUGIN_QT_TAGGED_URL" "$PLUGIN_QT_TAGGED_SHA"; PLUGIN_QT=plugin-qt-tagged.AppImage ;;
    continuous) fetch plugin-qt-continuous.AppImage "$PLUGIN_QT_CONT_URL" "$PLUGIN_QT_CONT_SHA"; PLUGIN_QT=plugin-qt-continuous.AppImage ;;
    *) die "RL_PLUGIN_QT must be tagged or continuous" ;;
esac
fetch uruntime-appimage-squashfs-lite-x86_64 "$URUNTIME_URL" "$URUNTIME_SHA"
fetch appimagetool-x86_64.AppImage "$APPIMAGETOOL_URL" "$APPIMAGETOOL_SHA"
# linuxdeploy finds plugins on PATH by their canonical file name.
PLUGIN_DIR="$TOOLS/plugins"
mkdir -p "$PLUGIN_DIR"
ln -sf "../$PLUGIN_QT" "$PLUGIN_DIR/linuxdeploy-plugin-qt-x86_64.AppImage"
export PATH="$PLUGIN_DIR:$PATH"
# No FUSE needed anywhere (containers, CI): the tools extract themselves.
export APPIMAGE_EXTRACT_AND_RUN=1

# ---- Qt ------------------------------------------------------------------------------------------
QMAKE=${QMAKE:-$(command -v qmake6 || command -v qmake || true)}
[ -n "$QMAKE" ] && [ -x "$QMAKE" ] || die "no qmake found; set QMAKE to the bundled Qt's qmake"
export QMAKE
QT_VERSION=$("$QMAKE" -query QT_VERSION)
QT_PLUGINS=$("$QMAKE" -query QT_INSTALL_PLUGINS)
QT_LIBS=$("$QMAKE" -query QT_INSTALL_LIBS)
echo "Qt $QT_VERSION from $QMAKE (plugins: $QT_PLUGINS)"
# Qt 6.10 renamed platforms/libqwayland-{generic,egl}.so to one libqwayland.so; plugin-qt only
# deploys platform plugins named here, and fails loudly on a name that does not exist.
if [ -f "$QT_PLUGINS/platforms/libqwayland.so" ]; then
    WAYLAND_PLUGINS="libqwayland.so"
else
    WAYLAND_PLUGINS="libqwayland-generic.so;libqwayland-egl.so"
fi
for p in ${WAYLAND_PLUGINS//;/ } libqoffscreen.so libqxcb.so; do
    [ -f "$QT_PLUGINS/platforms/$p" ] || die "Qt has no platforms/$p (install qtwayland / qtbase plugins)"
done
[ -f "$QT_PLUGINS/iconengines/libqsvgicon.so" ] || die "Qt has no iconengines/libqsvgicon.so (qtsvg)"
# linuxdeploy-plugin-qt deploys every plugin in imageformats/; Open, drag-and-drop and paste need
# the qtimageformats readers (WebP, TGA, ...) on top of qtbase's GIF / ICO / JPEG.
for p in libqgif.so libqico.so libqwebp.so libqtga.so; do
    [ -f "$QT_PLUGINS/imageformats/$p" ] || die "Qt has no imageformats/$p (install qtimageformats: aqt -m qtimageformats, qt6-imageformats, qt6-image-formats-plugins)"
done
export EXTRA_PLATFORM_PLUGINS="$WAYLAND_PLUGINS;libqoffscreen.so"
export EXTRA_QT_MODULES="waylandcompositor;svg"
# Also let linuxdeploy resolve the Qt libraries of a non-system Qt (aqtinstall in CI).
export LD_LIBRARY_PATH="$QT_LIBS${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

if [ -z "${NO_STRIP:-}" ]; then
    OS_ID=$(sed -n 's/^ID=//p' /etc/os-release 2>/dev/null | tr -d '"')
    case "$OS_ID" in ubuntu|debian) NO_STRIP=0 ;; *) NO_STRIP=1 ;; esac
fi
if [ "$NO_STRIP" = 1 ]; then export NO_STRIP=1; else unset NO_STRIP; fi
echo "NO_STRIP=${NO_STRIP:-unset}"

# ---- build + install -----------------------------------------------------------------------------
if [ "$DO_BUILD" = 1 ]; then
    say "build ($BUILD)"
    if [ ! -f "$BUILD/CMakeCache.txt" ]; then
        cmake -G Ninja -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DQT_NO_PRIVATE_MODULE_WARNING=ON
    fi
    cmake --build "$BUILD"
fi
[ -x "$BUILD/src/rasterloom" ] && [ -x "$BUILD/src/rasterloom-cli" ] || die "no built binaries in $BUILD/src"

APPDIR="$OUT/AppDir"
say "install into $APPDIR"
rm -rf "$APPDIR"
mkdir -p "$OUT"
cmake --install "$BUILD" --prefix "$APPDIR/usr" --component rasterloom > "$OUT/install.log"
echo "installed $(grep -c Installing "$OUT/install.log") files"

# ---- linuxdeploy ---------------------------------------------------------------------------------
# BUILD-SPEC <verification>: never bundle driver or core session libraries. The AppImage
# excludelist compiled into linuxdeploy covers most; the rest are named here (glib is NOT on the
# upstream list and was measured being bundled; systemd/dbus per the research note).
EXCLUDES=(
    'libGL.so*' 'libEGL.so*' 'libGLdispatch.so*' 'libGLX.so*' 'libGLX_*.so*' 'libOpenGL.so*'
    'libGLESv2.so*' 'libdrm.so*' 'libgbm.so*' 'libvulkan.so*'
    'libX11.so*' 'libX11-xcb.so*' 'libxcb.so*'
    'libwayland-client.so*' 'libwayland-cursor.so*' 'libwayland-egl.so*'
    'libglib-2.0.so*' 'libgobject-2.0.so*' 'libgio-2.0.so*' 'libgmodule-2.0.so*' 'libgthread-2.0.so*'
    'libfontconfig.so*' 'libfreetype.so*'
    'libsystemd.so*' 'libdbus-1.so*'
)
EXCL_ARGS=()
for e in "${EXCLUDES[@]}"; do EXCL_ARGS+=(--exclude-library "$e"); done

say "linuxdeploy (plugin-qt: ${RL_PLUGIN_QT:-tagged}; platforms: xcb;$EXTRA_PLATFORM_PLUGINS)"
if ! "$TOOLS/linuxdeploy-x86_64.AppImage" --appdir "$APPDIR" \
        -e "$APPDIR/usr/bin/rasterloom" -e "$APPDIR/usr/bin/rasterloom-cli" \
        -d "$APPDIR/usr/share/applications/rasterloom.desktop" \
        -i "$APPDIR/usr/share/icons/hicolor/256x256/apps/rasterloom.png" \
        -i "$APPDIR/usr/share/icons/hicolor/scalable/apps/rasterloom.svg" \
        --plugin qt "${EXCL_ARGS[@]}" > "$OUT/linuxdeploy.log" 2>&1; then
    tail -40 "$OUT/linuxdeploy.log"
    die "linuxdeploy failed (full log: $OUT/linuxdeploy.log)"
fi
grep -iE 'warn|error' "$OUT/linuxdeploy.log" | sort -u | head -20 || true
[ -f "$APPDIR/usr/plugins/platforms/libqxcb.so" ] || die "linuxdeploy did not produce a Qt bundle (see output above)"
[ -f "$APPDIR/usr/plugins/imageformats/libqwebp.so" ] || die "linuxdeploy did not bundle imageformats/libqwebp.so"
# linuxdeploy honours --exclude-library for what it deploys itself, but linuxdeploy-plugin-qt
# deploys the Qt plugins' dependencies on its own and ignores the list (measured: glib, systemd,
# dbus and libwayland-{cursor,egl} still landed in usr/lib). Enforce the list here; every one of
# these is a host library the dynamic linker finds on the system.
for e in "${EXCLUDES[@]}"; do
    for f in "$APPDIR"/usr/lib/$e; do
        [ -e "$f" ] || continue
        echo "removing excluded host library ${f#"$APPDIR"/}"
        rm -f "$f"
    done
done
python3 "$PKG/elfdeps.py" prune "$APPDIR"
echo "host libraries the bundle expects the system to provide:"
python3 "$PKG/elfdeps.py" needed "$APPDIR" | tr '\n' ' '; echo

# ---- glibc floor + AppRun ------------------------------------------------------------------------
say "glibc floor"
# readelf exits non-zero on the non-ELF files (scripts) the find also picks up: ignore that.
MEASURED=$( { find "$APPDIR" -type f \( -name '*.so*' -o -perm -u+x \) -print0 \
    | xargs -0 -r readelf -V --wide 2>/dev/null || true; } \
    | { grep -oE 'Name: GLIBC_[0-9]+\.[0-9]+(\.[0-9]+)?' || true; } | sed 's/Name: GLIBC_//' | sort -V | tail -1)
[ -n "$MEASURED" ] || die "could not measure the glibc floor"
MEASURED=$(echo "$MEASURED" | cut -d. -f1-2)
PROMISE=${RL_GLIBC_FLOOR:-2.35}
echo "highest GLIBC symbol version needed by the bundle: $MEASURED (promised: $PROMISE)"
ver_gt() { [ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | tail -1)" = "$1" ] && [ "$1" != "$2" ]; }
if ver_gt "$MEASURED" "$PROMISE"; then
    if [ "${RL_PERSONAL_BUILD:-0}" = 1 ]; then
        PROMISE=$MEASURED
        SENTENCE="Requires glibc >= $PROMISE. This is a personal test build for the machine it was built on; the release AppImage requires glibc >= 2.35 (Ubuntu 22.04+, Debian 12+, Fedora 36+, RHEL 9+, current Arch and openSUSE)."
    else
        die "the bundle needs GLIBC_$MEASURED but promises $PROMISE (build on ubuntu:22.04, or set RL_PERSONAL_BUILD=1 for a local test build)"
    fi
else
    SENTENCE="Requires glibc >= $PROMISE (Ubuntu 22.04+, Debian 12+, Fedora 36+, RHEL 9+, current Arch and openSUSE)."
fi
rm -f "$APPDIR/AppRun" "$APPDIR/AppRun.wrapped"
python3 - "$PKG/AppRun.in" "$APPDIR/AppRun" "$PROMISE" "$SENTENCE" <<'PY'
import sys
src, dst, floor, sentence = sys.argv[1:]
assert "'" not in sentence
t = open(src).read().replace("@RL_GLIBC_FLOOR@", floor).replace("@RL_GLIBC_SENTENCE@", sentence)
assert "@RL_" not in t
open(dst, "w").write(t)
PY
chmod 755 "$APPDIR/AppRun"
sh -n "$APPDIR/AppRun"

# ---- release-hygiene files that depend on the bundle -----------------------------------------------
say "THIRD-PARTY-NOTICES.md + WRITTEN-OFFER.txt"
DOC="$APPDIR/usr/share/doc/rasterloom"
python3 "$PKG/bundle_notices.py" --appdir "$APPDIR" --qmake "$QMAKE" --build-dir "$BUILD" \
    --out "$DOC/THIRD-PARTY-NOTICES.md" --manifest-out "$OUT/bundle-manifest.yaml"
python3 "$PKG/written_offer.py" --qmake "$QMAKE" --build-dir "$BUILD" --glibc-floor "$PROMISE" \
    --out "$DOC/WRITTEN-OFFER.txt"
for f in LICENSE COPYING.LESSER THIRD-PARTY-NOTICES.md NOTICE AUTHORS WRITTEN-OFFER.txt; do
    [ -s "$DOC/$f" ] || die "missing $DOC/$f"
done

# ---- runtime + appimagetool ----------------------------------------------------------------------
say "uruntime (URUNTIME_EXTRACT=2) + appimagetool"
RUNTIME="$OUT/uruntime-extract2"
cp "$TOOLS/uruntime-appimage-squashfs-lite-x86_64" "$RUNTIME"
# One-digit in-place edit of the embedded config: FUSE first, else always extract (the default,
# 3, refuses to extract files over 350 MiB - exactly our size ceiling). docs/research/qt-platform.md
sed -i 's|URUNTIME_EXTRACT=[0-9]|URUNTIME_EXTRACT=2|' "$RUNTIME"
grep -aq 'URUNTIME_EXTRACT=2' "$RUNTIME" || die "could not set URUNTIME_EXTRACT=2 in the runtime"
cmp -s "$RUNTIME" "$TOOLS/uruntime-appimage-squashfs-lite-x86_64" && die "runtime unchanged by the URUNTIME_EXTRACT patch"
IMG="$OUT/Rasterloom-x86_64.AppImage"
rm -f "$IMG" "$IMG.sha256"
APP_VERSION=$(grep -m1 -oE 'VERSION [0-9]+\.[0-9]+\.[0-9]+' "$ROOT/CMakeLists.txt" | cut -d' ' -f2)
if ! ARCH=x86_64 VERSION="$APP_VERSION" "$TOOLS/appimagetool-x86_64.AppImage" --no-appstream \
        --runtime-file "$RUNTIME" "$APPDIR" "$IMG" > "$OUT/appimagetool.log" 2>&1; then
    tail -30 "$OUT/appimagetool.log"
    die "appimagetool failed (full log: $OUT/appimagetool.log)"
fi
[ -f "$IMG" ] || die "appimagetool produced no $IMG"
chmod 755 "$IMG"
(cd "$OUT" && sha256sum "$(basename "$IMG")" > "$(basename "$IMG").sha256")

SIZE=$(stat -c %s "$IMG")
LIMIT=$(( ${RL_SIZE_LIMIT_MB:-350} * 1000 * 1000 ))
awk -v f="$IMG" -v s="$SIZE" -v l="${RL_SIZE_LIMIT_MB:-350}" -v h="$(cut -d' ' -f1 "$IMG.sha256")" \
    'BEGIN { printf "AppImage: %s\n  size %d bytes (%.1f MB, %.1f MiB); limit %d MB\n  sha256 %s\n", f, s, s/1e6, s/1048576, l, h }'
[ "$SIZE" -le "$LIMIT" ] || die "AppImage is over the ${RL_SIZE_LIMIT_MB:-350} MB ceiling"

say "check-bundle"
"$PKG/check-bundle.sh" "$IMG"
