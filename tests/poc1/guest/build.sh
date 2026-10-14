#!/bin/bash
# Build the PoC-1 PS2 guest ELF with the pspdev binutils in R5900 mode (run inside WSL).
set -e
export PATH=$HOME/pspdev/bin:$PATH
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$HERE/../../../build/poc1
cd "$OUT"
AS="psp-as -march=r5900 -EL -mabi=eabi -mgp64"
$AS -o start.o "$HERE/start.S"
$AS -o poc1_tests.o poc1_tests.S
$AS -o poc1_data.o poc1_data.S
psp-ld -nostdlib -T "$HERE/poc1.ld" -o poc1.elf start.o poc1_tests.o poc1_data.o
psp-nm -n poc1.elf > poc1.syms
psp-objdump -d -m mips:5900 -M no-aliases poc1.elf > poc1.dis
psp-readelf -h -l poc1.elf | grep -E "Type|Machine|Flags|Entry|LOAD"
