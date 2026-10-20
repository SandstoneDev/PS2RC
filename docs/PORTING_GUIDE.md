# Porting a PS2 game with PS2RC

This guide is the method that took one commercial 3D PS2 game from its disc to a
playable build on a PSP-3000. It is written for the next game. Read
[ARCHITECTURE.md](ARCHITECTURE.md) first for the vocabulary, and
[GAME_PACK_FORMAT.md](GAME_PACK_FORMAT.md) for the files you will write.

Everything that comes from your game (its program, its files, the C that PS2RC
generates from it, the EBOOT you build) stays on your machine. Nothing of it may be
committed, uploaded or shared. Your fork contains your pack and your code, nothing
else.

Today the pipeline is a set of scripts driven by a game pack. The direction is a PC
converter with a graphical interface, a command line and a bundled toolchain, in
which the player's own PC generates the game's code (and any module made from the
game) from the player's own disc. Write your pack with that in mind: everything
derived from the game must be reproducible on the player's machine from their disc.

## 0. What to expect

| Phase | What happens | Typical effort |
|---|---|---|
| Identify and analyse | boot program, overlays, IOP modules, Sony library names | days |
| Translate and boot on the PC | the host build reaches the game's main loop | days to weeks |
| Replace what the PSP lacks | rendering (by hand); audio and VU0 micro programs (generators plus pack glue); movies | weeks to months |
| Fit and speed up on the PSP | memory budget, profiling on hardware | open-ended |

