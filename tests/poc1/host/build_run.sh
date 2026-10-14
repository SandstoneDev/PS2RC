#!/bin/bash
# Build and run the PoC-1 host harness (WSL, gcc).  Output: build/poc1/host_results.bin
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$HERE/../../..
OUT=$ROOT/build/poc1
gcc -O2 -std=c11 -DRMB_CHECK -Wall -Wno-unused-label -Wno-unused-variable -frounding-math -ffp-contract=off -fno-fast-math \
    -I"$ROOT/runtime" -I"$OUT/rc" "$HERE/main.c" "$OUT/rc/poc1_rc.c" -lm -o "$OUT/poc1_host"
"$OUT/poc1_host" "$OUT/vectors.bin" "$OUT/host_results.bin"
