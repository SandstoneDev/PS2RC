#!/usr/bin/env bash
# Build the PCSX2-oracle smoke-test ELF with the pspdev binutils inside WSL Debian.
#
#   usage: build.sh [OUT_DIR]          (default: <repo>/build/oracle/smoke)
#   from Windows (Git Bash): MSYS_NO_PATHCONV=1 wsl.exe -d Debian - bash <the WSL path of this file>
#
# Outputs in OUT_DIR: smoke.o, smoke.elf, smoke.dis (disassembly), smoke.nm and
# smoke.json (symbol addresses for the host side; written last = build complete).
set -euo pipefail
export PATH="$HOME/pspdev/bin:$PATH"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
OUT="${1:-$REPO/build/oracle/smoke}"
mkdir -p "$OUT"
rm -f "$OUT/smoke.json"

psp-as -march=r5900 -EL -mabi=eabi -mgp64 -o "$OUT/smoke.o" "$HERE/smoke.S"
psp-ld -m elf32elmip -T "$HERE/smoke.ld" -z max-page-size=0x1000 --no-warn-rwx-segments \
       -o "$OUT/smoke.elf" "$OUT/smoke.o"
psp-objdump -d -m mips:5900 "$OUT/smoke.elf" > "$OUT/smoke.dis"
psp-nm -n -S "$OUT/smoke.elf" > "$OUT/smoke.nm"

ENTRY="$(psp-readelf -h "$OUT/smoke.elf" | awk '/Entry point address/ {print $4}')"

# psp-nm -S lines: "addr size type name" (sized) or "addr type name" (unsized).
awk -v entry="$ENTRY" '
BEGIN { printf "{\n  \"elf\": \"smoke.elf\",\n  \"entry\": \"%s\",\n  \"symbols\": {\n", entry; n = 0 }
NF == 4 { addr = $1; size = $2; type = $3; name = $4 }
NF == 3 { addr = $1; size = "0";  type = $2; name = $3 }
NF == 3 || NF == 4 {
    if (n++) printf ",\n"
    printf "    \"%s\": {\"addr\": \"0x%s\", \"size\": \"0x%s\", \"type\": \"%s\"}", name, addr, size, type
}
END { printf "\n  }\n}\n" }' "$OUT/smoke.nm" > "$OUT/smoke.json.tmp"
mv "$OUT/smoke.json.tmp" "$OUT/smoke.json"

echo "built $OUT/smoke.elf (entry $ENTRY)"
