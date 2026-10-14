PS2RC Converter
===============

Start: double-click "PS2RC Converter.exe". Nothing has to be installed: the folder brings its own Python, Qt for
Python, the PSP toolchain and the helper programs. Keep the folder together; it can live anywhere (also on a USB
drive), the path may contain spaces.

Requirements
------------
- 64-bit Windows 10 version 1809 or newer, or Windows 11 (the window is Qt 6; Qt's supported Windows versions:
  https://doc.qt.io/qt-6/windows.html). 32-bit Windows and Windows 7 / 8 do not work.
- Nothing else. The Microsoft Visual C++ runtime files the programs need are inside the folder; no .NET, no
  "Visual C++ Redistributable", no Python, no MSYS2 / WSL.
- Windows N / KN editions only: the menu music encoder (tools/atracdenc) needs Windows Media Foundation, which these
  editions lack until Microsoft's free Media Feature Pack is installed:
  https://support.microsoft.com/help/3145500
  Without it everything else works; the window says that the menu music is off.

Menu music: three choices on the Music tab - Best quality (ATRAC3plus 352k, ~11 s), Balanced (ATRAC3 132k,
~30 s, the default), Longest (ATRAC3 66k, ~60 s); the animated icon and the music share 500 KB. ATRAC3 is outside
Sony's documented menu-music format (ATRAC3plus) but played in the menu of a PSP-3000 with custom firmware in our
test; other PSP models are untested - pick Best quality if a PSP stays silent.

If the window does not open: send %LOCALAPPDATA%\PS2RC\gui_start.log. The log of each conversion is in
%LOCALAPPDATA%\PS2RC\logs.

Licences of the parts: the licenses folder (Python, Qt for Python / Qt: LGPL-3.0, the toolchain, ffmpeg, atracdenc).
