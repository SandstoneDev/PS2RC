#!/bin/bash
# gen_binutils_parsers.sh - the bison/flex outputs of a binutils source archive, as a tar for build_windows.sh
# (--binutils-generated): a git/GitHub source archive has only the .y/.l files, and MSYS2 builds then need bison and
# flex. Runs on Linux / WSL with bison + flex (configure + only the parser targets, about a minute).
#
#   bash tools/toolchain/gen_binutils_parsers.sh BINUTILS_SRC.tar[.gz] OUT.tar
set -euo pipefail
SRC_TAR=$(realpath "${1:?binutils source archive}")
OUT=$(realpath -m "${2:?output tar}")
command -v bison >/dev/null && command -v flex >/dev/null || { echo "needs bison and flex" >&2; exit 2; }
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
tar -C "$T" -xf "$SRC_TAR"
S=$(echo "$T"/*)
TOP=$(basename "$S")
mkdir "$T/b" && cd "$T/b"
"$S/configure" --target=psp --disable-nls --disable-werror --disable-gdb --disable-sim --disable-gdbserver \
    --disable-gprofng > configure.log 2>&1
make configure-binutils configure-gas configure-ld > sub.log 2>&1
make -C binutils -k arparse.c arlex.c defparse.c deflex.c sysinfo.c syslex.c rcparse.c rclex.c mcparse.c mclex.c \
    >> sub.log 2>&1 || true
make -C gas -k itbl-parse.c itbl-lex.c >> sub.log 2>&1 || true
make -C ld -k ldgram.c ldlex.c deffilep.c >> sub.log 2>&1 || true
mkdir -p "$T/out/$TOP"
n=0
for f in binutils/arparse.[ch] binutils/arlex.c binutils/defparse.[ch] binutils/deflex.c binutils/sysinfo.[ch] \
         binutils/syslex.c binutils/rcparse.[ch] binutils/rclex.c binutils/mcparse.[ch] binutils/mclex.c \
         gas/itbl-parse.[ch] gas/itbl-lex.c ld/ldgram.[ch] ld/ldlex.c ld/deffilep.[ch]; do
  [ -f "$f" ] || continue
  mkdir -p "$T/out/$TOP/$(dirname "$f")"
  cp "$f" "$T/out/$TOP/$f"
  n=$((n + 1))
done
tar -C "$T/out" -cf "$OUT" "$TOP"
echo "$n generated files -> $OUT"
