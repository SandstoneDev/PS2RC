#!/bin/bash
# build_windows.sh - a native Windows (MinGW-w64, UCRT) PSP C toolchain for the converter bundle: psp binutils and
# the psp-gcc C compiler built as plain Windows programs (statically linked: no MSYS2 or MinGW DLL at run time), the
# target files (newlib, libgcc, pspsdk) copied from a Linux build of the SAME source commits (they do not depend on
# the host), and the pspsdk host tools the build needs (psp-fixup-imports, psp-prxgen, psp-build-exports).
#
# Run in an MSYS2 UCRT64 shell (msys2.org). Packages: make tar xz git
#   mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-gmp mingw-w64-ucrt-x86_64-mpfr mingw-w64-ucrt-x86_64-mpc
#   mingw-w64-ucrt-x86_64-isl mingw-w64-ucrt-x86_64-zlib
# Bison, flex and texinfo are NOT needed when the binutils source carries its generated parsers (release tarballs do;
# a git checkout needs them, see --binutils-generated).
#
#   bash tools/toolchain/build_windows.sh --binutils SRC.tar[.gz|.xz] --gcc SRC.tar[.gz|.xz] \
#        --target-files TARGET.tar --pspsdk-tools TOOLS.tar --prefix OUT_DIR [--work DIR] [--jobs N] \
#        [--binutils-generated GEN.tar] [--gcc-patches DIR]
#
#   --binutils / --gcc      source archives (one top-level directory each), e.g. the pinned commits fetched by
#                           tools/toolchain/fetch_sources.py
#   --target-files          tar of the target side of a Linux pspdev install of the same commits:
#                           psp/ (newlib, pthread, pspsdk) and lib/gcc/psp/<version>/ (libgcc, crt files)
#                           (tools/toolchain/pack_target_files.sh makes it)
#   --pspsdk-tools          tar holding tools/*.c, tools/*.h of the pspsdk commit
#   --prefix                the toolchain directory to create (bin/, libexec/, lib/, psp/); must not exist
# Output: the toolchain in --prefix plus BUILDINFO.txt (sources, configure lines, host compiler) and
# licenses/ (texts copied from the sources).
set -Eeuo pipefail

JOBS=$(nproc 2>/dev/null || echo 4)
WORK=""
BINUTILS="" GCC="" TARGET_FILES="" PSPSDK_TOOLS="" PREFIX="" BINUTILS_GEN="" GCC_PATCHES="" GCC_GEN="" RESUME=0
while [ $# -gt 0 ]; do
  case "$1" in
    --binutils) BINUTILS=$2; shift 2 ;;
    --binutils-generated) BINUTILS_GEN=$2; shift 2 ;;
    --gcc) GCC=$2; shift 2 ;;
    --gcc-patches) GCC_PATCHES=$2; shift 2 ;;
    --gcc-generated) GCC_GEN=$2; shift 2 ;;            # gen_gcc_generated.sh output (a git source has no lexer)
    --target-files) TARGET_FILES=$2; shift 2 ;;
    --pspsdk-tools) PSPSDK_TOOLS=$2; shift 2 ;;
    --prefix) PREFIX=$2; shift 2 ;;
    --work) WORK=$2; shift 2 ;;
    --jobs) JOBS=$2; shift 2 ;;
    --resume) RESUME=1; shift ;;                       # a prefix of an interrupted run: target files kept
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done
for v in BINUTILS GCC TARGET_FILES PSPSDK_TOOLS PREFIX; do
  [ -n "${!v}" ] || { echo "missing --$(echo $v | tr 'A-Z_' 'a-z-')" >&2; exit 2; }
done
# callers pass relative paths (tc/foo.tar); from here on the script cd's into its build dirs,
# so resolve every input path once now - otherwise the pspsdk-tools tar fails after the gcc cd
for v in BINUTILS GCC TARGET_FILES PSPSDK_TOOLS BINUTILS_GEN GCC_GEN GCC_PATCHES; do
  if [ -n "${!v}" ]; then eval "$v=\$(realpath \"\${!v}\")"; fi
