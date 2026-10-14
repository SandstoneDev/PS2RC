# Architecture

PS2RC turns a PS2 program into a PSP program in three stages: a translator written in
Python produces C, the PSP toolchain compiles it together with a runtime and an HLE
layer, and the result runs on the PSP as an ordinary homebrew EBOOT. The same C also
builds for the PC, which is where most debugging happens.

```
 disc image (yours)
   |
   |  ps2rc: load ELF + overlays, find functions, analyse widths and ABI use
   v
 generated C (one function per guest function, ~100 files)      <- work directory, never committed
   |
   |  prepare: rename hooked functions, safe points, trap set, zones, patches, dispatch table
   v
 psp-gcc + runtime/ + hle/core/ + hle/psp/ + audio/ + pack modules  ->  EBOOT.PBP + kernel helper (.prx)
 gcc     + runtime/ + hle/core/ + hle/host/ + audio/ + pack modules  ->  host runner (PC)
```

## 1. The translator (`ps2rc/`)

| Module | Role |
|---|---|
| `image.py` | Loads the boot ELF: loadable segments, BSS, `$gp`, symbols and relocations when present, overlay slots. |
| `overlay.py` | Reads Metrowerks overlay files (`MWo3`) and checks them against the ELF's slots. |
| `r5900/decode.py` | Decodes every R5900 instruction to operands, including MMI, COP1, COP2 macro mode and COP0. |
| `r5900/flow.py` | Control flow inside a function: delay slots, branch-likely, jump tables. |
| `r5900/semantics.py` | What each instruction reads and writes, for the analyses. |
| `analysis/functions.py` | Function discovery from the entry point, call targets, tail calls, data pointers and gaps; jump-table recovery. |
| `analysis/abi.py` | Finds functions that do not follow the calling convention; they are translated in exact mode. |
| `analysis/width.py` | Proves which 64/128-bit values only need 32 bits, and which arguments and returns must stay wide across calls. |
| `analysis/vucode.py` | Finds VU microcode embedded in the ELF so it is not translated as EE code. |
| `ir.py` | The per-function representation the analyses and the back-end share. |
| `backend_c.py` | Emits C: registers as locals where the analyses allow, write-back only where observable, the FPU and VU0 clamping rules, dispatch for indirect calls. |
| `vu/` | VU0 macro-mode translation; the VU0 microcode translator (`micro_gen.py`: microcode to C, timing resolved at translation time). |
| `iop/` | The IOP recompiler: IOP modules (IRX, MIPS R3000) to C, with its own runtime (`iop/rt`: kernel and library HLE). |
| `convert/` | The converter (in progress): disc image in, PSP game folder out, on the player's PC (ISO reader, pack identification, recipe, toolchain discovery, build, staging). |
| `movies/`, `pbp/` | Open movie tools (PSS demux, PSMF mux) and the EBOOT.PBP / PARAM.SFO packer with the player's own menu assets. |
| `oracle/` | Runs PCSX2 and reads its memory over PINE: the reference for bit-exact tests. |
| `config.py`, `pack.py` | Local paths (`local.toml`) and game packs (`pack.toml`). |
| `pipeline/` | The steps that turn a pack and a disc into a build: translate, prepare, trap set, layout, disc map, staging. |

Design rules:

- **One C function per guest function.** Direct calls are C calls, so the C compiler
  sees the whole call graph. Indirect calls go through a sorted dispatch table;
  unknown targets stop the run with a clear error.
- **Translate what the program does, not what the CPU does.** Values proven 32-bit
  are 32-bit; registers are C locals; only what another function can observe is
  written back to the guest context. Each such lever is a back-end option, tested
  bit-exact against the exact mode and measured on hardware.
- **Exactness where the game can see it.** The FPU and VU0 follow the R5900's
  non-IEEE rules (clamping of denormals and infinities, rounding toward zero) on
  every value the game reads back.

## 2. The runtime (`runtime/`)

`r5900_rt.h` defines the guest context (general registers as 128-bit values, HI/LO,
the FPU and VU0 registers and accumulators, COP0 state), the memory access functions
and the helpers the generated code calls (multimedia instructions on 32-bit lanes,
FPU clamping, syscalls, fatal errors). Guest RAM is one window: 32 MB on the PC, a
smaller window on the PSP (section 4). Plain accesses mask the address into the
window; accesses that can reach hardware registers go through trapping accessors,
used only in the functions of the trap set. The header is specialised per target
(PC, PSP, check builds) by the pipeline.

## 3. The HLE layer (`hle/core/`)

High-level emulation replaces what the PS2 has and the PSP does not, at the level of
functions rather than hardware:

