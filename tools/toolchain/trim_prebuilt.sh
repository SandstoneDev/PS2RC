#!/bin/bash
# trim_prebuilt.sh - the part of a prebuilt pspdev release (Linux / macOS) that a converter bundle ships: the same set
# as the native Windows toolchain of build_windows.sh. Host side: binutils, the gcc / g++ drivers with cc1, cc1plus
# (the in-game menu is C++) and lto, and the three pspsdk host tools the converter calls. Target side: newlib, the pthread glue, libgcc / crt files and pspsdk
# (pack_target_files.sh). Left out: third-party psp-packages (SDL, Lua, psp-cfw-sdk, unrar ...), psp-gdb, the psplink
# tools, pacman and the documentation - so the source offer of a bundle is the pinned archives of
# toolchain.lock.toml plus the host libraries listed in BUILDINFO.txt.
#
#   bash tools/toolchain/trim_prebuilt.sh PSPDEV_DIR OUT_DIR [--sources DIR] [--bundle-host-libs]
#
# --sources DIR        the archives of fetch_sources.py: licence texts are copied from them into OUT_DIR/licenses/
#                      (binutils, gcc, pspsdk, newlib, pthread-embedded, GMP/MPFR/MPC note) and the commits of the
#                      prebuilt's build.txt are checked against the archives' commits (a mismatch fails: the source
#                      offer would not match the shipped binaries)
# --bundle-host-libs   copy the shared libraries the host programs need beyond the C library (pspdev links GMP, MPFR,
#                      MPC and zstd dynamically) into OUT_DIR/lib/host and point the programs at them (Linux: RUNPATH
#                      via patchelf; macOS: install_name_tool + an ad hoc signature); fails if a reference outside the
#                      system remains
set -Eeuo pipefail

SRC=${1:?pspdev install dir}
OUT=${2:?output dir}
shift 2
SOURCES=""
BUNDLE=0
while [ $# -gt 0 ]; do
  case "$1" in
    --sources) SOURCES=$(cd "$2" && pwd); shift 2 ;;
    --bundle-host-libs) BUNDLE=1; shift ;;
    *) echo "unknown argument $1" >&2; exit 2 ;;
  esac
done
SELF_DIR=$(cd "$(dirname "$0")" && pwd)
PY=${PYTHON:-python3}       # 3.11+ (tomllib)
SRC=$(cd "$SRC" && pwd)
[ ! -e "$OUT" ] || { echo "$OUT exists; remove it first" >&2; exit 2; }
VER=$(ls "$SRC/lib/gcc/psp" | tail -1)    # one version per install
[ -x "$SRC/libexec/gcc/psp/$VER/cc1" ] || { echo "no cc1 under $SRC/libexec/gcc/psp/$VER" >&2; exit 2; }
mkdir -p "$OUT/bin" "$OUT/psp/bin" "$OUT/libexec/gcc/psp/$VER"
OUT=$(cd "$OUT" && pwd)

copy() {   # copy one file (links resolved) if it exists
  if [ -e "$1" ]; then cp -pL "$1" "$2"; fi
}

# --- host programs (the Windows toolchain's set, ps2rc/convert/toolchain.py TOOLS + what the gcc driver runs)
for t in addr2line ar as c++filt cpp elfedit gcc g++ gcc-ar gcc-nm gcc-ranlib ld ld.bfd nm objcopy objdump ranlib readelf \
         size strings strip fixup-imports prxgen build-exports; do
  copy "$SRC/bin/psp-$t" "$OUT/bin/"
done
for t in ar as ld ld.bfd nm objcopy objdump ranlib readelf strip; do
  copy "$SRC/psp/bin/$t" "$OUT/psp/bin/"
done
for f in cc1 cc1plus collect2 lto-wrapper lto1 liblto_plugin.so liblto_plugin.dylib; do
  copy "$SRC/libexec/gcc/psp/$VER/$f" "$OUT/libexec/gcc/psp/$VER/"