done
case "${MSYSTEM:-}" in
  UCRT64|MINGW64) ;;
  *) echo "run this in an MSYS2 UCRT64 (or MINGW64) shell (MSYSTEM=${MSYSTEM:-unset})" >&2; exit 2 ;;
esac
[ -e "$PREFIX" ] && [ "$RESUME" = 0 ] && { echo "--prefix $PREFIX exists; remove it first (or --resume)" >&2; exit 2; }
SYSROOT_DEPS=$(dirname "$(command -v gcc)")/..          # /ucrt64: gmp, mpfr, mpc, isl (static libraries)
WORK=${WORK:-$(dirname "$PREFIX")/toolchain_work}
mkdir -p "$WORK" "$PREFIX"
PREFIX=$(cd "$PREFIX" && pwd)
PREFIX_W=$(cygpath -m "$PREFIX")                        # the Windows form of the path: what the programs store
LOG="$WORK/build.log"
: > "$LOG"
t0=$(date +%s)
say() { echo "[$(( $(date +%s) - t0 )) s] $*" | tee -a "$LOG"; }

# Every configure/make writes to $LOG (CI output would be flooded otherwise); on the first failed command show
# what failed and the log tail here - otherwise a red CI run says only "exit code 2" and nothing else.
on_err() {
  local rc=$?
  echo "" >&2
  echo "build_windows.sh: FAILED (exit $rc)" >&2
  echo "  command: ${BASH_COMMAND}" >&2
  echo "  at line ${BASH_LINENO[0]} of $(basename "${BASH_SOURCE[1]:-$0}")" >&2
  echo "---- last 60 lines of $LOG ----" >&2
  tail -n 60 "$LOG" >&2 || true
  echo "---- end of log ----" >&2
  exit "$rc"
}
trap on_err ERR

unpack() {   # archive dest [tar --exclude options] (one top-level directory expected)
  mkdir -p "$2"
  local a=$1 d=$2
  shift 2
  tar -C "$d" -xf "$a" "$@"
}
# parts of the source trees a C-only cross compiler never builds: test suites and other languages' front ends and
# run-time libraries (unpacking onto NTFS costs about 25 ms per file; the GCC tree has ~130,000)
B_SKIP=(--no-wildcards-match-slash --exclude='binutils-gdb-*/gdb' --exclude='binutils-gdb-*/sim' --exclude='binutils-gdb-*/gdbserver'
        --exclude='binutils-gdb-*/gprofng' --exclude='binutils-gdb-*/gold' --exclude='*/gas/testsuite'
        --exclude='*/ld/testsuite')       # binutils/testsuite/gentestdlls.c is built on Windows hosts
# (binutils: top-level directories only - * stops at /, binutils needs include/gdb; libiberty, libbacktrace, libsframe
# configure their tests)
G_SKIP=(--exclude='*/gcc/testsuite' --exclude='*/libgo' --exclude='*/libphobos' --exclude='*/libada'
        --exclude='*/gnattools' --exclude='*/libgrust' --exclude='*/libgm2' --exclude='*/libgfortran'
        --exclude='*/libgcobol' --exclude='*/libsanitizer' --exclude='*/libstdc++-v3' --exclude='*/libga68')
# (gcc/<language> front-end directories all stay: gcc numbers its tree codes over every front end found in the source
# tree (all-tree.def), and the LTO stream carries those numbers - a tree without gcc/ada, gcc/m2 ... reads pspdev's
# LTO objects as "LTO_tags out of range")

# unpacking ~150,000 files onto NTFS takes long (measured: 1 h on a busy disk; ~3 files/s on a busy USB HDD; 40 s on
# an SSD - put --work on an SSD):
# a finished unpack of the same archives is kept (stamp = the archives' sizes and times + the skip lists)
STAMP="$(stat -c '%s %Y' "$BINUTILS" "$GCC" ${BINUTILS_GEN:+"$BINUTILS_GEN"} ${GCC_GEN:+"$GCC_GEN"} | tr '\n' ' ')${GCC_PATCHES:+patches} $(echo "${B_SKIP[*]} ${G_SKIP[*]}" | md5sum | cut -c1-8)"
if [ -f "$WORK/src/.unpacked" ] && [ "$(cat "$WORK/src/.unpacked")" = "$STAMP" ]; then
  say "sources: kept from the last run"
