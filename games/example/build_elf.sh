#!/bin/bash
# Builds the example "disc": the example program assembled with the pspdev binutils in R5900 mode, plus its disc map.
#   bash games/example/build_elf.sh            (Linux / WSL with pspdev; output in the work dir: <work>/example/disc)
# Writes <disc>/EXAMPLE.ELF, <work>/example/discmap.txt ("lsn size ISO_PATH relative/path", one file) and checks that
# the addresses games/example/hooks.def gives match the program's symbols.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
export PATH=${PSPDEV:-$HOME/pspdev}/bin:$PATH
WORK=$(python3 "$ROOT/ps2rc/config.py" get work)
DISC=$WORK/example/disc
mkdir -p "$DISC" "$WORK/example/obj"
O=$WORK/example/obj
psp-as -march=r5900 -EL -mabi=eabi -mgp64 -o "$O/example.o" "$HERE/src/example.S"
psp-ld -nostdlib -T "$HERE/src/example.ld" -o "$DISC/EXAMPLE.ELF" "$O/example.o"
psp-nm -n "$DISC/EXAMPLE.ELF" > "$O/example.syms"
size=$(stat -c %s "$DISC/EXAMPLE.ELF")
printf '24 %s EXAMPLE.ELF EXAMPLE.ELF\n' "$size" > "$WORK/example/discmap.txt"
# hooks.def must name the program's addresses
python3 - "$HERE/hooks.def" "$O/example.syms" <<'EOF'
import re, sys
syms = {}
for ln in open(sys.argv[2]):
    p = ln.split()
    if len(p) == 3:
        syms[p[2]] = int(p[0], 16)
want = {"write": "write", "DelayThread": "DelayThread", "Exit": "Exit", "example_frame": "frame"}
text = re.sub(r"/\*.*?\*/", "", open(sys.argv[1]).read(), flags=re.S)
bad = []
for kind, addr, name in re.findall(r"^\s*(HOOK|TRACE)\(\s*([0-9a-f]{8})\s*,\s*(\w+)", text, re.M):
    sym = want.get(name)
    if sym is None or syms.get(sym) != int(addr, 16):
        bad.append("%s %s at %s (program: %s)" % (kind, name, addr, "%08x" % syms[sym] if sym in syms else "?"))
if bad:
    sys.exit("hooks.def does not match the program: " + "; ".join(bad))
print("example: hooks.def matches the program's symbols")
EOF
echo "example: $DISC/EXAMPLE.ELF ($size bytes), $WORK/example/discmap.txt"
