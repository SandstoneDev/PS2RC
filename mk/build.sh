#!/bin/bash
# build.sh - builds a translated PS2 program with the HLE runtime: the PSP EBOOT or the host (Linux x86-64) harness.
#
#   bash mk/build.sh [--pack NAME] [psp|host] [JOBS] [clean]
#
# The game pack (games/<NAME>/pack.toml; --pack, else PACK=, else local.toml [defaults] pack) gives every input:
#   [build] translation   the translated C (ps2rc/pipeline/translate.py output, in the work dir)
#           hooks         the hook list (hooks.def): prepare_gen renames the hooked functions, bind.py maps the names
#           sources       the pack's C module dirs (each may have host/ and psp/ subdirs), audio_sources (AUDIO=1 only)
#           patch_dir, trapset, textpatch, callflush, zones, sites, natives, layout, defines, y_hdr, y_gen, psp_ldflags,
#           pkg_out (where EBOOT.PBP goes)
# Environment (overrides the pack):  TRANS, BDIR (build dir; default ~/ps2rc-build/<pack>_<target>), PKG_OUT,
#   ALIAS (call|inline), LAYOUT (a layout dir or "none"), Y_HDR / Y_GEN / Y_NATIVE / ZONES (empty = off),
#   HLE_BUILD (build number in the log header and the menu title), SPLASH (1 / 0 / test; PSP), AUDIO (1: the pack's
#   sound-driver model and the SPU2 model are compiled in), PROFILE (a pack build profile, e.g. public) + GEN_DIR (its
#   generated modules) + PROFILE_KEEP (file names the profile excludes but a developer test build keeps, e.g. the
#   in-run A/B tester).
# Steps: 1. prepare_gen (hooks, loop safe points, trap set, text patches) + layout_apply (cold functions);
#   2. the HLE sources: hle/core + the platform (hle/host or hle/psp + psp_exc's user part) + the pack's modules, plus
#   the generated hle_bind.h (hook names) and hle_pack.h (the pack's values); 3. the runtime header for the target
#   (patch_rt_header: masked accessors, trapping accessors, the back-end's levers); 4. make (mk/Makefile.host or
#   mk/Makefile.psp). Toolchain: pspdev (psp-gcc) for the PSP, gcc for the host. "stubs out of order" from
#   psp-fixup-imports counts as a build error.
set -e
export PSPDEV=${PSPDEV:-$HOME/pspdev}
export PATH=$PSPDEV/bin:$PATH
SRC=$(cd "$(dirname "$0")/.." && pwd)
J=6
TARGET=psp
CLEAN=0
while [ $# -gt 0 ]; do
  case "$1" in
    --pack) PACK=$2; shift ;;
    clean) CLEAN=1 ;;
    host|psp) TARGET=$1 ;;
    [0-9]*) J=$1 ;;
  esac
  shift
