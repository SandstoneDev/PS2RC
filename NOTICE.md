# Third-party notices

PS2RC is covered by the [PS2RC License](LICENSE). The items below are not: each keeps
its own licence. PS2RC's repository contains one third-party source tree, Dear ImGui in
`lib/psp_imgui/third_party/imgui/` (section "Dear ImGui" below); it builds with and links against the
toolchains listed here, and its splash screen contains glyph bitmaps rendered from a free
font.

## Dear ImGui (the in-game settings menu)

`lib/psp_imgui/third_party/imgui/` is **Dear ImGui** 1.91.9b by Omar Cornut and contributors
([github.com/ocornut/imgui](https://github.com/ocornut/imgui)), unchanged, MIT licence
(Copyright (c) 2014-2025 Omar Cornut; the full text is `lib/psp_imgui/third_party/imgui/LICENSE.txt`).
`lib/psp_imgui/pimgui_font13.h` holds glyph bitmaps of **ProggyClean.ttf** by Tristan Grimmer
(MIT licence, [proggyfonts.net](http://www.proggyfonts.net)), as embedded in Dear ImGui.
Both are compiled into `EBOOT.PBP` (the menu opened with L + R + Cross + D-pad Down).

Dear ImGui licence:

```
The MIT License (MIT)

Copyright (c) 2014-2025 Omar Cornut

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

ProggyClean.ttf licence (as stated in Dear ImGui's `imgui_draw.cpp`: "ProggyClean.ttf, Copyright (c) 2004, 2005
Tristan Grimmer, MIT license"):

```
The MIT License (MIT)

Copyright (c) 2004, 2005 Tristan Grimmer

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## The author's earlier PSP projects

Parts of the PSP platform layer - the Media Engine bring-up, the design of the movie
player and the splash screen's QR matrix - come from SandstoneDev's earlier PSP
projects. They are published here by their author under the PS2RC License.

The Media Engine bring-up (`hle/psp/hle_me.c`) keeps the register sequence of the
earlier project, which took its boot pattern from mcidclan's
**me-feed-graphics-engine** (MIT licence, Copyright (c) 2024 m-c/d). The code here is
rewritten; the credit and the licence text stay:

```
Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Toolchain and system libraries

The repository does not include them. The converter app (a release download) ships the
PSP toolchain next to the converter as a separate program, with each part's licence
texts in `toolchain/licenses/` and the source offer of section "Converter app" below.

- **pspdev** ([github.com/pspdev](https://github.com/pspdev)): GCC, binutils and newlib
  for the PSP, and **PSPSDK**. The PSP build links PSPSDK libraries into `EBOOT.PBP`.
  PSPSDK is under a BSD-style licence:

  ```
  Copyright (c) 2005  adresd
  Copyright (c) 2005  Marcus R. Brown
  Copyright (c) 2005  James Forshaw
  Copyright (c) 2005  John Kelley
  Copyright (c) 2005  Jesper Svennevid
  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions
  are met:
  1. Redistributions of source code must retain the above copyright
     notice, this list of conditions and the following disclaimer.
  2. Redistributions in binary form must reproduce the above copyright
     notice, this list of conditions and the following disclaimer in the
     documentation and/or other materials provided with the distribution.
  3. The names of the authors may not be used to endorse or promote products
     derived from this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE AUTHORS ``AS IS'' AND ANY EXPRESS OR
  IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
  OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
  IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY DIRECT, INDIRECT,
  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
  NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
  THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
  ```

  If you distribute a program built with PS2RC, include this notice in its
  documentation (clause 2).
- **newlib** (the C library linked into PSP builds) is under the licences listed in
  its `COPYING.NEWLIB`, shipped with the toolchain.
- **libgcc** (GCC's runtime library, linked into PSP builds) is under the GPL-3.0 with
  the GCC Runtime Library Exception: no condition on the programs built with it.
- **libstdc++ headers / libsupc++** (GCC's C++ library, for the in-game menu's C++
  code; shipped with the toolchain): GPL-3.0 with the same GCC Runtime Library
  Exception. The menu is built without exceptions and RTTI and links no C++ library
  archive; only inline header code ends up in a PSP build.
- **pthread-embedded** (`libpthread.a`): the toolchain's default libraries link a few of
  its members into every PSP build. Licence: GNU LGPL 2.1 or later (its Vita patches:
  MIT, Copyright (c) 2016 Davee). PS2RC distributes no PSP program; whoever distributes a
  program built with PS2RC must meet the LGPL's conditions for it (library source, the
  means to relink). The converter app ships the library's source with the toolchain.
- **Converter app.** The app bundles the PSP toolchain (GCC, binutils: GPL-3.0; GMP,
  MPFR, MPC: LGPL-3.0; ISL: MIT; zlib, zstd, MinGW-w64 runtime: their licences) and a
  private Python (PSF licence; pyelftools: public domain, with construct: MIT; Pillow
  ([python-pillow.org](https://python-pillow.org), MIT-CMU licence): image conversion of the menu art). Their
  licence texts are in the app's `licenses/` folders; the source code of the toolchain
  (the pinned archives, the build scripts in `tools/toolchain/`, the build information)
  is offered next to every download of the app, for as long as it is offered. These
  parts keep their own licences: the PS2RC License's conditions do not apply to them.
- **Custom-firmware interfaces.** The PSP build calls two functions that custom
  firmware exports (`kuKernelCall` of the KUBridge library and `sctrlHENFindFunction`
  of SystemCtrlForKernel). It imports them through its own stubs
  (`hle/psp/ps2rc_kcall.S`, `psp_exc/ps2rc_sctrl.S`), written with PSPSDK's import
  macros; no header or library of a custom-firmware SDK is included or linked.
- The **PSPSDK debug font** (libpspdebug) is used for on-screen diagnostics; it is
  part of PSPSDK and covered by the notice above.

## Font

The start-up credit screen contains text pre-rendered from the **DejaVu** fonts
([dejavu-fonts.github.io](https://dejavu-fonts.github.io/)). DejaVu fonts are based
on Bitstream Vera; changes are in the public domain. The licence:

```
Fonts are (c) Bitstream (see below). DejaVu changes are in public domain.

Bitstream Vera Fonts Copyright
------------------------------

Copyright (c) 2003 by Bitstream, Inc. All Rights Reserved. Bitstream Vera is
a trademark of Bitstream, Inc.

Permission is hereby granted, free of charge, to any person obtaining a copy
of the fonts accompanying this license ("Fonts") and associated
documentation files (the "Font Software"), to reproduce and distribute the
Font Software, including without limitation the rights to use, copy, merge,
publish, distribute, and/or sell copies of the Font Software, and to permit
persons to whom the Font Software is furnished to do so, subject to the
following conditions:

The above copyright and trademark notices and this permission notice shall
be included in all copies of one or more of the Font Software typefaces.

The Font Software may be modified, altered, or added to, and in particular
the designs of glyphs or characters in the Fonts may be modified and
additional glyphs or characters may be added to the Fonts, only if the fonts
are renamed to names not containing either the words "Bitstream" or the word
"Vera".

This License becomes null and void to the extent applicable to Fonts or Font
Software that has been modified and is distributed under the "Bitstream
Vera" names.

The Font Software may be sold as part of a larger software package but no
copy of one or more of the Font Software typefaces may be sold by itself.

THE FONT SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO ANY WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF COPYRIGHT, PATENT,
TRADEMARK, OR OTHER RIGHT. IN NO EVENT SHALL BITSTREAM OR THE GNOME
FOUNDATION BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, INCLUDING
ANY GENERAL, SPECIAL, INDIRECT, INCIDENTAL, OR CONSEQUENTIAL DAMAGES,
WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
THE USE OR INABILITY TO USE THE FONT SOFTWARE OR FROM OTHER DEALINGS IN THE
FONT SOFTWARE.

Except as contained in this notice, the names of Gnome, the Gnome
Foundation, and Bitstream Inc., shall not be used in advertising or
otherwise to promote the sale, use or other dealings in this Font Software
without prior written authorization from the Gnome Foundation or Bitstream
Inc., respectively. For further information, contact: fonts at gnome dot
org.
```

## References (no code taken)

These projects and documents were used as behavioural references or sources of
public facts (instruction behaviour, syscall numbers, library semantics). No code
from them is included.

- **PCSX2** ([pcsx2.net](https://pcsx2.net/)), GPL-3.0-or-later: the reference
  emulator for bit-exact tests, driven through its PINE interface; behavioural
  reference for the R5900 FPU and VU0 rules and the SPU2 model.
- **PPSSPP** ([ppsspp.org](https://www.ppsspp.org/)), GPL: test platform.
- **ps2sdk** ([github.com/ps2dev](https://github.com/ps2dev)), AFL-2.0: EE syscall
  numbering and the behaviour, command numbers and error codes of the memory-card
  library; the memory-card HLE is written anew on top of host files, no ps2sdk code is
  copied.
- **Play!** (a PS2 emulator), BSD-2-Clause: semantics of the EE kernel's syscalls.
- **PS2Recomp**, GPL-3.0: the signature-matching method that the Sony library scan
  re-implements. Its signature database is not included and is not needed to build
  PS2RC.
- **psx-spx** ([problemkaputt.de/psx-spx.htm](https://problemkaputt.de/psx-spx.htm)):
  public documentation of the PlayStation sound hardware.
- **BloodPSP** ([github.com/RainArcanjo/BloodPSP](https://github.com/RainArcanjo/BloodPSP)), launcher MIT: the
  layout of the converter's Controls page (a drawn PSP with the buttons' roles around it) is inspired by its
  launcher. No code, artwork or icons of it are included; the PSP drawing is our own.
- **uofw** and the PSP homebrew community's Media Engine research: facts about the
  PSP's firmware interfaces and the Media Engine's start-up.

## Host build

The PC build of the runtime links the system's **zlib** (for PNG frame dumps) and C
library; they are not part of this repository.

## File formats

PS2RC reads data in formats defined by others: ELF, ISO9660, the PS2's ADPCM sound
format, Metrowerks code overlays, and writes PSP formats (EBOOT.PBP, PSMF). The code
that handles them in this repository is original.

## The converter window: Qt for Python

The converter window (gui/) is written with **Qt for Python (PySide6)** and runs on **Qt**
([qt.io](https://www.qt.io), [pyside.org](https://www.pyside.org)), used under the GNU Lesser General Public
License v3.0. The repository contains no Qt code. The converter app ships the unmodified PySide6 / shiboken6
packages of the version pinned in gui/build_requirements_qt.txt (Qt Widgets only) as separate, replaceable
files in its Python folder, with the LGPL-3.0 and GPL-3.0 texts and the download place of the exact source
in licenses/Qt of the app.

## Programs the converter app ships next to it (separate programs, not included in the repository)

The window makes the animated menu icon from a clip and the menu music from a song, and the converter makes
the intro movies, with two programs that run as separate processes. The converter app ships them (tools/ffmpeg, tools/atracdenc); each keeps its own
licence: its licence text and where to get its source code are in licenses/<name> of the app. PS2RC's own code
does not link them.

- **atracdenc** ([github.com/dcherednik/atracdenc](https://github.com/dcherednik/atracdenc)), LGPL-2.1, the
  Windows build of release 0.2.3: encodes the menu music (ATRAC3 by default, ATRAC3plus on request). PS2RC re-wraps its output file in the
  container layout described in `ps2rc/xmb/sound.py`.
- **FFmpeg** ([ffmpeg.org](https://ffmpeg.org)): reads clips, songs and the intro movies and encodes their H.264
  picture with libx264 (GPL-2.0-or-later), so the build the app ships (configured with --enable-gpl, without
  --enable-version3) is under the GNU General Public License v2.0 or later, text in licenses/ffmpeg (its
  configuration and version are in tools/ffmpeg/README.txt; the source of the FFmpeg release and of the
  libraries named there is offered as licenses/ffmpeg describes).

Without them the window still works; those two items are then off and the window says why.
