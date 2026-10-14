# PS2RC

> **Read [LICENSE](LICENSE) before you use the code.** This is free, source-available
> software. You may use it for free and fork it to contribute back; publishing your own
> builds or releases of this game's port is not allowed (see [Forks](#forks-and-other-games)).
> **No game data is included here**, and none may ever be added: you bring a disc you legally own.

PS2RC is a **converter**: it turns a PlayStation 2 game disc that you own into a game folder for the
**Sony PSP**, entirely on your own PC. Under the hood it is a static recompiler: it translates the game's
PS2 program into C, builds it with the open-source PSP toolchain together with a runtime that stands in for
the parts of the PS2 the PSP does not have, and stages the result for a PSP with custom firmware.
PS2RC is developed as a converter for the PS2 version of Grand Theft Auto: San Andreas (PAL disc SLES-52541, version 2.01).
It is an independent project, see [Not affiliated](#not-affiliated).

```
your disc image -> PS2RC converter (on your PC) -> PSP/GAME/<folder> -> PSP (custom firmware)
```

## Status of this repository

| Part | Here |
|---|---|
| Converter, command line (`python -m ps2rc.convert`): identify the disc, translate, build, stage, check and repair a PSP folder | yes |
| Converter window (`gui/`, Qt for Python): disc -> PSP folder wizard, menu icon / pictures / music of your choice, settings | yes (source; `gui/build_app.py` builds the Windows app folder) |
| Recompiler (R5900 to C), IOP module recompiler, VU0 microcode translator, runtime, PSP platform layer | yes |
| Example pack (`games/example`): a small PS2 program written for this repository | yes |
| **Game pack for the disc above** (in `games/`: the per-game part the converter needs; its README has the controls, the settings and what differs from the developer build) | yes |

Nothing made from a game is, or will be, in this repository or its releases: no game files, no translated
code, no EBOOT built from a game. Everything made from the game is generated on the player's PC from the
player's own disc and never leaves it.

## What you need

- A disc image made from **a disc you legally own** (for the supported disc: the PAL disc named above, version
  2.01, as an ISO made with a PC drive).
- A **PSP-2000 or PSP-3000** (64 MB of RAM) with custom firmware (ARK): the runtime uses the large user memory
  partition, a small kernel-mode helper module and the Media Engine. Other models: Unknown. About 4 GB free on
  the memory stick.
- A **64-bit Windows 10 (1809 or newer) or Windows 11** PC for the ready-made converter. From source the
  converter also runs on Linux and macOS with the pspdev toolchain.

## Convert your disc (ready-made converter, nothing to install)

1. Download the converter archive from the Releases page of this repository and unpack it anywhere (the path may
   contain spaces; keep the folder together).
2. Double-click **PS2RC Converter.exe**. Pick your disc image, pick the memory stick (or an output folder),
   optionally your own menu icon / pictures / music, then **Convert**. About 10 minutes on a 12-thread PC.
3. If you converted into a folder: copy the converted folder (its name is shown at the end) to `PSP/GAME/` on the memory stick. Start it from the PSP
   menu.

The command line of the same folder: `ps2rc-convert.cmd <your disc image> --out <folder>` (options:
`ps2rc-convert.cmd --help`). The game pack's README (in `games/`) explains the controls and the settings
file. Everything the converter makes stays on your PC: never share the folder it makes.

## Build from source

Windows 10/11 x64 (the commands are for the Windows command prompt; on Linux / macOS use `python3`):

1. Install **Python 3.11 or newer** (64-bit, python.org; tick "Add python.exe to PATH") and **git**.
2. Get the code and the Python packages:

   ```
   git clone https://github.com/SandstoneDev/PS2RC.git
   cd PS2RC
   python -m pip install pyelftools pillow pytest
   ```

3. Get a **PSP toolchain** (psp-gcc 15.2 with PSPSDK). One of:
   - the `toolchain` folder of the ready-made converter download above (a native Windows build): pass it with
     `--toolchain <folder>\toolchain` or set the environment variable `PS2RC_TOOLCHAIN`;
   - WSL2 with the official pspdev toolchain installed in `~/pspdev` of a WSL distribution
     ([pspdev install guide](https://pspdev.github.io/installation/windows.html)): the converter finds it there
     by itself;
   - build the native Windows toolchain yourself with the scripts in `tools/toolchain/` (MSYS2; see the comments
     at the top of `tools/toolchain/build_windows.sh`).
4. Optional: [ffmpeg](https://ffmpeg.org/) for the intro movies (`--ffmpeg <ffmpeg.exe>` or on PATH; without it
   the movies are skipped).
5. Check and convert:

   ```
   python -m ps2rc doctor --quick
   python -m pytest -q -m "not wsl and not oracle and not psp and not slow and not needs_game"
   python -m ps2rc.convert "<your disc image>" --out "<output folder>" --toolchain "<converter folder>\toolchain"
   ```

   `doctor` also lists what the developer pipeline would need (`local.toml`, an extracted disc folder, PPSSPP,
   PCSX2); the converter needs none of these. `--dry-run` prints the plan and writes nothing; `python -m ps2rc.convert --help` lists every option.
   `python -m ps2rc.convert verify <PSP folder>` checks a converted folder later; `update` repairs it and keeps
   your settings and saves.

The converter window from source: `python -m pip install --require-hashes -r gui/build_requirements_qt.txt`,
then `python gui/run_gui.py`. The ready-made folder itself is made by `python gui/build_app.py` (see the top of
that file).

The example pack goes through the whole pipeline without any game data (needs bash and a native gcc for the
host build):

```bash
bash mk/build.sh --pack example host      # translate + build for the PC
bash mk/run_host.sh --pack example        # run it on the PC
bash mk/build.sh --pack example psp       # build EBOOT.PBP for the PSP
```

## Layout

| Path | What it is |
|---|---|
| `ps2rc/` | the translator and pipeline (Python), the converter (`convert/`), the IOP recompiler (`iop/`), the VU0 microcode translator (`vu/`), movie tools (`movies/`), the EBOOT packer (`pbp/`), menu artwork helpers (`xmb/`) |
| `gui/` | the converter window and the script that builds the Windows app folder |
| `runtime/` | the runtime the generated C is compiled against |
| `hle/core/` | EE kernel, syscalls, hardware registers, Sony-library HLE, IOP RPC framework, disc and memory-card layers |
| `hle/host/`, `hle/psp/` | the PC and PSP platform layers |
| `audio/` | SPU2 model and audio output stage |
| `psp_exc/` | kernel-mode helper module for the PSP (crash handler, counters) |
| `mk/` | build and run scripts |
| `tools/` | analysis tools for PS2 ELF files, development tools, PSP toolchain scripts |
| `games/` | game packs: the disc above, `_template` and `example` |
| `tests/` | tests that need no game data |
| `docs/` | [architecture](docs/ARCHITECTURE.md), [porting guide](docs/PORTING_GUIDE.md), [game pack format](docs/GAME_PACK_FORMAT.md) |

## Porting another game

PS2RC's recompiler is game-neutral. The toolkit for porting other PS2 games, with an agent kit (instructions,
skills and field notes for an AI coding agent), is
[universal-ps2-recompiler](https://github.com/SandstoneDev/universal-ps2-recompiler). For general PSP
development see [psp-developer](https://github.com/SandstoneDev/psp-developer).

## Forks and other games

The game this repository converts is **reserved** (the pack's `RESERVED.md` in `games/`; LICENSE, section
2(e)). In short (the [LICENSE](LICENSE) has the exact terms):

- you may fork this repository on GitHub **only to contribute back** (pull requests);
- you may **not** publish your own releases or builds of a port of this game, or a modified game pack for it,
  anywhere;
- to port **other** games, use [universal-ps2-recompiler](https://github.com/SandstoneDev/universal-ps2-recompiler):
  free, under the same licence.

For work on other games, under the licence:

- everything you distribute must be **free**: no selling, renting, paywalls or
  subscriptions. Voluntary donations are fine as long as they gate nothing (no
  feature, build, download or early access tied to them);
- your README and every release page must carry this credit:

  ```
  Based on PS2RC by SandstoneDev
  https://github.com/SandstoneDev/PS2RC
  Community: https://discord.gg/wV8x46ZTDu
  ```

- the PS2RC code in your fork stays under the PS2RC License; keep `LICENSE` and
  `NOTICE.md` with your source;
- **never distribute game data**: no game files, no disc images, and nothing
  translated or built from a game (generated C, EBOOTs of a commercial game). Every
  user builds from their own disc;
- a game that SandstoneDev has declared reserved (a `RESERVED.md` notice in a
  SandstoneDev repository, or on the community server) may not be ported in a
  distributed fork.

If you break the terms, your permission ends; SandstoneDev may restore it in writing. The start-up credit screen
is a build option; you may keep, change or remove it.

## Community

**Join the community** on Discord: [discord.gg/wV8x46ZTDu](https://discord.gg/wV8x46ZTDu) - questions, progress
reports, help with the converter. Please never post game files, disc images or converted folders there.
Contributions: read [CONTRIBUTING.md](CONTRIBUTING.md). Security problems: see
[SECURITY.md](SECURITY.md). Other contact: GitHub issues, the Discord, or
sandstonedevpsp@gmail.com.

**Takedown policy.** Rightsholders can reach the maintainer through GitHub issues,
the Discord or sandstonedevpsp@gmail.com. On a takedown or removal notice the named
material is removed promptly and not published again (no re-uploads or mirrors);
GitHub's DMCA process applies as usual. Details: [TAKEDOWN.md](TAKEDOWN.md).

## Not affiliated

This project is not affiliated with, authorized or endorsed by Rockstar Games, Take-Two Interactive or Sony Interactive Entertainment.
The game's title is used only to say which disc the converter is made for; all trademarks belong to their owners.
"PlayStation", "PS2" and "PSP" are trademarks of Sony Interactive Entertainment Inc.
