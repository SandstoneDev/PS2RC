#!/bin/bash
# run_host.sh - runs the host harness built by mk/build.sh host on the game pack's disc.
#
#   bash mk/run_host.sh [--pack NAME] [max_seconds] [verbose] [tag] [extra harness args, e.g. --mainloop-limit 200]
#
# Same translation, hooks, trap set and patches as the EBOOT, with the PSP memory layout (--psp-layout: RAM end =
# the pack's [memory] ram_end_psp, the code region not loaded and checked at the end) and the check-mode runtime
# header (plain accessors report hardware addresses). The disc folder (local.toml [games."<pack>"] disc, or the pack's
# [run] disc), the boot program ([id] boot_elf), the disc map and the disc-file overrides ([run]) come from the pack;
# BDIR = the build dir (default ~/ps2rc-build/<pack>_host), OUT = the output dir (default [run] out).
# Outputs in OUT: host_<tag>.log, alias_<tag>.txt (plain accessors that saw hardware: must be empty),
# mmio_<tag>.txt (functions that reached the register model), disc_<tag>.txt (disc files read).
SRC=$(cd "$(dirname "$0")/.." && pwd)
if [ "$1" = --pack ]; then PACK=$2; shift 2; fi
eval "$(python3 "$SRC/ps2rc/pack.py" shell ${PACK:+"$PACK"} --posix)"
B=${BDIR:-$HOME/ps2rc-build/${PACK_NAME}_host}
OUT=${OUT:-$PACK_RUN_OUT}
DISC="$PACK_DISC"
[ -n "$DISC" ] || { echo "no disc folder for pack $PACK_NAME (local.toml [games.\"$PACK_NAME\"] disc)"; exit 4; }
OVR=()
for o in $PACK_DISC_OVERRIDE; do OVR+=(--disc-override "$o"); done
MAXS=${1:-600}
VERB=${2:-0}
TAG=${3:-strict}
shift 3 2>/dev/null
mkdir -p "$OUT"
cd "$B"
exec 9>"$B/run.lock"
if ! flock -n 9; then echo "another host run is in progress in $B"; exit 9; fi
start=$(date +%s)
./ps2rc_host --elf "$DISC/$PACK_ELF" --disc "$DISC" --discmap "$PACK_DISCMAP" --log "$B/run_$TAG.log" \
    --max-seconds "$MAXS" --verbose "$VERB" --psp-layout "${OVR[@]}" \
    --alias-report "$OUT/alias_$TAG.txt" --mmio-profile "$OUT/mmio_$TAG.txt" --disc-usage "$OUT/disc_$TAG.txt" "$@"
code=$?
size=$(stat -c %s "$B/run_$TAG.log")
if [ "$size" -lt 20000000 ]; then
  cp "$B/run_$TAG.log" "$OUT/host_$TAG.log.tmp"
else
  { head -c 8000000 "$B/run_$TAG.log"; echo; echo "... [log truncated: $size bytes] ..."; tail -c 8000000 "$B/run_$TAG.log"; } > "$OUT/host_$TAG.log.tmp"
fi
rm -f "$OUT/host_$TAG.log"
mv "$OUT/host_$TAG.log.tmp" "$OUT/host_$TAG.log"
echo "exit code $code, wall $(( $(date +%s) - start )) s, log $size bytes"
grep -E "^MILESTONE|^FATAL|^ALIAS|hole check|limit reached|host stacks|alias check|maskbase check|^RMB_MISMATCH|^TTY" "$B/run_$TAG.log" | tail -60
