#!/bin/bash
# Build the PoC-1 PSP EBOOT in a Linux-native copy (~/ps2rc-build/poc1: no mtime skew on a Windows drive under WSL),
# always from clean, and stage it with its data files in build/poc1/psp_stage/PS2RC_POC1/.
# Proof of a real build = the psp-gcc -c lines printed below.
set -e
export PSPDEV=${PSPDEV:-$HOME/pspdev}
export PATH=$PSPDEV/bin:$PATH
SRC=$(cd "$(dirname "$0")/../../.." && pwd)
# overridable: B = build dir, STAGE = staging dir
B=${B:-$HOME/ps2rc-build/poc1}
STAGE=${STAGE:-$SRC/build/poc1/psp_stage/PS2RC_POC1}
rm -rf "$B"
mkdir -p "$B" "$STAGE"
cp "$SRC/tests/poc1/psp/main.c" "$SRC/tests/poc1/psp/Makefile" "$SRC/runtime/r5900_rt.h" \
   "$SRC/build/poc1/rc/poc1_rc.c" "$SRC/build/poc1/rc/poc1_table.h" "$B/"
cd "$B"
sed -i 's#^ROOT .*#ROOT     = .#; s#^INCDIR .*#INCDIR   = .#; s#^vpath .*##' Makefile
make clean >/dev/null 2>&1 || true
make 2>&1 | tee make.log | grep -E "psp-gcc .* -c|error|warning|stubs out of order" || true
if grep -q "stubs out of order" make.log; then echo "BUILD ERROR: stubs out of order"; exit 1; fi
test -f EBOOT.PBP
cp EBOOT.PBP "$STAGE/"
cp "$SRC/build/poc1/vectors.bin" "$STAGE/vectors.bin"
cp "$SRC/build/poc1/pcsx2_interp.bin" "$STAGE/expected.bin"
md5sum EBOOT.PBP
ls -la "$STAGE"
