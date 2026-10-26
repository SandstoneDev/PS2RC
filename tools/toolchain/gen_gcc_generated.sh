#!/bin/bash
# gen_gcc_generated.sh - the files a GCC git/GitHub source archive lacks and a release tarball carries, made with
# flex on Linux / WSL, as a tar for build_windows.sh (--gcc-generated): today gcc/gengtype-lex.cc (MSYS2 has no
# flex). The build needs no other generator for the C compiler (MAKEINFO=true skips the manuals).
#
#   bash tools/toolchain/gen_gcc_generated.sh GCC_SRC.tar[.gz] OUT.tar
set -euo pipefail
SRC_TAR=$(realpath "${1:?gcc source archive}")
OUT=$(realpath -m "${2:?output tar}")
command -v flex >/dev/null || { echo "needs flex" >&2; exit 2; }
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
TOP=$(tar -tf "$SRC_TAR" | sed -n 1p | cut -d/ -f1)      # sed reads to the end: no SIGPIPE under pipefail
tar -C "$T" -xf "$SRC_TAR" "$TOP/gcc/gengtype-lex.l"
mkdir -p "$T/out/$TOP/gcc"
flex -o "$T/lex.cc" "$T/$TOP/gcc/gengtype-lex.l"
# gcc/Makefile.in's own rule puts bconfig.h first
{ echo '#include "bconfig.h"'; cat "$T/lex.cc"; } > "$T/out/$TOP/gcc/gengtype-lex.cc"
tar -C "$T/out" --owner=0 --group=0 --numeric-owner -cf "$OUT" "$TOP"   # no user names in the archive
echo "gcc generated files -> $OUT ($(tar -tf "$OUT" | grep -c -v '/$') files)"
