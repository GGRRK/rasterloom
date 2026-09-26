#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# BUILD-SPEC req 1: librasterloomcore.a contains no Qt at all.
#   1. nm: no symbol (defined or undefined) that names a Qt class/namespace or qt_* function.
#   2. source: no file under src/core includes a Qt header.
# usage: check_core_no_qt.sh path/to/librasterloomcore.a path/to/src/core
set -eu

lib=${1:?usage: check_core_no_qt.sh LIB.a SRC_CORE_DIR}
src=${2:?usage: check_core_no_qt.sh LIB.a SRC_CORE_DIR}
fail=0

[ -f "$lib" ] || { echo "FAIL: library not found: $lib"; exit 1; }
[ -d "$src" ] || { echo "FAIL: source dir not found: $src"; exit 1; }

nsyms=$(nm -C "$lib" 2>/dev/null | grep -c . || true)
if [ "$nsyms" -eq 0 ]; then
    echo "FAIL: nm produced no symbols for $lib"
    exit 1
fi

# Qt symbols: QString/QImage/QObject/... (Q followed by an uppercase letter and a lowercase letter),
# the Qt namespace (Qt::), qt_* helpers, and mangled Qt names (e.g. 7QString, 6QImage).
qt_syms=$(nm -C "$lib" 2>/dev/null \
    | grep -E '(^|[^A-Za-z0-9_])(Q[A-Z][a-z][A-Za-z0-9_]*|Qt::|qt_[A-Za-z0-9_]+)' || true)
qt_mangled=$(nm "$lib" 2>/dev/null | grep -E '[0-9]Q[A-Z][a-z][A-Za-z0-9_]*' || true)
if [ -n "$qt_syms$qt_mangled" ]; then
    echo "FAIL: Qt symbols in $lib:"
    printf '%s\n%s\n' "$qt_syms" "$qt_mangled" | sed '/^$/d' | head -20
    fail=1
fi

qt_inc=$(grep -rnE '^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"](Q[A-Za-z]+|Qt[A-Za-z]*/)' "$src" || true)
if [ -n "$qt_inc" ]; then
    echo "FAIL: Qt includes under $src:"
    echo "$qt_inc" | head -20
    fail=1
fi

if [ "$fail" -eq 0 ]; then
    echo "OK: $nsyms symbols checked in $(basename "$lib"), no Qt symbols; no Qt includes under $src"
fi
exit "$fail"
