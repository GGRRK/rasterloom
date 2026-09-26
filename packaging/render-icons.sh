#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Re-render the committed PNG icons from share/icons/rasterloom.svg (the only source of truth).
# Uses rsvg-convert (librsvg). The PNGs carry no metadata chunks (check with
#   `python3 -c "from PIL import Image; print(Image.open(F).info)"`).
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
svg="$here/share/icons/rasterloom.svg"
command -v rsvg-convert >/dev/null || { echo "render-icons: rsvg-convert (librsvg) is required" >&2; exit 2; }
for s in 16 24 32 48 64 128 256 512; do
    mkdir -p "$here/share/icons/hicolor/${s}x${s}/apps"
    rsvg-convert -w "$s" -h "$s" "$svg" -o "$here/share/icons/hicolor/${s}x${s}/apps/rasterloom.png"
done
echo "render-icons: wrote 8 sizes under share/icons/hicolor/"
