#!/bin/bash
# ps2rc_exc.prx (kernel half of the crash handler + the hardware-counter thread) -> <work dir>/psp_exc/ps2rc_exc.prx.
# Linux (or WSL) with pspdev. BDIR=<build dir> / OUT=<output dir> select others (experimental packages).
#   bash psp_exc/build.sh
set -e
export PSPDEV=${PSPDEV:-$HOME/pspdev}
export PATH=$PSPDEV/bin:$PATH
SRC=$(cd "$(dirname "$0")/.." && pwd)
B=${BDIR:-$HOME/ps2rc-build/psp_exc}
OUT=${OUT:-$(python3 "$SRC/ps2rc/config.py" get work)/psp_exc}
OUT_NAME=${OUT_NAME:-ps2rc_exc.prx}           # another output name (a candidate module next to the canonical one)
rm -rf "$B"
mkdir -p "$B" "$OUT"
P="$SRC/psp_exc"
cp "$P/exc_main.c" "$P/exc_me.c" "$P/exc_asm.S" "$P/ps2rc_exc.h" "$P/ps2rc_sctrl.S" "$P/ps2rc_sctrl.h" "$P/exports.exp" "$P/Makefile" "$B/"
cd "$B"
make 2>&1 | tee make.log | grep -E "error|warning" || true
test -f ps2rc_exc.prx
cp ps2rc_exc.prx "$OUT/$OUT_NAME"
psp-objdump -d ps2rc_exc.elf | grep -c "eret" | awk '{print "eret instructions:", $1}'
psp-readelf -s ps2rc_exc.elf 2>/dev/null | grep -E "ps2rc_exc_handler|module_start" || true
psp-readelf -s ps2rc_exc.elf 2>/dev/null | grep -E " module_stop$" || true
md5sum "$OUT/$OUT_NAME"
ls -la "$OUT/$OUT_NAME"