done
if [ -d "$SRC/lib/bfd-plugins" ]; then mkdir -p "$OUT/lib/bfd-plugins" && cp -pL "$SRC"/lib/bfd-plugins/* "$OUT/lib/bfd-plugins/"; fi
for t in psp-gcc psp-g++ psp-as psp-ld psp-strip psp-fixup-imports psp-prxgen psp-size psp-objcopy; do
  [ -x "$OUT/bin/$t" ] || { echo "missing bin/$t in $SRC" >&2; exit 2; }
done
[ -x "$OUT/libexec/gcc/psp/$VER/cc1plus" ] || { echo "no cc1plus under $SRC/libexec/gcc/psp/$VER (in-game menu)" >&2; exit 2; }

# --- target files (no host binaries, no psp-packages)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
bash "$SELF_DIR/pack_target_files.sh" "$SRC" "$TMP/target.tar" --gcc-version "$VER"
tar -C "$OUT" -xf "$TMP/target.tar"
copy "$SRC/build.txt" "$OUT/build.txt"
# psp-packages must have stayed out (pack_target_files.sh reads pacman's file lists)
pk=$(ls "$OUT"/psp/lib/libSDL* "$OUT"/psp/lib/liblua* "$OUT"/psp/lib/libpng* 2>/dev/null | head -3 || true)
[ -z "$pk" ] || { echo "psp-packages were not left out: $pk" >&2; exit 2; }

# --- host libraries
os=$(uname -s)
system_lib() {   # libraries every supported system has (C library, loader, the OS frameworks)
  case "$1" in
    linux-vdso.so*|*/ld-linux*|ld-linux*|libc.so*|libm.so*|libdl.so*|libpthread.so*|librt.so*|libresolv.so*) return 0 ;;
    /usr/lib/*|/System/*|@loader_path/*|@executable_path/*) [ "$os" = Darwin ] && return 0 ;;
  esac
  return 1
}
host_files() { find "$OUT/bin" "$OUT/psp/bin" "$OUT/libexec" "$OUT/lib/bfd-plugins" -type f 2>/dev/null; }
deps_of() {      # -> the shared libraries one file references (nothing for scripts / non-binaries)
  if [ "$os" = Darwin ]; then
    otool -L "$1" 2>/dev/null | tail -n +2 | awk '{print $1}' | grep -v "^$1\$" || true
  else
    ldd "$1" 2>/dev/null | awk '/=>/ {print ($3 == "not" ? $1 : $3); next} /^\s*\// {print $1}' || true
  fi
}
foreign=$(for f in $(host_files); do for d in $(deps_of "$f"); do system_lib "$(basename "$d")" || system_lib "$d" || echo "$d"; done; done | sort -u)
if [ -n "$foreign" ]; then
  if [ "$BUNDLE" = 0 ]; then
    echo "note: the host programs need these libraries beyond the C library (use --bundle-host-libs):"; echo "$foreign" | sed 's/^/  /'
  else
    L="$OUT/lib/host"
    mkdir -p "$L"
    for d in $foreign; do
      [ -f "$d" ] || { echo "host library $d not found on this machine" >&2; exit 2; }
      cp -pL "$d" "$L/"
      chmod u+w "$L/$(basename "$d")"
    done
    if [ "$os" = Darwin ]; then
      # libraries can reference each other (MPC -> MPFR -> GMP): rewrite every foreign path everywhere
      for f in $(host_files) "$L"/*; do
        changed=0
        for d in $(deps_of "$f"); do
          case " $(echo $foreign) " in *" $d "*) ;; *) continue ;; esac
          rel=$("$PY" -c 'import os,sys; print(os.path.relpath(sys.argv[1], os.path.dirname(sys.argv[2])))' "$L" "$f")
          install_name_tool -change "$d" "@loader_path/$rel/$(basename "$d")" "$f"
          changed=1
        done
        case "$f" in "$L"/*) install_name_tool -id "@loader_path/$(basename "$f")" "$f"; changed=1 ;; esac
        if [ "$changed" = 1 ]; then codesign --force --sign - "$f"; fi
      done
    else
      command -v patchelf >/dev/null || { echo "patchelf is needed for --bundle-host-libs" >&2; exit 2; }
      for f in $(host_files); do
        if [ -n "$(deps_of "$f")" ] && file "$f" | grep -q ELF; then
          rel=$("$PY" -c 'import os,sys; print(os.path.relpath(sys.argv[1], os.path.dirname(sys.argv[2])))' "$L" "$f")
          patchelf --set-rpath "\$ORIGIN/$rel" "$f"
        fi
      done
      for f in "$L"/*; do patchelf --set-rpath '$ORIGIN' "$f"; done
    fi
    # what remains must resolve inside the folder or to the system
    left=$(for f in $(host_files) "$L"/*; do for d in $(deps_of "$f"); do
             case "$d" in "$L"/*|@loader_path/*) continue ;; esac
             system_lib "$(basename "$d")" || system_lib "$d" || echo "$f -> $d"; done; done | sort -u)
    [ -z "$left" ] || { echo "host references outside the bundle remain:"; echo "$left"; exit 2; } >&2
    {
      echo "host libraries bundled in lib/host (taken from the build machine; their source: the packages below)"
      for d in $foreign; do
        if [ "$os" = Darwin ]; then
          echo "  $(basename "$d")  <-  $d  (Homebrew $(brew list --versions "$(echo "$d" | sed -n 's#.*/opt/\([^/]*\)/.*#\1#p')" 2>/dev/null || echo '?'))"
        else
          echo "  $(basename "$d")  <-  $d  ($(dpkg -S "$(readlink -f "$d")" 2>/dev/null | cut -d: -f1 | head -1 | \
                 xargs -r dpkg-query -W -f='${Package} ${Version}, source ${source:Package} ${source:Version}' 2>/dev/null || echo '?'))"
        fi
      done
    } > "$OUT/HOST_LIBS.txt"
    cat "$OUT/HOST_LIBS.txt"
  fi
