#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# check-bundle.sh - hard assertions on a built Rasterloom AppImage (BUILD-SPEC <verification> as
# corrected by the 2026-09-26 addendum). Any failure exits 1; the CI package job runs this.
#
#   packaging/check-bundle.sh Rasterloom-x86_64.AppImage
#
# Listing: `unsquashfs -o "$(X --appimage-offset)" -l X` when squashfs-tools is installed (the
# squashfs image starts after the runtime, so a plain `unsquashfs -l X` cannot work); otherwise
# `X --appimage-extract` into a temporary directory (the runtime needs neither FUSE nor tools).
set -euo pipefail

IMG=${1:?usage: check-bundle.sh Rasterloom-x86_64.AppImage}
[ -f "$IMG" ] || { echo "check-bundle: no such file $IMG" >&2; exit 2; }
IMG=$(cd "$(dirname "$IMG")" && pwd)/$(basename "$IMG")
chmod +x "$IMG" 2>/dev/null || true

FAILS=0
ok()   { printf '  ok    %s\n' "$*"; }
fail() { printf '  FAIL  %s\n' "$*"; FAILS=$((FAILS + 1)); }

OFFSET=$("$IMG" --appimage-offset)
[[ "$OFFSET" =~ ^[0-9]+$ ]] || { echo "check-bundle: --appimage-offset printed '$OFFSET'" >&2; exit 1; }
echo "check-bundle: $IMG (squashfs at offset $OFFSET)"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
if command -v unsquashfs >/dev/null; then
    unsquashfs -o "$OFFSET" -l "$IMG" | sed -n 's|^squashfs-root/||p' > "$TMP/list"
    LIST_HOW="unsquashfs -o $OFFSET -l"
else
    (cd "$TMP" && "$IMG" --appimage-extract >/dev/null)
    (cd "$TMP/squashfs-root" && find . -mindepth 1 | sed 's|^\./||') > "$TMP/list"
    LIST_HOW="--appimage-extract"
fi
N=$(wc -l < "$TMP/list")
echo "  ($N entries via $LIST_HOW)"
[ "$N" -gt 50 ] || { echo "check-bundle: implausibly small listing" >&2; exit 1; }

has() { grep -qE "$1" "$TMP/list"; }
need() { if has "$1"; then ok "$2"; else fail "$2 (no match for /$1/)"; fi; }
forbid() {
    local hits
    mapfile -t hits < <(grep -E "$1" "$TMP/list" || true)
    if [ "${#hits[@]}" -eq 0 ]; then ok "$2"; else fail "$2:"; printf '          %s\n' "${hits[@]}"; fi
}

echo "platform plugins"
need '^usr/plugins/platforms/libqxcb\.so$' "xcb platform plugin (the default, D6)"
# Qt <= 6.9 ships libqwayland-generic.so, Qt >= 6.10 libqwayland.so (addendum).
need '^usr/plugins/platforms/libqwayland(-generic)?\.so$' "Wayland platform plugin - not an X11-only bundle"
need '^usr/plugins/wayland-shell-integration/libxdg-shell\.so$' "Wayland xdg-shell integration"
need '^usr/plugins/platforms/libqoffscreen\.so$' "offscreen platform plugin (headless self-test)"
need '^usr/plugins/xcbglintegrations/libqxcb-(glx|egl)-integration\.so$' "xcb GL integration"
echo "image and icon plugins"
need '^usr/plugins/iconengines/libqsvgicon\.so$' "iconengines/libqsvgicon.so (else blank icons, no error)"
need '^usr/plugins/imageformats/libqsvg\.so$' "imageformats/libqsvg.so"
# Open / drag-and-drop / paste of formats the core has no codec for (gui/image_import.cpp).
need '^usr/plugins/imageformats/libqgif\.so$' "imageformats/libqgif.so (GIF, qtbase)"
need '^usr/plugins/imageformats/libqico\.so$' "imageformats/libqico.so (ICO/CUR, qtbase)"
need '^usr/plugins/imageformats/libqwebp\.so$' "imageformats/libqwebp.so (WebP, qtimageformats)"
need '^usr/plugins/imageformats/libqtga\.so$' "imageformats/libqtga.so (TGA, qtimageformats)"
need '^usr/lib/libQt6Svg\.so' "libQt6Svg"
echo "no driver or core session libraries"
forbid '(^|/)lib(GL|EGL|GLdispatch|GLX|GLX_[^/]*|OpenGL|GLESv[12][^/]*|drm|gbm|vulkan|nvidia[^/]*|cuda)\.so' "no GL/EGL/drm/gbm/vulkan/driver libraries"
forbid '(^|/)lib(X11|X11-xcb|xcb|wayland-client|wayland-cursor|wayland-egl|fontconfig|freetype)\.so' "no X11/xcb/wayland-client/fontconfig/freetype"
forbid '(^|/)lib(glib|gobject|gio|gmodule|gthread)-2\.0\.so' "no glib"
forbid '(^|/)lib(systemd|dbus-1|c|m|dl|pthread|rt|stdc\+\+|gcc_s)\.so' "no systemd/dbus/libc/libstdc++/libgcc_s"
forbid '(^|/)ld-linux[^/]*\.so' "no dynamic loader"
echo "application"
need '^usr/bin/rasterloom$' "usr/bin/rasterloom"
need '^usr/bin/rasterloom-cli$' "usr/bin/rasterloom-cli"
need '^usr/bin/qt\.conf$' "usr/bin/qt.conf"
need '^usr/lib/libqtadvanceddocking-qt6\.so' "shared ADS library"
need '^usr/share/rasterloom/selftest/buildspec_example\.json$' "self-test scripts"
need '^AppRun$' "AppRun"
need '^rasterloom\.desktop$' "desktop file at the AppDir root"
need '^rasterloom\.(png|svg)$' "icon at the AppDir root"
need '^usr/share/metainfo/io\.github\.ggrrk\.rasterloom\.metainfo\.xml$' "AppStream metainfo"
echo "release hygiene (usr/share/doc/rasterloom, read by Help > About > Licenses)"
for f in LICENSE COPYING.LESSER THIRD-PARTY-NOTICES.md NOTICE AUTHORS WRITTEN-OFFER.txt; do
    need "^usr/share/doc/rasterloom/${f//./\\.}$" "$f"
done

echo "runtime"
head -c "$OFFSET" "$IMG" > "$TMP/runtime"   # (a file, not a pipe: grep -q would SIGPIPE head)
if grep -aq 'URUNTIME_EXTRACT=2' "$TMP/runtime"; then
    ok "uruntime embedded config URUNTIME_EXTRACT=2"
else
    fail "runtime is not uruntime with URUNTIME_EXTRACT=2"
fi
if grep -aqi 'libfuse\.so\.2' "$TMP/runtime"; then
    fail "runtime references libfuse.so.2 (not the static uruntime)"
else
    ok "static runtime (no libfuse2 dependency)"
fi
SIZE=$(stat -c %s "$IMG")
if [ "$SIZE" -le 350000000 ]; then ok "size $SIZE bytes <= 350 MB"; else fail "size $SIZE bytes > 350 MB"; fi
if [ -f "$IMG.sha256" ]; then
    if (cd "$(dirname "$IMG")" && sha256sum -c --status "$(basename "$IMG").sha256"); then
        ok "sha256 file matches"
    else
        fail "sha256 file does not match the AppImage"
    fi
else
    fail "no $(basename "$IMG").sha256 beside the AppImage"
fi

if [ "$FAILS" -ne 0 ]; then
    echo "check-bundle: $FAILS assertion(s) FAILED"
    exit 1
fi
echo "check-bundle: all assertions passed"
