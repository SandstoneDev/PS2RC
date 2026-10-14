#!/bin/bash
# Build the VU0 oracle guest ELF with the pspdev binutils in R5900 mode (run inside WSL).
# Input: build/poc_vu0/vu0_tests.S, vu0_data.S, vectors.bin (tests/poc_vu0/gen_vu0.py). Output: vu0.elf, vu0.syms.
# usage: build.sh [out dir, default build/poc_vu0]
set -e
export PATH=$HOME/pspdev/bin:$PATH
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$HERE/../../../build/poc_vu0}
cd "$OUT"
AS="psp-as -march=r5900 -EL -mabi=eabi -mgp64"
$AS -o start.o "$HERE/start.S"
$AS -o vu0_tests.o vu0_tests.S
$AS -o vu0_data.o vu0_data.S
psp-ld -nostdlib -T "$HERE/vu0.ld" -o vu0.elf start.o vu0_tests.o vu0_data.o
psp-nm -n vu0.elf > vu0.syms
psp-readelf -h -l vu0.elf | grep -E "Type|Machine|Flags|Entry|LOAD"
