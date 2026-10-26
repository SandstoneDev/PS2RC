# ps2rc.pbp - EBOOT.PBP with your own menu assets

Writes the PSP application container (EBOOT.PBP) and its parameter file (PARAM.SFO), and puts the pictures, the
animated icon and the music of the PSP menu into it. Pure Python; the converter's GUI and command line call it.

Optional pieces, each only for the step that needs it:

| Piece | Needed for | Without it |
|---|---|---|
| Pillow (`python -m pip install pillow`) | converting pictures of another size or format | pictures must already be exact PNGs (the error says which size) |
| [ffmpeg](https://ffmpeg.org) with libx264 (user-installed) | ICON1 from a video or an animated GIF (muxed by `ps2rc/movies/pmfmux.py`) | give a ready 144x80 `.pmf`, or no ICON1 |

## The menu assets

| File in the PBP | What the menu does with it | Format | Size | Needed |
|---|---|---|---|---|
| `PARAM.SFO` | title and parameters | parameter file (see `sfo.py`) | - | yes |
| `ICON0.PNG` | still icon; also shown when ICON1 is missing or cannot play | PNG 24-bit RGB | 144x80 | commonly treated as required; PBPs without it still appear and start |
| `ICON1.PMF` | animated icon while the entry is selected, looped | PSMF, H.264 Main profile level 2.1, 29.97 fps progressive; sound only as ATRAC3plus | 144x80 | no |
| `PIC0.PNG` | information picture, drawn over the background | PNG 32-bit RGBA (may be transparent) | 310x180 | no |
| `PIC1.PNG` | background while the entry is selected | PNG 24-bit RGB | 480x272 | no |
| `SND0.AT3` | music while the entry is selected; loops when the file has loop points | RIFF/WAVE with ATRAC3plus, 44.1 kHz, stereo | ICON1 + SND0 together at most 500 KB | no |
| `DATA.PSP` | the program | ELF/PRX | - | yes |
| `DATA.PSAR` | extra data of the program | any | - | no |

The formats, sizes and the 500 KB limit in the table are the commonly used values for
PSP menu assets; no public specification of them is cited here, and the packer
follows them so the menu shows the assets as expected. The header lists the eight
files in exactly this order (see `pbp.py`). When an entry is selected, the menu shows
PIC1 first, then fades in PIC0 and ICON1 and starts SND0 at about the same time; their exact timing and layout are
not fixed. When ICON1 has its own sound and SND0 exists, SND0 is the one expected to play.

Unknown (not verified): what the menu does with files over the limits or in other formats (an RGBA ICON0, an
ATRAC3 SND0, larger pictures); whether 500 KB means 500,000 or 512,000 bytes (the packer aims below 500,000 and
refuses above 512,000); whether SND0 without loop points repeats. The packer refuses files outside these commonly
used formats unless `allow_off_spec` / `--allow-off-spec` is given.

ICON1 layout: the muxer writes PSMF version 0015 without an entry-point map (sequential play). `--icon1-epmap`
adds a map (one entry per IDR picture: its time stamp and pack number) and writes version 0012; only the 2048-byte
header changes. Which of the two the menu needs is Unknown until it is tried on a PSP.

**SND0.AT3 is accepted only as a ready ATRAC3plus file.** No open-source ATRAC3plus encoder is known to this
project, so WAV or MP3 music is not converted.

## Command line

Run from the repository root:

```
python -m ps2rc.pbp pack --eboot EBOOT.PBP --out OUT/EBOOT.PBP --title "My title" \
    --icon0 icon.png --icon1 clip.mp4 --pic0 info.png --pic1 back.jpg --snd0 music.at3
python -m ps2rc.pbp pack --data-psp program.elf --out EBOOT.PBP --title "My title" --memsize 1
python -m ps2rc.pbp info EBOOT.PBP [--strict]
python -m ps2rc.pbp unpack EBOOT.PBP DIR
python -m ps2rc.pbp repack DIR EBOOT.PBP
python -m ps2rc.pbp placeholders DIR [--text PS2RC] [--sub "SECOND LINE"] [--icon1]
```

`pack` keeps everything the input EBOOT has and replaces only what is given. Other options: `--disc-id`,
`--disc-version`, `--app-ver`, `--parental`, `--memsize`, `--category`, `--set KEY=TEXT`, `--set-int KEY=N`,
`--drop icon1` (remove an asset), `--fit cover|contain|stretch`, `--background RRGGBB`, `--icon0-alpha`,
`--icon1-seconds S` (longest part of a video used for ICON1, default 10), `--icon1-epmap` (see below),
`--ffmpeg PATH`, `--allow-off-spec`. Exit status 0 = written, 1 = refused (nothing written; the message says why).

`placeholders` writes neutral pictures drawn from code (a text icon, an information panel, a gradient background,
and with `--icon1` a short animation) for builds that have no assets of their own.

## Editing an existing EBOOT (`editor.py`, `eboot_edit.py`)

`python -m ps2rc.pbp.eboot_edit EBOOT.PBP [--title T] [--icon0 PIC --icon0-shape wide|square] [--pic0 PIC]
[--pic1 PIC] [--icon1 PMF|clip] [--snd0 AT3|audio] [--remove SLOT] [--fix] [--extract DIR] [--out FILE]
[--no-backup]` changes the menu look and the title of any EBOOT.PBP (PS2RC games, PSP homebrew, PS1 eboots). The
converter window has the same as Tools > EBOOT editor (`gui/eboot_editor.py`). Rules:

- DATA.PSP and DATA.PSAR are copied byte for byte (DATA.PSAR is streamed, it can be hundreds of MB); with nothing
  changed the rebuilt file equals the original (tested). The original is renamed to `EBOOT.PBP.bak` (or `.bak2`...)
  unless `--no-backup`; the new file is written beside it, read back and compared before the rename.
- Files before DATA.PSAR are written back to back (as pack-pbp does). A packed "~PSP" DATA.PSP followed by zero
  padding (PS1 eboots) is counted by its own size (u32 at 0x2C of its header). DATA.PSAR keeps its offset when the
  new files fit before it, else it moves to the next 64 KiB boundary, as the public PS1 packers place it. The PS1
  data inside refers to itself relative to the start of DATA.PSAR (the multi-disc table at +0x200 too: the ARK-5
  Popcorn source adds the DATA.PSAR offset to it), so it stays valid.
- ICON0 is 144x80 (16:9) for PSP programs and 80x80 for PS1 eboots. The PS1 emulator also reads ICON0 when a PS1
  game starts: the Popcorn module of the PRO and ARK custom firmwares (public source,
  https://github.com/PSP-Arkfive/ARK-5 `PopCorn/src/syspatch.c`, `getIcon0Status`) accepts only an 80x80 PNG there
  and gives the emulator a stock icon otherwise; elsewhere the effect of a 144x80 icon is Unknown. The editor
  therefore warns on a 144x80 icon in a PS1 eboot, and `--fix` makes it 80x80.
- Pictures are checked like the packer's (exact size, 8-bit RGB / RGBA, not interlaced, intact chunks); `--fix`
  re-makes them and drops data after the PNG end chunk.

## Python API

```python
from ps2rc.pbp import Pbp, Sfo, make_sfo, apply, set_params, check, describe, PbpError

p = Pbp.read('EBOOT.PBP')                    # or Pbp.parse(bytes); Pbp() for a new one
set_params(p, title='My title', memsize=1)   # PARAM.SFO values; rebuilt only when something changes
notes = apply(p, icon0='icon.png', icon1_src='clip.mp4', pic1='back.png', snd0='music.at3',
              fit='cover', icon1_seconds=10.0, allow_off_spec=False,
              ffmpeg=None, icon1_epmap=False, log=print)    # paths or bytes; raises PbpError, p unchanged
for issue in check(p):                       # Issue(level, what, message); level: error/spec/warn/info
    print(issue)
p.write('OUT/EBOOT.PBP')                     # p.build() -> bytes
```

- `Pbp`: `p['ICON0.PNG']` read/write a file by its name (`spec.SLOTS`), `offsets()`, `build()`, `write()`,
  `unpack(dir)`, `Pbp.from_dir(dir)`. A PBP read and written unchanged is byte-identical.
- `Sfo`: `Sfo.parse(bytes)`, `get/set/remove/keys/to_dict`, `build()`, `validate()`; `make_sfo(title, extra)` writes
  what pspsdk's `mksfoex` writes (same bytes). `standard` tells whether the file is in the layout `build()`
  reproduces.
- `image.prepare(src, spec, fit, background, keep_alpha)` -> (PNG bytes, notes); `image.check(data, spec)`.
- `icon1.encode(src=... or frames=..., budget, max_seconds, fit, ffmpeg, epmap)` -> (PMF bytes, report);
  `icon1.check(data)`, `icon1.parse(data)`, `icon1.entry_points(data)`, `icon1.add_entry_points(data, version)`.
- `at3.parse(data)`, `at3.check(data)`.
- `placeholder.icon0(text)`, `pic0(text, sub)`, `pic1()`, `icon1_frames(text, seconds)`.

## Sources

- PBP and PARAM.SFO layouts: pspsdk's pack-pbp.c, mksfoex.c and build.mak
  (https://github.com/pspdev/pspsdk); PPSSPP's Core/ELF/PBPReader.cpp and Core/ELF/ParamSFO.cpp
  (https://github.com/hrydgard/ppsspp). The tests compare the output with pspsdk's `pack-pbp` and `mksfoex` when
  they are on PATH.
- AT3 tags: the RIFF tags ffmpeg maps to its ATRAC3 / ATRAC3plus decoders (a test checks them with ffprobe when
  it is installed).
