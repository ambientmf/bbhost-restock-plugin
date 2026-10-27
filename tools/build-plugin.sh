#!/bin/sh
# Builds plugin/restock.c into a Windows DLL with clang + mingw-w64.
#   tools/build-plugin.sh [BBHOST_SRC] [OUT_DIR]
# BBHOST_SRC: a bbhost checkout (for include/bbhost_plugin.h); or set $BBHOST_SRC
# OUT_DIR:    where the DLL goes, default build/
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
bbhost=${1:-${BBHOST_SRC:?usage: tools/build-plugin.sh BBHOST_SRC [OUT_DIR] (or set BBHOST_SRC)}}
outdir=${2:-$here/build}
mkdir -p "$outdir"
prlimit --as=2000000000 clang --target=x86_64-w64-mingw32 -O2 -Wall -Wextra -shared \
    -I"$bbhost/include" -o "$outdir/restock.dll" "$here/plugin/restock.c"
echo "built $outdir/restock.dll"
