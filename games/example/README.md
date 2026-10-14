# The example game pack

A complete game pack for a small PS2 program written for this repository (`src/example.S`): no game, no disc, no
Sony code. It takes the program through every stage of PS2RC, so it is the place to start before porting a real
game, and the end-to-end test of the pipeline.

The program sets up its stack and heap with two EE kernel calls, prints a line through the C library's `write`, runs
60 "frames" (a little 32-bit, 64-bit and 128-bit MMI arithmetic, then `DelayThread(20 ms)`), prints its state and
calls `Exit(0)`. `write`, `DelayThread` and `Exit` are stand-ins at fixed addresses: the HLE core implements them by
name and `hooks.def` binds the names to the addresses, exactly as a real pack binds the Sony library functions it
found in its game. `frame` is traced as the main loop (`[runtime] main_loop`).

## Files

| File | What |
|---|---|
| `pack.toml` | identity, memory map, runtime values, translation and build options |
| `hooks.def` | the hook list: three library functions by name, the main-loop trace |
| `src/example.S`, `src/example.ld` | the program and its memory map |
| `build_elf.sh` | assembles the program (pspdev binutils in R5900 mode) into `<work>/example/disc/EXAMPLE.ELF`, writes the disc map, checks `hooks.def` against the symbols |
| `games/example/tests/test_example.py` | the end-to-end test (needs the toolchain) |

The pack has no C modules of its own: the core alone runs this program.

## Commands

Linux or WSL with the pspdev toolchain (`~/pspdev`), Python 3.11+ with pyelftools:

```
bash games/example/build_elf.sh                        # the "disc": <work>/example/disc/EXAMPLE.ELF
python3 ps2rc/pipeline/translate.py --pack example     # <work>/example/trans: main.h, main_000.c, main_tab.c
bash mk/build.sh --pack example host                   # the host harness
bash mk/run_host.sh --pack example 60 0 example        # prints the program's two lines (TTY) and exits
bash mk/build.sh --pack example psp                    # <work>/example/psp/EBOOT.PBP
python3 ps2rc/pipeline/stage.py --pack example --out <dir>/PS2RC_EXAMPLE    # a PSP folder (PSP/GAME/...)
```

Expected output of the run (the test computes the same numbers in Python):

```
TTY: PS2RC example: hello from a PS2 program translated to C
TTY: PS2RC example: state 90afa006 000006ea c069b4d7 00011242 fffff8da
guest Exit(0)
```

On the PSP the program's output goes to `session.log` next to the EBOOT, which ends with
`result=main_loop end=limit code=0`.