done
eval "$(python3 "$SRC/ps2rc/pack.py" shell ${PACK:+"$PACK"} --posix)"
TRANS=${TRANS:-$PACK_TRANS}
ALIAS=${ALIAS:-${PACK_ALIAS:-call}}
LAYOUT=${LAYOUT:-$PACK_LAYOUT}
Y_HDR=${Y_HDR-$PACK_Y_HDR}
Y_GEN=${Y_GEN-$PACK_Y_GEN}
Y_NATIVE=${Y_NATIVE-$PACK_NATIVES}
if [ "$TARGET" = host ]; then B=${BDIR:-$HOME/ps2rc-build/${PACK_NAME}_host}; else B=${BDIR:-$HOME/ps2rc-build/${PACK_NAME}_psp}; fi
PKG_OUT=${PKG_OUT:-$PACK_PKG_OUT}
[ -n "$TRANS" ] && [ -f "$TRANS/stats.json" ] || { echo "no translation at '$TRANS' (pack $PACK_NAME [build] translation)"; exit 1; }
[ "$CLEAN" = 1 ] && rm -rf "$B"
mkdir -p "$B/gen" "$B/gen_prep" "$B/hle/rt" "$B/obj"
# PROFILE=<name>: the pack's build profile [profile.<name>] as the converter applies it (e.g. "public": nothing
#   hand-translated from the game's code): hooks / zones without its exclude_hooks, its natives / defines, its source
#   exclusions and extra sources; its generated modules come from GEN_DIR (a converter work dir's modules/)
if [ -n "$PROFILE" ]; then
  eval "$(python3 "$SRC/ps2rc/pack.py" profile "${PACK:--}" "$PROFILE" "$B" ${GEN_DIR:+"$GEN_DIR"} --posix)"
  PACK_HOOKS=$PROF_HOOKS
  [ -n "$PROF_ZONES" ] && PACK_ZONES=$PROF_ZONES
  [ "$PROF_NATIVES_OFF" = 1 ] && Y_NATIVE=""
  PACK_DEFINES="$PACK_DEFINES $PROF_DEFINES"
  echo "profile: $PROFILE (excluded: $PROF_EXCLUDE)"
fi
start=$(date +%s)
MASKBASE=""
grep -q '"variant": "v5s4' "$TRANS/stats.json" && MASKBASE="--maskbase"       # back-end v5s4(y*): rmb_ accessors
[ "$LAYOUT" = none ] && LAYOUT=""
# 1. generated units: hooks, loop back-edge safe points, trap set (rtx_/rmx_/rmbx_ accessors), text patches, MFC0
#    result reloads, register flushes before calls that read a register as a hidden argument
CALLFLUSH=""; for cf in $PACK_CALLFLUSH; do CALLFLUSH="$CALLFLUSH --callflush $cf"; done   # pack [build] callflush
python3 "$SRC/ps2rc/pipeline/prepare_gen.py" "$TRANS" "$PACK_HOOKS" "$B/gen_prep" ${PACK_PATCH_DIR:+"$PACK_PATCH_DIR"} \
    ${PACK_TRAPSET:+--trapset "$PACK_TRAPSET"} ${PACK_TEXTPATCH:+--textpatch "$PACK_TEXTPATCH"} --fix-mfc0 \
    $CALLFLUSH --zones "${ZONES:-$PACK_ZONES}" ${Y_NATIVE:+--native "$Y_NATIVE"} ${PACK_SITES:+--sites "$PACK_SITES"} \
    | tee "$B/prepare.log"
#    the code layout's cold list (compiler hints only; gen/ is rewritten only where the text changes)
python3 "$SRC/ps2rc/pipeline/layout_apply.py" "$B/gen_prep" "$B/gen" ${LAYOUT:+--cold "$LAYOUT/cold.txt"} \
    | tee -a "$B/prepare.log"
# 2. HLE runtime: the core, the platform layer, the pack's modules (a dir's host/ or psp/ subdir for this target only)
HLE_BUILD=${HLE_BUILD:-21}
HLE_DEFS="-DHLE_BUILD_NUM=$HLE_BUILD"
[ -n "$Y_NATIVE" ] && HLE_DEFS="$HLE_DEFS -DHLE_YNATIVE"          # the pack's native fast paths are compiled in
for d in $PACK_DEFINES; do HLE_DEFS="$HLE_DEFS -D$d"; done       # pack [build] defines
# the credit splash before the boot (hle/psp/hle_splash.c, PSP only): SPLASH=1 compiles it in (-DHLE_SPLASH),
#   SPLASH=0 = no splash code; default 1 for the default build dir, 0 for every BDIR build; SPLASH=test = the
#   automatic-press check variant (emulator checks only, never a package)
if [ -z "${SPLASH+x}" ]; then if [ -z "$BDIR" ]; then SPLASH=1; else SPLASH=0; fi; fi
if [ "$TARGET" = psp ]; then
  case "$SPLASH" in
    1) HLE_DEFS="$HLE_DEFS -DHLE_SPLASH" ;;
    test) HLE_DEFS="$HLE_DEFS -DHLE_SPLASH -DHLE_SPLASH_AUTOPRESS"; echo "SPLASH=test: AUTO-PRESS TEST BUILD - never a package" ;;
  esac
  echo "splash: SPLASH=$SPLASH"