else
  say "sources"
  rm -rf "$WORK/src"
  unpack "$BINUTILS" "$WORK/src/b" "${B_SKIP[@]}"
  B_DIR=$(echo "$WORK"/src/b/*)
  if [ -n "$BINUTILS_GEN" ]; then
    unpack "$BINUTILS_GEN" "$WORK/src/gen"
    cp -r "$WORK"/src/gen/*/. "$B_DIR/"
    # generated parsers newer than their .y/.l sources: make never asks for bison/flex
    find "$B_DIR" \( -name '*parse.c' -o -name '*parse.h' -o -name '*lex.c' -o -name 'ldgram.[ch]' -o -name 'deffilep.[ch]' \
         -o -name 'sysinfo.[ch]' \) -exec touch {} +
  fi
  unpack "$GCC" "$WORK/src/g" "${G_SKIP[@]}"
  tar -C "$WORK/src/g" -xf "$GCC" --wildcards '*/gcc/testsuite/selftests/*'   # all-gcc runs cc1's self tests
  G_DIR=$(echo "$WORK"/src/g/*)
  if [ -n "$GCC_GEN" ]; then
    unpack "$GCC_GEN" "$WORK/src/ggen"
    cp -r "$WORK"/src/ggen/*/. "$G_DIR/"
    touch "$G_DIR/gcc/gengtype-lex.cc"                 # newer than the .l: make never asks for flex
  fi
  if [ -n "$GCC_PATCHES" ]; then
    for p in "$GCC_PATCHES"/*.patch; do
      (cd "$G_DIR" && git apply --whitespace=nowarn "$p") && say "patch $(basename "$p")"
    done
  fi
  echo "$STAMP" > "$WORK/src/.unpacked"
fi
B_DIR=$(echo "$WORK"/src/b/*)
G_DIR=$(echo "$WORK"/src/g/*)
GCC_VER=$(cat "$G_DIR/gcc/BASE-VER")

if [ "$RESUME" = 1 ] && [ -f "$PREFIX/.target_files" ]; then
  say "target files: kept in $PREFIX"
else
  say "target files (newlib, libgcc, pspsdk) -> $PREFIX"
  tar -C "$PREFIX" -xf "$TARGET_FILES"
  touch "$PREFIX/.target_files"
fi
[ -d "$PREFIX/lib/gcc/psp/$GCC_VER" ] || { echo "target files have no lib/gcc/psp/$GCC_VER (gcc source is $GCC_VER)" >&2; exit 2; }

STATIC_LDFLAGS="-static -static-libgcc -static-libstdc++"
COMMON="--target=psp --prefix=$PREFIX_W --disable-nls --disable-shared --disable-werror"

# plugins and LTO on: pspdev's libpthread.a members carry LTO code; the Linux toolchain links them through the
# LTO plugin, and only the same plugin path gives the same EBOOT
B_CONF="$COMMON --disable-gdb --disable-gdbserver --disable-sim --disable-gprofng --without-zstd \
  --disable-readline --disable-libctf"
if [ "$RESUME" = 1 ] && [ -f "$PREFIX/.binutils_done" ]; then
  say "binutils: kept in $PREFIX"
else
  say "binutils: configure"
  rm -rf "$WORK/b-build" && mkdir -p "$WORK/b-build" && cd "$WORK/b-build"
  # -std=gnu17: older binutils name a variable static_assert, a keyword of C23 (the default of gcc 15+)
  "$B_DIR/configure" $B_CONF CFLAGS="-O2 -std=gnu17" LDFLAGS="$STATIC_LDFLAGS" MAKEINFO=true >> "$LOG" 2>&1
  say "binutils: make -j$JOBS"
  make -j"$JOBS" MAKEINFO=true >> "$LOG" 2>&1
  make install-strip MAKEINFO=true >> "$LOG" 2>&1
  touch "$PREFIX/.binutils_done"
  say "binutils: done ($(ls "$PREFIX/bin" | wc -l) programs)"
fi

# MSYS form of prefix and sysroot: libtool rejects a drive-letter path as a sysroot; the sysroot below the prefix keeps gcc
# relocatable (it finds both from its own location)
# zstd: the LTO sections in pspdev's libpthread.a are zstd-compressed (the Linux gcc has zstd). Languages c,c++ as
# pspdev's build (the converter compiles C only: the C++ programs are deleted after the install).
G_CONF="$COMMON --prefix=$PREFIX --with-sysroot=$PREFIX/psp --with-native-system-header-dir=/include --with-float=hard --with-newlib \
  --disable-libssp --disable-multilib --enable-threads=posix --disable-tls --enable-languages=c,c++ \
  --with-zstd=$SYSROOT_DEPS --with-gmp=$SYSROOT_DEPS --with-mpfr=$SYSROOT_DEPS --with-mpc=$SYSROOT_DEPS --with-isl=$SYSROOT_DEPS"
# configure through a relative path: srcdir ends up in gtyp-input.list, which the native gengtype.exe reads (it cannot
# open MSYS-form /c/... paths; MSYS converts only command-line arguments)
G_REL=$(realpath --relative-to="$WORK/g-build" -m "$G_DIR")
G_STAMP="$STAMP $G_CONF $G_REL"
if [ "$RESUME" = 1 ] && [ -f "$WORK/g-build/.configured" ] && [ "$(cat "$WORK/g-build/.configured")" = "$G_STAMP" ]; then
  say "gcc $GCC_VER: configured by the last run (make continues where it stopped)"
  cd "$WORK/g-build"
else
  say "gcc $GCC_VER: configure"
  rm -rf "$WORK/g-build" && mkdir -p "$WORK/g-build" && cd "$WORK/g-build"
  "$G_REL/configure" $G_CONF LDFLAGS="$STATIC_LDFLAGS" MAKEINFO=true >> "$LOG" 2>&1
  echo "$G_STAMP" > .configured
fi
say "gcc: make all-gcc -j$JOBS"
# The LTO plugin must be a DLL (ld loads plugins with LoadLibrary), but with --disable-shared and -static libtool
# makes only a static library and gcc's configure then turns the driver's plugin support off (it looks for
# gcc/liblto_plugin.la). So: build the plugin's object, link the DLL here, show configure the .la, then build gcc.
make -j"$JOBS" all-lto-plugin MAKEINFO=true >> "$LOG" 2>&1
LIBIB=libiberty-linker-plugin/libiberty.a
[ -f "$LIBIB" ] || LIBIB=libiberty/libiberty.a
gcc -shared -static-libgcc -o lto-plugin/liblto_plugin.dll lto-plugin/.libs/lto-plugin.o "$LIBIB" >> "$LOG" 2>&1
mkdir -p gcc && cp lto-plugin/liblto_plugin.la gcc/liblto_plugin.la
make -j"$JOBS" all-gcc MAKEINFO=true >> "$LOG" 2>&1
grep -q "define HAVE_LTO_PLUGIN [12]" gcc/auto-host.h || { echo "gcc configured without LTO plugin support" >&2; exit 2; }
make install-strip-gcc MAKEINFO=true >> "$LOG" 2>&1
install -m 755 lto-plugin/liblto_plugin.dll "$PREFIX/libexec/gcc/psp/$GCC_VER/" && strip "$PREFIX/libexec/gcc/psp/$GCC_VER/liblto_plugin.dll"
rm -f "$PREFIX/libexec/gcc/psp/$GCC_VER/cc1plus.exe" "$PREFIX/libexec/gcc/psp/$GCC_VER/g++-mapper-server.exe" \
      "$PREFIX/bin/psp-g++.exe" "$PREFIX/bin/psp-c++.exe"
[ -f "$PREFIX/libexec/gcc/psp/$GCC_VER/liblto_plugin.dll" ] || { echo "no liblto_plugin.dll: LTO members of the target libraries would link differently from the Linux toolchain" >&2; exit 2; }
say "gcc: done"

say "pspsdk host tools"
mkdir -p "$WORK/tools" && tar -C "$WORK/tools" -xf "$PSPSDK_TOOLS"
T=$(dirname "$(find "$WORK/tools" -name psp-fixup-imports.c | head -1)")
# the tools include the autotools config.h; the only macros they test: HAVE_STDINT_H, WORDS_BIGENDIAN (x86: unset)
mkdir -p "$WORK/tools/cfg"
printf '#define HAVE_STDINT_H 1\n#define HAVE_INTTYPES_H 1\n' > "$WORK/tools/cfg/config.h"
for t in psp-fixup-imports psp-prxgen psp-build-exports; do
  extra=""
  [ -f "$T/getopt_long.c" ] && grep -q getopt_long "$T/$t.c" && extra="$T/getopt_long.c"
  [ -f "$T/sha1.c" ] && grep -q "sha1 *(" "$T/$t.c" && extra="$extra $T/sha1.c"
  gcc -O2 -static -DHAVE_CONFIG_H -I"$WORK/tools/cfg" -I"$T" -o "$PREFIX/bin/$t.exe" "$T/$t.c" $extra >> "$LOG" 2>&1
  strip "$PREFIX/bin/$t.exe"
done

say "licences"
L="$PREFIX/licenses"
mkdir -p "$L/binutils" "$L/gcc" "$L/pspsdk" "$L/mingw-w64"
cp "$B_DIR"/COPYING* "$L/binutils/" 2>/dev/null || true
cp "$G_DIR"/COPYING* "$L/gcc/" 2>/dev/null || true
for f in "$T"/../LICENSE "$T"/LICENSE; do [ -f "$f" ] && cp "$f" "$L/pspsdk/"; done
for d in /ucrt64/share/licenses/{crt,winpthreads,libwinpthread,gmp,mpfr,mpc,isl,zlib,zstd,gcc-libs}; do
  [ -d "$d" ] && cp -r "$d" "$L/mingw-w64/$(basename "$d")"
done
# pthread-embedded / newlib / GMP-MPFR-MPC texts from the pinned archives, ISL's MIT text from an isl source archive or
# MSYS2's isl package (complete_licenses.py)
python3 "$(dirname "$0")/complete_licenses.py" "$PREFIX" "$(dirname "$GCC")" || say "licences: some texts missing (see above)"

say "build info"
{
  echo "PS2RC Windows toolchain - built $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "host compiler: $(gcc --version | head -1) ($MSYSTEM)"
  echo "binutils source: $(basename "$BINUTILS") $(sha256sum "$BINUTILS" | cut -c1-64)"
  [ -n "$BINUTILS_GEN" ] && echo "binutils generated parsers: $(basename "$BINUTILS_GEN") $(sha256sum "$BINUTILS_GEN" | cut -c1-64)"
  echo "gcc source: $(basename "$GCC") $(sha256sum "$GCC" | cut -c1-64) (version $GCC_VER)"
  [ -n "$GCC_PATCHES" ] && echo "gcc patches: $(ls "$GCC_PATCHES" | tr '\n' ' ')"
  [ -n "$GCC_GEN" ] && echo "gcc generated files: $(basename "$GCC_GEN") $(sha256sum "$GCC_GEN" | cut -c1-64)"
  echo "target files: $(basename "$TARGET_FILES") $(sha256sum "$TARGET_FILES" | cut -c1-64)"
  echo "pspsdk tools: $(basename "$PSPSDK_TOOLS") $(sha256sum "$PSPSDK_TOOLS" | cut -c1-64)"
  echo "binutils configure: $B_CONF CFLAGS=-O2 -std=gnu17 LDFLAGS=$STATIC_LDFLAGS"
  echo "gcc configure: $G_CONF LDFLAGS=$STATIC_LDFLAGS"
} > "$PREFIX/BUILDINFO.txt"
rm -rf "$PREFIX/share/info" "$PREFIX/share/man" "$PREFIX/share/doc"
say "toolchain: $PREFIX ($(du -sh "$PREFIX" | cut -f1)); log $LOG"
"$PREFIX/bin/psp-gcc.exe" --version | head -1 | tee -a "$LOG"
