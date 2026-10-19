#!/bin/bash
# pack_target_files.sh - the target side of a Linux pspdev install as one tar, for build_windows.sh: the files that
# run on the PSP or describe it (newlib, pthread glue, libgcc and crt files, pspsdk headers/libraries/link files) and
# do not depend on the host the compiler runs on. Third-party psp-packages (SDL, Lua, ...) installed through psp-pacman
# are left out, except the ones named with --keep (default: none - the runtime imports the CFW functions it needs
# through its own stubs, so psp-cfw-sdk's GPL-3.0 libraries are not shipped).
#
#   bash tools/toolchain/pack_target_files.sh PSPDEV_DIR OUT.tar [--keep pkg1,pkg2] [--gcc-version X.Y.Z]
set -euo pipefail
SRC=${1:?pspdev install dir}
OUT=${2:?output tar}
shift 2
KEEP=""
VER=""
while [ $# -gt 0 ]; do
  case "$1" in
    --keep) KEEP=$2; shift 2 ;;
    --gcc-version) VER=$2; shift 2 ;;
    *) echo "unknown argument $1" >&2; exit 2 ;;
  esac
done
SRC=$(cd "$SRC" && pwd)
[ -n "$VER" ] || VER=$(ls "$SRC/lib/gcc/psp" | sort -V | tail -1)
[ -d "$SRC/lib/gcc/psp/$VER" ] || { echo "no $SRC/lib/gcc/psp/$VER" >&2; exit 2; }
LIST=$(mktemp)
EXCL=$(mktemp)
# files owned by psp-pacman packages (paths relative to the install root) minus the kept packages
if [ -d "$SRC/var/lib/pacman/local" ]; then
  for d in "$SRC"/var/lib/pacman/local/*/; do
    name=$(sed -n '/^%NAME%/{n;p;}' "$d/desc" 2>/dev/null || true)
    case ",$KEEP," in *",$name,"*) continue ;; esac
    [ -f "$d/files" ] && sed -n '/^%FILES%/,/^$/p' "$d/files" | grep -v '^%' | grep -v '/$' | grep -v '^$' >> "$EXCL" || true
  done
fi
sort -u -o "$EXCL" "$EXCL"
# lib/gcc/psp/<ver>/plugin and install-tools are for the host the compiler runs on (Linux .so, scripts): not taken
(cd "$SRC" && find psp/include psp/lib psp/sdk "lib/gcc/psp/$VER" \( -path "lib/gcc/psp/$VER/plugin" -o     -path "lib/gcc/psp/$VER/install-tools" \) -prune -o \( -type f -o -type l \) -print) | sort > "$LIST.all"
comm -23 "$LIST.all" "$EXCL" > "$LIST"
# nothing for the host in there (pkgconfig/cmake helpers are kept: small, harmless)
tar -C "$SRC" -h -cf "$OUT" -T "$LIST"          # -h: links stored as files (Windows has none)
echo "target files: $(wc -l < "$LIST") of $(wc -l < "$LIST.all") files (psp-packages left out: $(wc -l < "$EXCL") files," \
     "kept: $KEEP), gcc $VER -> $OUT ($(du -h "$OUT" | cut -f1))"
rm -f "$LIST" "$LIST.all" "$EXCL"