fi
copy_dir() {                                           # *.c *.h *.S of a dir and of its <target>/ subdir
  for f in "$1"/*.c "$1"/*.h "$1"/*.S "$1/$TARGET"/*.c "$1/$TARGET"/*.h "$1/$TARGET"/*.S; do
    [ -f "$f" ] && cp -pu "$f" "$B/hle/"
  done
  return 0
}
copy_dir "$SRC/hle/core"
for d in $PACK_SOURCES; do copy_dir "$d"; done
cp -pu "$SRC/runtime/r5900_rt.c" "$B/hle/"
cp -pu "$PACK_HOOKS" "$B/hle/hooks.def"                 # the pack's hook list (hle_traces.c includes it)
if [ "$TARGET" = psp ]; then
  copy_dir "$SRC/hle/psp"
  # the in-game settings menu = Dear ImGui + lib/psp_imgui (C++, partial-linked with --gc-sections into one
  #   object by Makefile.psp) + hle/psp/hle_menu.cpp; MENU=0 leaves all of it out (the pad / flip hooks are weak)
  rm -f "$B"/hle/*.cpp "$B"/hle/imgui*.h "$B"/hle/imstb*.h "$B"/hle/pimgui*.h "$B"/hle/psp_imgui.h
  if [ "${MENU:-1}" != 0 ]; then
    for f in "$SRC"/hle/psp/*.cpp "$SRC"/lib/psp_imgui/*.cpp "$SRC"/lib/psp_imgui/*.h "$SRC"/lib/psp_imgui/third_party/imgui/imgui*.cpp              "$SRC"/lib/psp_imgui/third_party/imgui/*.h; do cp -p "$f" "$B/hle/"; done
  else
    rm -f "$B/hle/hle_menu_psp.c"
  fi
  echo "menu: MENU=${MENU:-1}"
  cp -pu "$SRC/psp_exc/exc_user.c" "$SRC/psp_exc/ps2rc_exc.h" "$B/hle/"   # the user side of the kernel PRX
  rm -f "$B/hle/rmb_check.c"
else
  cp -pu "$SRC/hle/host/hle_host.c" "$SRC/hle/host/hle_mcfs_host.c" "$B/hle/"
  # a maskbase (v5s4) host build counts base+offset accesses that would leave the window (0 expected)
  if [ -n "$MASKBASE" ]; then cp -pu "$SRC/hle/host/rmb_check.c" "$B/hle/"; else rm -f "$B/hle/rmb_check.c"; fi
fi
# AUDIO=1: the pack's sound-driver model (audio_sources) with the SPU2 model (audio/) in lockstep with virtual time
AUDIO=${AUDIO:-0}
if [ "$AUDIO" = 1 ] && [ -n "$PACK_AUDIO_SOURCES" ]; then
  copy_dir "$SRC/audio"
  for d in $PACK_AUDIO_SOURCES; do copy_dir "$d"; done
  HLE_DEFS="$HLE_DEFS -DHLE_HAVE_AUDIO_MODEL"
fi
if [ -n "$PROFILE" ]; then                             # the profile's source set (see above)
  for f in $PROF_EXCLUDE; do                          # PROFILE_KEEP: developer test aids kept anyway (A/B sets)
    case " $PROFILE_KEEP " in *" $f "*) continue ;; esac
    rm -f "$B/hle/$f"
  done
  add_src() { if [ -d "$1" ]; then copy_dir "$1"; else cp -pu "$1" "$B/hle/"; fi; }
  add_inc() {                                          # include path only (#included by a glue unit)
    if [ -d "$1" ]; then
      for f in "$1"/*.c "$1"/*.h "$1/$TARGET"/*.c "$1/$TARGET"/*.h; do [ -f "$f" ] && cp -pu "$f" "$B/hle/rt/"; done
    else cp -pu "$1" "$B/hle/rt/"; fi
    return 0
  }
  for x in $PROF_EXTRA; do add_src "$x"; done
  for x in $PROF_INCLUDE; do add_inc "$x"; done
  if [ "$AUDIO" = 1 ]; then
    for x in $PROF_AUDIO_EXTRA; do add_src "$x"; done
    for x in $PROF_AUDIO_INCLUDE; do add_inc "$x"; done
  fi
  for g in $PROF_GEN; do                               # path:define:include_only:audio
    IFS=: read -r gpath gdef ginc gaud <<< "$g"
    [ "$gaud" = 1 ] && [ "$AUDIO" != 1 ] && continue
    if [ "$ginc" = 1 ]; then cp -pu "$gpath" "$B/hle/rt/"; else cp -pu "$gpath" "$B/hle/"; fi
    HLE_DEFS="$HLE_DEFS -D$gdef"
  done
fi
TRANS_NAME=$(python3 -c "
import json,sys; s=json.load(open('$TRANS/stats.json'))
print('%s,%s,exempt=%d' % (s.get('variant'), (s.get('function_set') or 'v1').split('/')[-1], len((s.get('exempt') or {}).get('main', []))))")
TRANS_NAME="$TRANS_NAME,alias=$ALIAS,layout=$([ -n "$LAYOUT" ] && basename "$LAYOUT" || echo none)"   # no spaces
[ -n "$Y_HDR" ] && TRANS_NAME="$TRANS_NAME,y=$(echo "$Y_HDR" | tr ',' '+')"                   # header levers
HLE_DEFS="$HLE_DEFS -DHLE_TRANSLATION=\\\"$TRANS_NAME\\\""
echo "translation: $TRANS ($TRANS_NAME)"
# the hook bindings (the HLE sources name their hooks; the pack's hooks.def gives the addresses) and the pack's values
python3 "$SRC/ps2rc/bind.py" "$PACK_HOOKS" "$B/hle/hle_bind.h" "$B/hle"
python3 "$SRC/ps2rc/pack.py" cheader ${PACK:+"$PACK"} "$B/hle/hle_pack.h"
# 3. runtime header: original + target delta (psp: masked accessors + rtx_/rmx_; host: the same with alias checks)
if [ "$TARGET" = psp ]; then MODE=psp; else MODE=check; fi
YH="$Y_HDR"; [ "$TARGET" = host ] && YH=$(echo "$Y_HDR" | tr ',' '\n' | grep -v '^ca2\?$' | paste -sd, -)
python3 "$SRC/ps2rc/pipeline/patch_rt_header.py" "$SRC/runtime/r5900_rt.h" "$B/hle/rt/r5900_rt.h" --mode $MODE \
    --alias "$ALIAS" $MASKBASE ${YH:+--y "$YH"}
cd "$B"
if [ "$TARGET" = host ]; then
  GEN_OPT="-O1"
  [ -n "$MASKBASE" ] && GEN_OPT="$GEN_OPT -DRMB_CHECK"
  echo "$GEN_OPT" > flags.new                              # objects of other flags are rebuilt
  cmp -s flags.new flags.txt || rm -rf obj/gen ps2rc_host
  mv flags.new flags.txt
  make -f "$SRC/mk/Makefile.host" -j"$J" ps2rc_host GEN_OPT="$GEN_OPT" CFLAGS_HLE_EXTRA="$HLE_DEFS" 2>&1 \
      | grep -v "^make" | tail -40
  test "${PIPESTATUS[0]}" = 0
  echo "build wall time: $(( $(date +%s) - start )) s"
  ls -la "$B/ps2rc_host" | awk '{print "binary:", $5, "bytes"}'
  exit 0
fi
# 4. PSP: all generated units in parallel, then HLE + link + EBOOT
#    a code layout (-ffunction-sections, order.ld: the main-loop functions first in .text) when the pack has one
GEN_EXTRA=""; HLE_EXTRA=""; LAYOUT_ORDER=""
rm -f order.ld
if [ -n "$LAYOUT" ] && [ -f "$LAYOUT/order.ld" ]; then
  cp "$LAYOUT/order.ld" order.ld
  # the native fast paths (HLE, -ffunction-sections) first in .text, next to the hot generated code
  [ -n "$Y_NATIVE" ] && sed -i 's/^\.text : {$/.text : {\n  *(.text.y_nat_*)/' order.ld
  GEN_EXTRA="-ffunction-sections"; HLE_EXTRA="-ffunction-sections"; LAYOUT_ORDER="order.ld"
fi
[ -n "$Y_GEN" ] && GEN_EXTRA="$GEN_EXTRA $Y_GEN"          # extra compiler flags for the generated units
echo "$GEN_EXTRA|$HLE_EXTRA" > flags.new                  # objects of other flags are rebuilt
cmp -s flags.new flags.txt || rm -rf obj/gen obj/hle
mv flags.new flags.txt
[ -f splash.txt ] || [ "$SPLASH" != 0 ] || echo "splash=0" > splash.txt   # dirs built before = no splash
echo "splash=$SPLASH" > splash.new                       # a SPLASH change rebuilds the HLE objects (not obj/gen)
cmp -s splash.new splash.txt || rm -rf obj/hle
mv splash.new splash.txt
make -f "$SRC/mk/Makefile.psp" -j"$J" gen_objs GEN_EXTRA="$GEN_EXTRA" HLE_EXTRA="$HLE_EXTRA" \
    LAYOUT_ORDER="$LAYOUT_ORDER" > make_gen.log 2>&1 || { grep -E "error|Error" make_gen.log | head -20; exit 1; }
echo "generated units compiled: $(grep -c 'psp-gcc -c' make_gen.log) in $(( $(date +%s) - start )) s"
rm -f PARAM.SFO                                        # build.mak never remakes it (a stale menu title otherwise)
rm -f ps2rc.elf                                         # relink: order.ld is not a make dependency
make -f "$SRC/mk/Makefile.psp" HLE_DEFS="$HLE_DEFS" HLE_BUILD="$HLE_BUILD" PSP_TITLE="$PACK_TITLE" PACK_LDFLAGS="$PACK_PSP_LDFLAGS" GEN_EXTRA="$GEN_EXTRA" \
    HLE_EXTRA="$HLE_EXTRA" LAYOUT_ORDER="$LAYOUT_ORDER" 2>&1 | tee make.log | grep -E "error|warning|stubs out of order" | head -40 || true
if grep -q "stubs out of order" make.log; then echo "BUILD ERROR: stubs out of order"; exit 1; fi
test -f EBOOT.PBP
psp-size -t obj/gen/*.o | tail -1 | awk '{print "generated code: text", $1, "data", $2, "bss", $3}'
psp-size -t obj/hle/*.o | tail -1 | awk '{print "HLE runtime: text", $1, "data", $2, "bss", $3}'
psp-size ps2rc.elf
ls -la EBOOT.PBP
mkdir -p "$PKG_OUT"
cp EBOOT.PBP "$PKG_OUT/EBOOT.PBP"
cp ps2rc.elf "$PKG_OUT/ps2rc.elf"                        # symbols: maps a crash EPC to its function
psp-size -A ps2rc.elf > "$PKG_OUT/ps2rc_size.txt"
echo "build wall time: $(( $(date +%s) - start )) s (EBOOT.PBP -> $PKG_OUT)"