- **Hooks.** A pack's hook list names guest functions by address. Their translated
  bodies are renamed to `fn_<addr>__guest`, and C written for them takes their place;
  it can call the original. Trace entries log calls without replacing them.
- **EE kernel.** Threads with strict priorities, semaphores, sleep and wake-up,
  alarms, interrupt and DMA handlers. Guest threads are coroutines inside one host
  thread; they switch only at calls into the kernel HLE and at safe points (loop
  back-edges), which is also where interrupts are delivered.
- **Virtual time.** EE cycles advance by fixed costs, never by the host clock, so a
  run is reproducible and the PC and the PSP reach the same milestones at the same
  times. On the PSP, waiting for the next VBlank waits for real time.
- **Syscalls, hardware registers, DMA.** The syscalls compiled games use; a model of
  the hardware registers (timers, interrupt controller, DMA controller, GS status)
  driven by virtual time.
- **Sony libraries, by name.** The functions games link statically (file I/O, CD/DVD,
  controller, memory card, SIF RPC, IOP module loading, GS synchronisation) are
  implemented once in C; a pack maps them to their addresses in its game.
- **IOP side.** RPC calls to the I/O processor are dispatched by server ID to C
  handlers. The standard Sony services are part of the core; a game's own modules
  (streaming, sound) are either translated by the IOP recompiler on the player's PC
  and bound by the pack, or modelled in the pack.
- **Disc.** Files are served from the user's extracted disc, sector reads by LSN with
  synthetic ISO9660 sectors, and optional same-size file overrides.
- **Memory card.** A folder per card, written crash-safely.
- **Instruments.** Profiler zones, call traces, a frame-time spike histogram, and
  logs written in a way that survives a crash.

## 4. The PSP platform layer (`hle/psp/`)

- **Boot and memory.** The program asks custom firmware for the large user
  partition of a PSP-2000/3000. The guest RAM window is allocated in one block with
  guard areas around it; the game's heap is sized by its pack; the guest's code
  region, which the translated program never reads, is reused for display lists,
  textures and vertex caches.
- **Coroutines.** Guest threads switch with a small hand-written assembly routine.
- **Media Engine.** A job interface for the second CPU, started through kubridge,
  used by packs for audio rendering and other work that can run in parallel.
- **GE.** Display-list helpers, texture swizzling and the frame flip that render HLE
  modules build on.
- **Input, audio output, movies.** The PSP controls mapped to a DualShock 2; a
  time-stretching audio output thread; a PSMF movie player.
- **Start-up credit screen.** A QR code to the community server and the credit
  line, drawn by the CPU before the GE starts; a build option.
- **Kernel helper module** (`psp_exc/`). A small kernel-mode module loaded at start:
  crash handler with a register dump, hardware performance counters for the
  profiler, and a sleep handler that keeps the Media Engine from hanging the console.

The host layer (`hle/host/`) implements the same interface with POSIX: coroutines,
a 32 MB window, files, a software rasteriser for frame dumps and audio capture.

## 5. Audio (`audio/`)

An SPU2 model (voices, ADPCM decoding, envelopes, interrupts, mixing) that a pack's
sound-driver model drives, and the output stage that resamples and time-stretches
the mix for the PSP's audio hardware.

## 6. Builds

`mk/build.sh` reads the pack, runs the translation (or reuses it), the prepare step,
and compiles. The PSP build uses psp-gcc from pspdev with `-O2 -G0 -std=gnu11
-frounding-math -ffp-contract=off` (the generated units also with
`-ffunction-sections` and a section-ordering file for the code layout) and the
large-memory flag, links PSPSDK libraries, and produces `EBOOT.PBP` and the kernel
helper module. The host build uses the PC's gcc at `-O1` and links with `-rdynamic`,
so a crash backtrace names the guest functions. Staging copies the EBOOT, the
configuration file, the helper module and a disc map into a folder for `PSP/GAME/`;
the disc data itself is read from the user's copy.

The generated C is split into units of 150 functions plus one file with the
dispatch table; on the PSP and the PC the HLE puts a direct-mapped cache in front of
the table's binary search.

## 7. Tests

- Pure Python tests of the decoder, the analyses and the back-end, using synthetic
  R5900 programs assembled in the test itself.
- Tests that compile generated C with the native gcc and with psp-gcc (marker `wsl`).
- Bit-exact tests against PCSX2 (marker `oracle`), with committed golden results so
  they can run without it.
- Tests in PPSSPP or on a PSP (marker `psp`).
- The example pack, built and run end to end.

Tests that need a game's files belong to that game's pack and are marked
`needs_game`.