fi

# --- licence texts and the source check
if [ -n "$SOURCES" ]; then
  LIC="$OUT/licenses"
  mkdir -p "$LIC/binutils" "$LIC/gcc" "$LIC/pspsdk"
  one() { ls "$SOURCES"/$1-*.tar.gz 2>/dev/null | head -1; }
  topfiles() {   # archive dest regex: copy the matching top-level files of an archive (GNU tar and bsdtar)
    local a=$1 d=$2 re=$3 top names
    top=$(tar -tzf "$a" | head -1 | cut -d/ -f1)
    names=$(tar -tzf "$a" | grep -E "^$top/($re)\$" || true)
    [ -n "$names" ] || { echo "licences: no $re in $(basename "$a")"; return 0; }
    # shellcheck disable=SC2086
    tar -xzf "$a" -C "$TMP" $names
    for n in $names; do cp "$TMP/$n" "$d/"; done
    rm -rf "${TMP:?}/$top"
  }
  topfiles "$(one binutils)" "$LIC/binutils" 'COPYING[^/]*'
  topfiles "$(one gcc)" "$LIC/gcc" 'COPYING[^/]*'
  topfiles "$(one pspsdk)" "$LIC/pspsdk" 'LICENSE[^/]*'
  "$PY" "$SELF_DIR/complete_licenses.py" "$OUT" "$SOURCES" || echo "licences: some texts missing (see above)"
  # the prebuilt must be built from the pinned commits, or the archives are not its source
  # (STRICT_SOURCES=0: only warn - for a test build that is never published)
  if ! "$PY" "$SELF_DIR/check_prebuilt_commits.py" "$OUT/build.txt"; then
    if [ "${STRICT_SOURCES:-1}" = 0 ]; then echo "warning: source check failed (STRICT_SOURCES=0)"; else exit 2; fi
  fi
fi
echo "trimmed toolchain: $OUT ($(du -sh "$OUT" | cut -f1), gcc $VER)"