The translator, the runtime, the EE kernel and library HLE, the IOP recompiler and the
VU0 microcode translator are generic; the generators run on the player's PC and the
pack binds their output to the game. Rendering is written per game or per engine. A game built on an
engine that someone has already covered is much less work than a new one. The
reference port runs at about 13 to 16 frames per second on a PSP-3000 at 333 MHz
(the game's own cap is 25): the translated game logic alone takes about 40 ms per
frame. Plan for a game that is slower than the original unless it does much less work
per frame than the PSP can afford.

## 1. Set up

1. Install the PSP toolchain ([pspdev](https://github.com/pspdev/pspdev)), a native
   gcc, Python 3.11 or newer and pytest. On Windows use WSL2; a native Windows psp-gcc
   was not tried.
2. Create `local.toml` at the repository root (it is ignored by git). It holds the
   paths of your machine: the work directory for everything derived from the game
   (translations, caches, builds: plan for 10 GB) and the folder with your game's
   extracted disc. See the comment at the top of `ps2rc/config.py`.
3. Run the tests and build the example pack (README, "Quick start"). If the example
   does not build and run, nothing else will.
4. Copy `games/_template/` to `games/<serial>_v<version>/` for your game and fill it
   in as you go. A pack describes exactly one version of one game: a different disc
   revision is a different pack.

## 2. Identify the game

1. **Make a disc image of your own disc** with a PC DVD drive, and extract its files.
   Keep the ISO9660 directory with each file's logical sector number (LSN) and size:
   the runtime serves sector reads by LSN, and some games read the directory
   themselves.
2. **Read `SYSTEM.CNF`.** `BOOT2` names the boot program, `VER` the revision,
   `VMODE` the video mode. Record the boot program's size and SHA-256 in the pack's
   `[verify]` table. Note PAL (50 Hz) or NTSC (60 Hz) now: the VBlank rate drives
   the game's clock, and getting it wrong changes the game's speed.
3. **Load the boot program** (`ps2rc/image.py`). It reads the loadable segments
   (zero-filling BSS), `$gp` from `.reginfo`, and symbols and relocations if the
   program was not stripped. Write the code range, the end of the file data and the
   end of BSS into the pack's `[memory]` table.
4. **Find the overlays.** Some games load extra code into a fixed window at run time.
   `image.py` reports overlay slots: loadable segments without file data that share
   one address. PS2RC reads the Metrowerks overlay format (`ps2rc/overlay.py`): a
   header with the magic `MWo3`, an id, the load address, the section sizes and a
   static-constructor table. Overlays are linked to one address and carry no
   relocations; the main program calls into the window directly. Two pitfalls:
   - an overlay's static constructors can sit after its `.text`; find them through
     the constructor table, or they are never translated;
   - an overlay linked for another revision of the game looks plausible and is
     wrong; the loader checks every overlay against its slot.

   Games that use another overlay scheme need a new loader.
5. **List the IOP side.** The I/O processor runs modules (IRX files, sometimes
   packed in a replacement image) that the EE talks to through SIF RPC. Sony's
   standard modules are covered by the library HLE. Every custom module of the game
   (streaming, sound) is an RPC server you will model in C (section 8).
6. **Check for self-modifying code.** Run the game in PCSX2 and compare its code
   region in memory with the program file during boot and play. In the reference
   game they were identical, including the overlay window after each load. If your
   game writes code, static translation needs extra work there.

## 3. First look: the analysis tools

The tools in `tools/ps2elf/` take the boot program (and overlays) as arguments:

| Tool | Answers |
|---|---|
| `census.py` | Where is the code? Which R5900 instructions, syscalls and COP0 operations does it use? |
| `idioms.py` | Which 64/128-bit instructions are compiler idioms (moves, sign extension, register saves) and which do real wide arithmetic? |
| `abi_probe.py` | Per function: registers read before written, callee-saved saves and their width, FPU arguments, return registers. |
| `hwaccess.py` | Which functions touch hardware registers, VU memory, the scratchpad or uncached mirrors, by region. |
| `xref_addr.py` | Who references an address, as data pointers and as LUI/ADDIU pairs. |
| `find_vu.py`, `vu_programs.py`, `vudis.py` | Where is VU microcode, which programs are uploaded, what do they do. |
| `sce_sigscan.py` | Which functions are Sony library functions (section 4). |

Typical result for a compiled game: most "exotic" R5900 code is compiler idioms
(nearly all 64-bit adds are register moves, most 128-bit loads and stores are
register saves), real 64/128-bit work is a few percent of the functions, and only a
few dozen functions touch hardware directly. That is what makes a translation to
32-bit C practical.

## 4. Name the Sony library functions

Games link Sony's libraries statically, so their functions are in the boot program
without names. `sce_sigscan.py` finds them: for each candidate it blanks the bytes
that relocations change, hashes the rest and looks the hash up in a signature
database. Only matches with enough fixed bits and a single possible name are
accepted. A second pass uses the relocations recorded in the database to name the
call targets inside matched functions; accept only names that every matching record
agrees on, and cross-check syscall wrappers against the syscall numbers they issue.

PS2RC does not ship a signature database. You supply one; check its licence before
you redistribute anything made from it (names are facts, the database is not).

The reference game had about 670 library functions identified this way. They are
what the HLE binds to by name (section 8).

## 5. Find the functions

The translator needs every function's extent. `ps2rc/analysis/functions.py` starts
from the entry point and all direct call targets, adds jump targets (with this
compiler, `j` is only used for tail calls), data words that point at plausible
function starts, and well-formed code in the gaps. Strong starts (entry, call
targets) cut a function; weak starts (tail calls, data pointers, gaps) do not, so
functions may share code.

**Jump tables** are recovered by evaluating the table address backwards along the
only path to the `jr`, and the bound from the guard compare. A missed table shows up
later as "functions" made of case labels that end in a fatal dispatch error at run
time.

**Non-ABI functions** (`ps2rc/analysis/abi.py`) are the few that break the calling
convention: code entered by `j` that reads registers the caller set, functions that
leave callee-saved registers changed, exception handlers that end in `eret`, thunks
that take their target in `t9`, code that uses the return address as data. They are
translated in exact mode, with every register synchronised at every transfer. In the
reference game this was 12 functions out of about 12,700.

**Width analysis** (`ps2rc/analysis/width.py`) proves which 64/128-bit operations only
need 32 bits. In the reference game that was about 97 percent of them.

Lessons from real boots, each of which cost a debugging session:

- A `syscall` reads `a0`-`a3`, `t0`-`t3` and `v1`; treat them all as live.
- 64-bit values that pass through wrappers (a double returned by a tail call, a
  64-bit timer) must stay wide across the call. PS2RC computes a whole-program
  fixpoint of wide arguments and returns, including between overlays and the main
  program. When it was missing, objects were placed at the origin and a 32-bit clock
  wrapped.
- Functions you replace with HLE get full argument liveness: your C may read more
  arguments than the original did. Translate again whenever the hook list changes.

## 6. Translate

The translator decodes, analyses and emits one C function per guest function. Each
guest register lives in a C local where the analyses allow it and is written back to
the context only when observable. The back-end has levers, each measured on hardware
before it became a default:

| Lever | What it does |
|---|---|
| ABI mode | only registers the calling convention makes observable are synchronised at calls |
| canonical locals | 32-bit locals for values proven sign-extended |
| clamp elision | FPU operand clamping only where a value can be denormal or infinite |
| 128-bit locals | quadword loads, stores and moves on locals instead of the context |
| dead save removal | callee-saved save/restore pairs proven useless are dropped |
| mask-base | memory accesses mask the base register once; needs guard areas around the guest window and a host check run with zero escapes |
| layout | never-executed functions marked cold; hot call chains placed together |

Control flow: delay slots run after the branch condition is evaluated (only on the
taken path for branch-likely); `jal` is a C call; `jalr` goes through a sorted
dispatch table; a recovered jump table becomes a `switch`; calls into an overlay
window go through the table of the overlay currently loaded, which the HLE switches
when the game's overlay loader finishes reading the file.

**The FPU is not IEEE.** Operands with exponent 0 are zero, operands with exponent
255 are the largest finite value, results are rounded toward zero and flushed,
division by zero gives the largest value, and multiply-add rounds the product first.
The PSP's FPU runs in round-toward-zero with flush (FCR31 = 0x01000001), but it does
use denormal inputs, so operand clamping cannot be dropped. Build with
`-frounding-math -ffp-contract=off`, or GCC folds constants with the wrong rounding.
Check PCSX2's game database entry for your game: its clamp and rounding settings tell
you what the game depends on.

**VU0 macro mode** (COP2 instructions in EE code) is translated to C with the same
clamping and rounding. The MAC, status and clip flags are not modelled: check that
your game does not read them. The PSP's VFPU cannot reproduce VU0 results (its
rounding and dot products differ), so do not use it for values the game reads back.

Numbers from the reference game: about 5 MB of guest code became about 12 MB of
Allegrex code (2.3 times the guest size with all levers), 66 to 80 MB of C in about
110 files. Translation takes 2 to 5 minutes; compiling with psp-gcc at `-j6` took
between 90 s and 15 minutes depending on the machine's load (median about 150 s).

## 7. Boot on the PC first

The host build compiles the same generated C and the same HLE with the PC's gcc and
runs the game in a 32 MB window. It finds most problems in seconds instead of a
round trip to the PSP, and a backtrace of a host crash is the guest call chain.

**The prepare step** (between translation and compilation) renames hooked functions,
inserts a safe point at every loop back-edge (without it, polling loops never see
interrupts), applies the trap set, the profiler zones and small text patches, and
generates the dispatch table.

**Virtual time.** The game never sees the host clock. Time advances by fixed costs
(a syscall, a hardware register access, a back-edge, a file read by size) and jumps
to the next event when every guest thread waits: VBlank start and end at 50 or
60 Hz, alarms. Runs are bit-reproducible, and the PSP reaches the same milestones at
the same virtual times as the PC.

**Boot to the first blocker.** Run strict. Read the fatal line and its call chain.
Add a named experiment switch that steps over the blocker (for example answering
every VU0 micro-program call with a neutral result) to see the next three blockers;
fix them; then implement the real thing and remove the switch. Unknown RPC servers,
unknown syscalls and unexpected hardware accesses are fatal by design: that is how
you find what is not replaced yet.

**The trap set.** Only functions that access hardware through computed pointers
get trapping memory accessors; everything else uses plain masked accesses. The set is
the static hardware-access scan plus every function a host run with the MMIO profile
saw touching hardware.

**Gates.** After every change: the game reaches a fixed milestone (for example "first
main-loop iteration at virtual time T, frame N"), and the checks report zero: no
plain accessor saw a non-RAM address, no masked access escaped the window, nothing
wrote into the code region. Two runs of the same build must be guest-identical.

The host runner's options include a time limit, a main-loop iteration limit, an MMIO
profile, an alias report, disc usage, PC sampling, address watches, frame dumps from
a software rasteriser, audio capture, scripted controller input and a memory-card
directory. Exit codes separate a reached limit from a fatal guest event, a deadlock,
a host signal and a watchdog stop.

## 8. Replace what the PSP does not have: HLE by name

A hook replaces one guest function with C. The hook list maps addresses to names;
the prepare step renames the translated original to `fn_<addr>__guest`, so every call
(direct or through a dispatch table) lands in your C, which can still call the
original. Trace entries wrap a function with a call log instead, which is how you
mark boot milestones. A test checks that every hook has a definition.

**Sony library functions** are bound by name: the core implements `sceSifCallRpc`,
`sceCdRead`, `scePadRead` and the rest once, and your pack maps each name to the
address the signature scan found. About 120 hooks in the reference game were library
functions.

What the core HLE provides:

- **EE kernel**: threads with strict priorities and no time slicing (a ready thread
  of higher priority preempts; equal priorities do not rotate on their own),
  semaphores, sleep and wake-up with counts, suspend and resume, alarms, interrupt
  handlers for the INTC and DMAC delivered at safe points in their own register
  context. Guest threads run as coroutines inside one PSP thread.
- **Syscalls**: the ones compiled games use (threads, semaphores, interrupt and DMA
  handlers, SIF DMA, caches, system configuration, GS registers). Check the video-mode
  constants of the GS reset call against your game: swapping PAL and NTSC made the
  reference game run a quarter slower.
- **Hardware registers and DMA**: transfers complete at once; scratchpad DMA moves
  data; VIF, GIF and IPU data is dropped unless an HLE layer consumes it; timers and
  the interrupt controller run on virtual time.
- **SIF RPC**: binding creates server records, calls are dispatched by server ID to C
  models, end functions run in interrupt context. Unknown server IDs stop the run.
- **Files and disc**: file I/O on the extracted disc (case-insensitive), the CD/DVD
  control calls, sector reads by LSN with synthetic ISO9660 sectors consistent with
  the real layout. A file you override must keep its original size, so the sector
  map stays valid. On the PSP every file open costs tens of milliseconds: files stay
  open in a small cache, and an optional reader thread makes reads asynchronous.
- **Controller**: one DualShock 2 in analog mode from the PSP's controls, with button
  combinations for the buttons the PSP lacks, and scripted input for tests.
- **Memory card**: a folder on the memory stick, written crash-safely, compatible
  with PCSX2's folder memory cards.

## 9. Rendering

The PS2 draws through the GS, fed by the VIF and the VU1, which the PSP does not
have. The approach that worked: **replace the game's renderer at its lowest stable
software boundary**, usually the middleware's device driver, and draw with the PSP's
GE. Keep the engine and all of the game's render logic translated.

Rejected alternatives, and why:

- emulating the VU1: the microcode depends on exact pipeline latencies, and the CPU
  would do all transform and lighting;
- translating GIF packets to GE commands: they are produced by the VU1, so this still
  needs the VU1;
- replacing higher up, at the game's own render functions: many entry points, and the
  game's exact behaviour is lost.

A driver boundary typically covers raster (texture) creation, locking and
destruction, native texture reading, camera begin, end and clear, the frame flip,
render state, the immediate-mode 2D and 3D calls, and the per-mesh pipeline
callback. Games add their own fast paths beside the middleware (custom batchers,
post effects, water, mirrors, a defragmenter that moves textures); each needs a hook.

Order of work: hooks that only log; a software rasteriser on the host whose frames
you compare with PCSX2 screenshots; the GE path for 2D; then 3D. Keep the translated
driver code running behind your hooks until the GE path is validated, so the guest
timeline does not change; then skip it with a switch (it saved about 10 ms per frame).

GE facts that shape the design:

- 2 MB of VRAM, a 16-bit depth buffer (expect z-fighting in large scenes), textures up
  to 512 x 512, an 8 KB texture cache.
- No clipping in X and Y: keep geometry inside the guard band and clip the rest on
  the CPU.
- Fixed blend factors: blends that use destination alpha are approximations.
- Low-precision matrices: compute camera-relative transforms on the CPU in float.
- Write the data cache back before a display list references new data. GE state
  persists between lists. A malformed list hangs the console.
- Convert textures to the PSP's swizzled indexed formats at load or first use, never
  larger than the PS2 original. Convert mesh data from the VIF unpack streams to GE
  vertex blocks once per mesh and cache them.

In the reference port the GE needed about 1.4 ms per frame for about 10,000
triangles. The CPU, not the GE, was the limit.

## 10. Audio

The game's sound driver runs on the IOP. The EE side of the game sends it commands
through SIF RPC and usually reads state back every frame (which banks are loaded,
play positions), so whatever stands in for the driver must answer exactly and in step
with virtual time.

What PS2RC provides:

- the **IOP recompiler** (`ps2rc/iop`): it translates an IOP module (IRX, MIPS R3000)
  to C for the IOP runtime in `ps2rc/iop/rt` (kernel and library HLE: threads, event
  flags, SIF RPC, timers, the sound library's register interface). Like the main
  translator it runs on the player's PC, on the player's own disc
  (`python -m ps2rc.iop gen MODULE.IRX -o OUT.c --sym NAME`); the output is never
  distributed. Your pack binds the result: which RPC server it answers, its memory,
  and its connection to the SPU2 model;
- an SPU2 model (48 voices, 2 MB of sound RAM, PS-ADPCM, ADSR envelopes, IRQ, 48 kHz
  mixing), an output stage on the PSP that time-stretches the mix when the game runs
  slower than real time, and a Media Engine path.

The alternative, used before the recompiler existed, is a hand-written model of the
driver's commands; it is code derived from the game and stays in your local pack.
Either way, validate the replies field by field against PCSX2, and keep fingerprints
of the replies in the log so the host and the PSP can be compared.

Moving the sample rendering to the **Media Engine** saved about 6 ms per frame in the
reference port: the main CPU keeps the state machine (so the replies stay exact) and
the ME renders samples from a command ring. Rules for ME code:

- start it through kubridge;
- its caches are not coherent with the main CPU: write back, synchronise, then
  publish; keep small flags in uncached memory;
- code that runs on the ME must not call into libc;
- putting the PSP to sleep while the ME runs your code hangs or powers off the
  console unless the sleep handler is replaced (the kernel helper module does this);
- taking over the ME stops the firmware's video and ATRAC decoders: start it after
  the movies.

Mixing only the loudest voices is a cheap speed lever, as long as quiet voices keep
their observable state exact.

## 11. VU0 micro programs

Games upload small programs to the VU0 and start them from EE code. The microcode
translator (`ps2rc/vu/micro_gen.py`) turns such a program into C that runs it to
completion when the game starts it: it follows the DMA chain that uploads the program,
resolves the pipeline timing at translation time (flag reads see the result of the
instruction issued four cycles earlier, division results arrive after a fixed
latency), and writes one C function per entry point plus a check that the uploaded
code is the expected one. Like everything made from the game, it runs on the player's
PC from their own disc (`python -m ps2rc.vu.micro_gen --elf FILE --chain ADDR
--entries A,B --sym NAME -o OUT.c`); your pack names the chain address and the entry
points.

The translator refuses what it does not support (VU data memory, XGKICK, the EFU,
some flag instructions) instead of guessing; such a program still needs a
hand-written replacement, which is code derived from the game and stays in your local
pack. Check that the EE code does not touch the VU0 between the start and the read of
the results, and verify against PCSX2's VU0 interpreter with vectors that cover every
branch path.

## 12. Movies

PS2 movies (PSS files: MPEG-2 video with a separate audio stream) cannot be played
by the PSP directly. The PSP plays PSMF (H.264 with ATRAC3plus audio) through its
firmware decoder; PS2RC includes a player for it. Free tools can encode the video, but
no free ATRAC3plus encoder is known, and the encoders used so far are proprietary.
For a public port, skip the movies (the default) until an open path exists.

## 13. Fit into 64 MB

On a PSP-2000/3000 in large-memory mode about 51 MB is available to the program. The
guest RAM window, the translated code, the HLE buffers and the game's heap must share
it. What made the reference game fit:

1. **Smaller code.** Every back-end lever that removes code also frees memory; the
   whole-program code shrank from about 20 MB to 12 MB.
2. **A smaller game heap.** Many games compute "end of RAM minus end of BSS minus
   stack" at start-up and take it all with one allocation. Patch that one constant in
   the generated C to the runtime's RAM end, and reduce the game's streaming budget
   through its own configuration file (served as a same-size replacement). Size it
   from a PCSX2 measurement: resident data plus streaming budget plus 1.6 to 2 MB of
   headroom. Too little causes missing geometry or an allocator that evicts and
   reloads the same data every frame (on the PSP, giving back 1.5 MB to the budget
   saved 7.5 ms per frame).
3. **The code hole.** The game never reads its own code region as data (verify it
   statically, and with PCSX2 memory breakpoints over boot and play), and the
   translated program does not need the original code. That region is reused for
   display lists, the texture arena and the vertex cache; a debug build checks it
   stays untouched.

Smaller items: guest threads are coroutines whose stacks are carved from the game
thread's own stack (stacks placed elsewhere crashed on hardware), the C library heap
is small and fixed, and the movie player's memory is released after the movies.

**Custom firmware** is required: for the large-memory mode, kubridge, the Media
Engine, and the kernel helper module (crash handler, hardware counters, sleep hook).
Load kernel-mode code through the custom firmware's interfaces; loading your own
kernel module into the kernel partition from user mode froze a PSP.

## 14. Run on the PSP and profile

**Profiler zones** wrap any guest function (by address, in your pack's zone list)
with a timer; HLE code uses scoped zones. Time is accounted per caller-callee edge
and per guest thread. Output: a summary every 10 s, final tables in the session log,
and an optional per-iteration dump that a tool turns into percentiles, the slowest
frames and a trace for a timeline viewer.

**Hardware counters** (through the kernel helper module) report CPI, memory and FPU
stall shares, and instruction and data cache misses, with a verdict: memory-bound or
instruction-bound. Do not enable them in an emulator.

What the reference port learned:

- The CPU is the limit, and it is memory-bound: a large translated program misses the
  16 KB instruction cache constantly. **Code size and layout are speed.**
- Hand-optimised C versions of the hottest functions gained little. Replacing work at
  the API level and moving work to the Media Engine gained much more.
- Frame-time spikes are a separate problem from the average: synchronous log writes,
  synchronous disc reads, the game's own streaming, first-use conversions. Buffer
  logs and write them from another thread; read the disc asynchronously.

## 15. Testing discipline

**PPSSPP gives booleans, hardware gives numbers.** An emulator tells you whether a
build boots, renders and plays. It does not tell you how fast it is or how much
memory is free: PPSSPP does not charge for the GE, paces the loop itself, reports
several MB more free memory and has no Media Engine. Never take a timing from it.

The ladder of proofs used for PS2RC itself, which a new feature should climb too:

| Level | Content |
|---|---|
| decoder | every instruction word compared with GNU objdump |
| synthetic programs | small R5900 programs built with GNU as, run on the host, in PPSSPP and on a PSP, bit-exact against PCSX2 |
| real compiled code | functions of a compiled program called with many inputs; registers and memory compared |
| VU0 | macro and micro programs, exact against PCSX2's interpreter over vectors covering every path |
| whole game | deterministic milestones on the host and the PSP; checks at zero |

- PCSX2 is the reference (through its PINE interface). Its interpreter and its
  recompiler disagree on some FPU edge cases: compare against both.
- Break each translation rule on purpose once and make sure a test fails.
- Commit golden outputs of reference runs so tests run without PCSX2.

On hardware:

- Increase the build number with every deploy and show it in the XMB title, the log
  and on screen.
- A/B sets: one folder per variant, each with its own title, sharing the game data;
  the same scripted input; run the control first and again at the end; same power
  state. Compare by game time, not by iteration count, and check first that both
  variants did the same guest work. A change that alters gameplay (timing, random
  numbers) moves the scripted run into another situation: compare profiler zones,
  not totals.
- Logs: open, append and close per write (the memory stick's file system records a
  file's size only when it is closed), keep two rotating logs per folder.
- Copy builds to the memory stick file by file and verify checksums.
- Exit in order (audio, GE, counters, kernel module, then exit the game); an
  unordered exit powered consoles off.

## 16. Share your work

When you publish your fork or your pack:

- keep `LICENSE` and `NOTICE.md`, and put the credit line from the README in your
  README and on every release page;
- publish code and configuration only: no game files, no disc images, no generated C,
  no EBOOT built from a commercial game, no tables copied from a game, no hand
  translations of the game's code;
- name things neutrally (see [CONTRIBUTING.md](../CONTRIBUTING.md));
- keep it free: no selling, renting, paywalls or subscriptions; voluntary donations
  are fine only if they gate nothing;
- the start-up credit screen may be kept, changed or removed; the credit in your
  README, with the link to https://github.com/SandstoneDev/PS2RC, stays.

Users of your pack build their own copy from their own disc, on their own machine.
