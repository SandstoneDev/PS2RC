#!/usr/bin/env bash
# A small ffmpeg + ffprobe for the converter app (tools/ffmpeg): only what PS2RC runs, statically linked, no DLLs.
#
#   MSYS2 UCRT64 shell:  bash gui/ffmpeg_min/build_ffmpeg_min.sh FFMPEG_SRC_DIR OUT_DIR [X264_SRC_DIR]
#
# Needs: the ffmpeg release source (unpacked, e.g. ffmpeg-8.0.1 from https://ffmpeg.org/releases/), MSYS2 packages
# mingw-w64-ucrt-x86_64-gcc, make, pkgconf, mingw-w64-ucrt-x86_64-zlib (PNG), and optionally nasm (without it the
# decoders, the scaler and x264 are built without SIMD: slower, same results).
# X264_SRC_DIR (recommended for a shipped build): an unpacked x264 source snapshot
# (https://code.videolan.org/videolan/x264, a pinned commit); it is built here as a static library and linked in, so
# the shipped program corresponds exactly to the two source archives. Without it the MSYS2 package
# mingw-w64-ucrt-x86_64-libx264 (static libx264.a) is used.
# Result: OUT_DIR/ffmpeg.exe, ffprobe.exe, LICENSE.GPL-2.0.txt (ffmpeg + libx264 = GPL), README.txt (versions,
# configure lines: the source offer of tools/ffmpeg names exactly these).
#
# What PS2RC runs (ps2rc/xmb, ps2rc/pbp/icon1.py, ps2rc/movies/convert_movies_open.py):
#   read clips / pictures / music the player picks: mp4 / m4a / mov, mkv / webm, avi, gif, png / jpeg / webp (still),
#     wav, mp3, ogg (vorbis / opus), flac, aac; the PS2 intro movies (MPEG-2 program stream); our PMF / AT3 files
#   filters: crop, scale (lanczos), pad, fps, setsar, format, aresample (+ the auto-inserted null / anull / aformat)
#   write: H.264 Main@2.1 through libx264 (-f h264 elementary stream, two-pass), rawvideo rgb24 / s16le to a pipe,
#     PCM / IMA ADPCM WAV, one PNG frame (image2pipe / image2), -f null for checks (decoded frames: wrapped_avframe)
#   read from / write to pipes: protocols pipe and fd (ffmpeg 8 opens '-' through fd)
set -euo pipefail
SRC=$(cd "$1" && pwd)
OUT=$(mkdir -p "$2" && cd "$2" && pwd)
X264_SRC=${3:+$(cd "$3" && pwd)}
BUILD=${BUILD:-$OUT/_build}
mkdir -p "$BUILD"
ASM=--disable-x86asm
command -v nasm >/dev/null 2>&1 && ASM=
X264_CONF="from MSYS2 package mingw-w64-ucrt-x86_64-libx264"
if [ -n "$X264_SRC" ]; then
  X264_ASM=--disable-asm
  command -v nasm >/dev/null 2>&1 && X264_ASM=
  mkdir -p "$BUILD/x264" && cd "$BUILD/x264"
  X264_CONF="--prefix=$BUILD/x264/prefix --host=x86_64-w64-mingw32 --enable-static --disable-cli --disable-opencl --disable-lavf --disable-swscale --disable-ffms --disable-gpac --disable-lsmash $X264_ASM --extra-cflags=-O2"
  # shellcheck disable=SC2086
  "$X264_SRC/configure" $X264_CONF
  make -j"$(nproc)"
  make install-lib-static
  export PKG_CONFIG_PATH="$BUILD/x264/prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
  X264_CONF="built from source: configure $X264_CONF"
fi
DEMUX=mov,matroska,avi,gif,image2,image2pipe,png_pipe,jpeg_pipe,webp_pipe,wav,mp3,ogg,flac,aac,mpegps,mpegvideo,h264,rawvideo,pcm_s16le,oma
DEC=h264,hevc,mpeg1video,mpeg2video,mpeg4,msmpeg4v3,mjpeg,vp8,vp9,theora,gif,png,webp,rawvideo,pcm_s16le,pcm_s16be,pcm_s24le,pcm_f32le,pcm_u8,mp3,mp3float,mp2,aac,vorbis,opus,flac,alac,atrac3,atrac3p,adpcm_ima_wav,adpcm_ms
ENC=libx264,rawvideo,png,pcm_s16le,adpcm_ima_wav,wrapped_avframe
MUX=h264,rawvideo,pcm_s16le,wav,image2,image2pipe,null
FILT=crop,scale,pad,fps,setsar,format,aresample,aformat,null,anull,trim,atrim,setpts,asetpts
PARSE=h264,hevc,mpegvideo,mpeg4video,mpegaudio,aac,flac,vorbis,opus,vp8,vp9,png,mjpeg,gif
BSF=h264_mp4toannexb,vp9_superframe_split,null
cd "$BUILD"
"$SRC/configure" --prefix="$BUILD/prefix" --target-os=mingw32 --arch=x86_64 \
  --disable-everything --disable-autodetect --disable-network --disable-doc --disable-debug --disable-shared \
  --enable-static --enable-gpl --enable-libx264 --enable-zlib $ASM \
  --enable-ffmpeg --enable-ffprobe --disable-ffplay \
  --enable-protocol=file,pipe,fd \
  --enable-demuxer=$DEMUX --enable-decoder=$DEC --enable-encoder=$ENC --enable-muxer=$MUX \
  --enable-filter=$FILT --enable-parser=$PARSE --enable-bsf=$BSF \
  --enable-swscale --enable-swresample \
  --pkg-config-flags=--static --extra-ldflags="-static -static-libgcc" --extra-cflags=-O2
make -j"$(nproc)"
strip -o "$OUT/ffmpeg.exe" ffmpeg.exe
strip -o "$OUT/ffprobe.exe" ffprobe.exe
cp "$SRC/COPYING.GPLv2" "$OUT/LICENSE.GPL-2.0.txt"
{
  echo "ffmpeg + ffprobe built for PS2RC (gui/ffmpeg_min/build_ffmpeg_min.sh), statically linked."
  echo "ffmpeg source: $(cat "$SRC/RELEASE" 2>/dev/null || echo unknown) (https://ffmpeg.org/releases/)"
  echo "libx264 $(pkg-config --modversion x264) (GPL-2.0+, https://code.videolan.org/videolan/x264): $X264_CONF"
  [ -n "$X264_SRC" ] && echo "x264 source tree: $(basename "$X264_SRC")"
  echo "configuration: $(./ffmpeg -hide_banner -buildconf 2>&1 | tr '\n' ' ')"
} > "$OUT/README.txt"
ls -la "$OUT"
