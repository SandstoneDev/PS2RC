# Game pack format

A game pack is everything PS2RC needs to know about **one version of one game**: how
to recognise the disc, where the code is, which functions to replace, and the C
modules that replace them. It lives in `games/<serial>_v<version>/`, for example
`games/SLUS-12345_v1.00/`. `games/_template/` is an empty pack with every key
commented; `games/example/` is a complete pack for a small test program.

A pack contains **no game data**: no files from the disc, no code, text or tables
copied from the game, and nothing translated from it. It describes the game (sizes,
addresses, checksums) and holds code its author wrote. Everything derived from the
game is produced on the user's machine, in the work directory.

## Files

| File | What it holds |
|---|---|
| `pack.toml` | identity, verification, memory map, overlays, translation and build options (below) |
| hook list | the guest functions the HLE replaces (HOOK) or logs (TRACE), by address and neutral label, including the Sony library functions found by the signature scan |
| zone list | optional profiler zones on guest functions |
| text patches | optional small patches to the generated C (for example the game's heap size) |
| lists | optional inputs computed once and kept with the pack, so the user's machine does not have to recompute them: the trap set, a code-layout order, the functions with native C versions |
| `src/` | the pack's own C modules: the game's render HLE, its sound-driver model, VU0 micro programs, game-specific hooks |
| `tests/` | the pack's own tests; tests that need the user's game files are marked `needs_game` |

`pack.toml` names the files it uses, so their exact names are up to the pack; the
template uses the defaults the build scripts expect.

## pack.toml

A TOML file with `format = 1` and these tables. Paths are strings: `{work}/x` is in
the work directory (from `local.toml`), `{disc}/x` is in this pack's extracted disc,
`{repo}/x` is in the checkout, and a plain relative path is inside the pack.

```toml
format = 1

[id]
serial = "SLUS-12345"            # from the disc's SYSTEM.CNF
version = "1.00"                 # SYSTEM.CNF VER
region = "NTSC-U"
boot_elf = "SLUS_123.45"         # the boot program's file name on the disc
psp_title = "My game (PS2RC)"    # title in the PSP's menu; the build adds its number

[verify]
system_cnf_boot2 = 'cdrom0:\SLUS_123.45;1'
elf_size = 0                     # size and SHA-256 of the boot program: a different
elf_sha256 = ""                  # disc revision is refused with a clear message

[memory]
code = [0, 0]                    # the translated code range [start, end)
ctor_code = [0, 0]               # static-constructor code outside it, if any
data_end = 0                     # end of the program's file data
bss_end = 0                      # end of BSS

[overlays]
scheme = "mwo3"                  # Metrowerks overlays; omit the table if there are none
window = [0, 0]                  # the address range overlays load into
files = []                       # overlay files on the disc

[translate]
# inputs and options of the translation (analysis caches in {work})

[build]
# translation variant, hook and zone lists, text patches, layout, output folder

[run]
# disc map, file overrides served instead of the disc's own (same size)
```

Use `python ps2rc/pack.py get <pack> <key>` to see a value as the build scripts see it.

## Hooks

The hook list is the contract between the translated game and the HLE:

- `HOOK(addr, label)`: the translated function at `addr` is renamed to
  `fn_<addr>__guest`; every call lands in the C function the pack or the core defines
  for it, which may call the original.
- `TRACE(addr, label, flags)`: the original runs, wrapped with a call log; flag 1 logs
  the first call as a boot milestone.
- `HOOK_OV` and `TRACE_OV` do the same inside an overlay (with the overlay's id).

Sony library functions are implemented once in the core, by name. The pack only says
where each one is in this game's program. Labels for the game's own functions are
neutral descriptions of what the function does, never names taken from the game.

Translate again whenever the hook list changes: hooked functions get different
argument rules in the translation.

## Pack C modules

Modules in `src/` are compiled into the EBOOT with the core. They implement the hooks
the core does not: the game's renderer boundary, its sound-driver model, its VU0 micro
programs, and fixes or speed-ups for single functions. Keep them neutral in names and
comments, and remember that a hand translation of the game's own code is derived
from the game: it may stay in your local pack, but it must not be published.

## Local configuration

`local.toml` at the repository root (never committed) tells PS2RC where things are
on your machine:

```toml
[paths]
work = "~/ps2rc-work"            # everything derived from games

[games."SLUS-12345_v1.00"]
disc = "~/discs/my-game"         # the extracted disc of this pack

[defaults]
pack = "SLUS-12345_v1.00"
```

Environment variables override it (see `ps2rc/config.py`).

## Checklist for a new pack

1. Copy `games/_template/` and fill `[id]` and `[verify]` from your disc.
2. Fill `[memory]` and `[overlays]` from the boot program (PORTING_GUIDE, section 2).
3. Run the signature scan and add the Sony library hooks.
4. Translate, build for the host, and boot to the first blocker
   (PORTING_GUIDE, section 7).
5. Add hooks and modules until the game reaches its main loop on the host, then on
   the PSP.
6. Before publishing: the pack contains no game data and no game names, and your
   README carries the credit line.
