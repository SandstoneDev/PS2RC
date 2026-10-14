#!/bin/bash
# Builds tests/memcard/mc_unit.c with the memory card HLE (src/hle/hle_memcard.c + hle_mcfs_host.c) and runs one mode
# on a fresh card folder: mc_unit.sh <mode> <work dir>   (called by tests/memcard/test_memcard.py through WSL)
set -e
SRC=$(cd "$(dirname "$0")/../.." && pwd)
MODE=$1
W=$2
mkdir -p "$W"
BIN="$W/mc_unit"
if [ ! -x "$BIN" ] || [ "$SRC/hle/core/hle_memcard.c" -nt "$BIN" ] || [ "$SRC/hle/host/hle_mcfs_host.c" -nt "$BIN" ] || \
   [ "$SRC/tests/memcard/mc_unit.c" -nt "$BIN" ]; then
  gcc -O1 -g -std=gnu11 -Wall -Wno-unused-function -Wno-format-truncation -I"$SRC/hle/core" -I"$SRC/runtime" \
      "$SRC/tests/memcard/mc_unit.c" "$SRC/hle/core/hle_memcard.c" "$SRC/hle/host/hle_mcfs_host.c" -o "$BIN"
fi
rm -rf "$W/card_$MODE"
"$BIN" "$MODE" "$W/card_$MODE"
