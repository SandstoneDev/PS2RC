#!/bin/bash
# Build the VU0 oracle PSP EBOOT (back-end v4 translation of build/poc_vu0/vu0.elf) in a Linux-native copy
# (~/ps2rc-build/poc_vu0_psp), always from clean, and stage it with its data files in
# build/poc_vu0/psp_stage/PS2RC_VU0/ (EBOOT.PBP, vectors.bin, expected.bin). Run it on PPSSPP or the PSP: the report
# vu0_psp_report.txt next to the EBOOT must say RESULT PASS.
# Inputs (made on Windows first): python tests/poc_vu0/vu0_build_rc.py v4
#                                 python tests/poc_vu0/vu0_compare.py --expected build/poc_vu0/expected_psp.bin
set -e
export PSPDEV=${PSPDEV:-$HOME/pspdev}
export PATH=$PSPDEV/bin:$PATH
SRC=$(cd "$(dirname "$0")/../../.." && pwd)
B=${B:-$HOME/ps2rc-build/poc_vu0_psp}
STAGE=$SRC/build/poc_vu0/psp_stage/PS2RC_VU0
rm -rf "$B"
mkdir -p "$B" "$STAGE"
cp "$SRC/tests/poc_vu0/psp/main.c" "$SRC/tests/poc_vu0/psp/Makefile" "$SRC/runtime/r5900_rt.h" \
   "$SRC/runtime/r5900_rt.c" "$SRC/build/poc_vu0/rc/v4/vu0_rc.c" "$SRC/build/poc_vu0/rc/v4/vu0_table.h" "$B/"
cd "$B"
make 2>&1 | tee make.log | grep -E "psp-gcc .* -c|error|warning|stubs out of order" || true
if grep -q "stubs out of order" make.log; then echo "BUILD ERROR: stubs out of order"; exit 1; fi
test -f EBOOT.PBP
cp EBOOT.PBP "$STAGE/"
cp "$SRC/build/poc_vu0/vectors.bin" "$STAGE/vectors.bin"
cp "$SRC/build/poc_vu0/expected_psp.bin" "$STAGE/expected.bin"
psp-size vu0_rc.o
md5sum EBOOT.PBP
ls -la "$STAGE"
