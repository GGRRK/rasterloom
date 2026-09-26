#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# test-artifact.sh - run the shipped AppImage itself (never the build tree): BUILD-SPEC
# <verification> "Test the shipped artifact". Used locally and by every CI distro container.
#
#   packaging/test-artifact.sh Rasterloom-x86_64.AppImage SCRIPT.json EXPECTED.png [OUTDIR]
#
#   1. --version --verbose       exit 0, names the version, glibc, Qt and the platform choice
#   2. --cli --render-script     renders SCRIPT.json; the PNG must be byte-identical to EXPECTED.png
#                                (rendered by the build tree's rasterloom-cli, whose output the
#                                goldens prove byte-equal to the NumPy reference), or, where the
#                                host zlib deflates differently, pixel-identical to it
#   3. --selftest --junit-xml    the embedded self-test over the bundled scripts, exit 0
#   4. --third-party-notices / --license   exit 0
#   5. GUI start: the editor window on $DISPLAY via xcb when a display exists (CI: Xvfb), else on
#      the offscreen platform - never a visible window on a desktop. It must still be running
#      after 8 s and must not print Qt's "could not load the Qt platform plugin". xcb is used
#      only when RL_TEST_XCB=1 and DISPLAY is set (CI under xvfb-run).
# Runs with APPIMAGE_EXTRACT_AND_RUN=1 (containers have no FUSE). Exit 1 on any failure.
set -uo pipefail

IMG=${1:?usage: test-artifact.sh APPIMAGE SCRIPT.json EXPECTED.png [OUTDIR]}
SCRIPT=${2:?script}
EXPECT=${3:?expected png}
OUT=${4:-$(mktemp -d)}
mkdir -p "$OUT"
IMG=$(cd "$(dirname "$IMG")" && pwd)/$(basename "$IMG")
chmod +x "$IMG" 2>/dev/null || true
export APPIMAGE_EXTRACT_AND_RUN=1
unset QT_QPA_PLATFORMTHEME
VERSION=$(grep -m1 -oE '^rasterloom [0-9]+\.[0-9]+\.[0-9]+' <<<"$("$IMG" --version 2>/dev/null)" | cut -d' ' -f2)

FAILS=0
ok()   { printf '  ok    %s\n' "$*"; }
fail() { printf '  FAIL  %s\n' "$*"; FAILS=$((FAILS + 1)); }

echo "test-artifact: $IMG"
echo "  host: $(sed -n 's/^PRETTY_NAME=//p' /etc/os-release 2>/dev/null | tr -d '"'), glibc $(getconf GNU_LIBC_VERSION 2>/dev/null | cut -d' ' -f2)"

# 1. version
if "$IMG" --version --verbose > "$OUT/version.txt" 2>&1; then
    sed 's/^/        /' "$OUT/version.txt"
    if grep -qE '^rasterloom [0-9]+\.[0-9]+\.[0-9]+ \(Qt 6\.' "$OUT/version.txt" \
        && grep -qE '^  glibc +[0-9]' "$OUT/version.txt" && grep -qE '^  platform ' "$OUT/version.txt"; then
        ok "--version --verbose ($VERSION)"
    else
        fail "--version --verbose output is missing the version, glibc or platform line"
    fi
else
    sed 's/^/        /' "$OUT/version.txt"
    fail "--version --verbose exited non-zero"
fi

# 2. byte-exact render through the bundled CLI
rm -f "$OUT/render.png"
if "$IMG" --cli --render-script "$SCRIPT" --out "$OUT/render.png" > "$OUT/render.log" 2>&1; then
    if cmp -s "$OUT/render.png" "$EXPECT"; then
        ok "render $(basename "$SCRIPT"): byte-identical to $(basename "$EXPECT") ($(stat -c %s "$EXPECT") bytes)"
    else
        # zlib is a host library (not bundled), and zlib-ng (Fedora) deflates the same pixels to
        # different bytes. Re-encode both files through the artifact's own PNG path on this host:
        # the decode -> single-layer composite -> encode round trip is exact for straight RGBA8
        # (only fully transparent pixels are canonicalised, which every render already is), so the
        # re-encoded files are byte-identical exactly when the pixels are.
        rm -f "$OUT/render.norm.png" "$OUT/expected.norm.png"
        if "$IMG" --cli --render-file "$OUT/render.png" --out "$OUT/render.norm.png" >> "$OUT/render.log" 2>&1 &&
           "$IMG" --cli --render-file "$EXPECT" --out "$OUT/expected.norm.png" >> "$OUT/render.log" 2>&1 &&
           cmp -s "$OUT/render.norm.png" "$OUT/expected.norm.png"; then
            ok "render $(basename "$SCRIPT"): pixel-identical to $(basename "$EXPECT") (PNG bytes differ: host zlib $(sed -n 's/^ *zlib *//p' "$OUT/version.txt"))"
        else
            fail "render $(basename "$SCRIPT") differs from $(basename "$EXPECT")"
        fi
    fi
else
    sed 's/^/        /' "$OUT/render.log"
    fail "--cli --render-script exited non-zero"
fi

# 3. embedded self-test
if "$IMG" --selftest --junit-xml="$OUT/selftest-junit.xml" > "$OUT/selftest.txt" 2>&1; then
    ok "--selftest: $(tail -1 "$OUT/selftest.txt")"
else
    sed 's/^/        /' "$OUT/selftest.txt"
    fail "--selftest failed"
fi

# 4. licence texts
if "$IMG" --third-party-notices > "$OUT/notices.txt" 2>&1 && "$IMG" --license > "$OUT/license.txt" 2>&1; then
    ok "--license / --third-party-notices ($(wc -l < "$OUT/license.txt") / $(wc -l < "$OUT/notices.txt") lines)"
else
    fail "--license or --third-party-notices exited non-zero"
fi

# 5. GUI start (xcb on a display, else offscreen; never a visible window on a desktop session)
if [ -n "${DISPLAY:-}" ] && [ -n "${RL_TEST_XCB:-}" ]; then PLAT=xcb; else PLAT=offscreen; fi
# Own session/process group, so the runtime AND the editor it starts are stopped together.
RASTERLOOM_PLATFORM=$PLAT setsid "$IMG" > "$OUT/gui.log" 2>&1 &
GPID=$!
alive=1
for _ in 1 2 3 4 5 6 7 8; do
    sleep 1
    kill -0 "$GPID" 2>/dev/null || { alive=0; break; }
done
if [ "$alive" = 1 ]; then
    kill -TERM -- "-$GPID" 2>/dev/null; sleep 2; kill -KILL -- "-$GPID" 2>/dev/null
    wait "$GPID" 2>/dev/null
else
    wait "$GPID"; rc=$?
fi
if [ "$alive" = 1 ] && ! grep -qiE 'could not (load|find) the qt platform plugin|cannot load library|error while loading shared libraries' "$OUT/gui.log"; then
    ok "GUI ran for 8 s on the $PLAT platform"
else
    sed 's/^/        /' "$OUT/gui.log" | tail -20
    fail "GUI on $PLAT: exited (rc ${rc:-?}) within 8 s, or a plugin/library load error was printed"
fi

if [ "$FAILS" -ne 0 ]; then
    echo "test-artifact: $FAILS check(s) FAILED"
    exit 1
fi
echo "test-artifact: all checks passed"
