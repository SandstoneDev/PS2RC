#!/bin/bash
# Build and run the VU0 oracle host harness (WSL, gcc).
# usage: build_run.sh <rc dir (vu0_rc.c, vu0_table.h)> <results.bin> [runtime dir, default src/runtime]
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$HERE/../../..
RC=$1
RES=$2
OUT=$(dirname "$RES")                                      # the oracle set's dir (vectors.bin)
RT=${3:-$ROOT/runtime}
gcc -O2 -std=gnu11 -Wall -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-function \
    -frounding-math -ffp-contract=off -fno-fast-math -I"$RT" -I"$RC" "$HERE/main.c" "$RC/vu0_rc.c" "$RT/r5900_rt.c" \
    -lm -o "$RC/vu0_host"
"$RC/vu0_host" "$OUT/vectors.bin" "$RES"
