# Game pack template

An empty game pack. Copy this folder to `games/<SERIAL>_v<VERSION>/` and fill it in; `games/example/` is a complete
pack to compare with. A pack describes one version of one game: how to recognise it, where its code is, which of its
functions the HLE replaces, and the C modules that replace the game-specific ones. It contains no game data: the
user's own disc is read on the user's machine, and everything derived from it goes to the work directory.

| File | What |
|---|---|
| `pack.toml` | every key, documented: identity, verification, memory map, overlays, runtime values, translation, build, run, staging |
| `hooks.def` | the hook list: the Sony library functions (by the name the core implements) and the game's own functions the pack replaces or traces |
| `src/pack_main.c` | the pack's side of the HLE interface (all optional) |

Optional files a pack may add (named in `pack.toml`): a profiler zone list (`zones.def`), text patches of the
generated C (`textpatch.tsv`), lists computed once (trap set, native fast paths, code layout), the pack's own tests.

Typical first steps:

1. Fill `[id]` and `[verify]` from the disc (`SYSTEM.CNF`, the boot program's size and SHA-256).
2. Fill `[memory]` from the boot program's program header (one loadable segment: code, data, BSS).
3. Find the Sony library functions in the program (a signature scan) and bind their names in `hooks.def`.
4. `python3 ps2rc/pipeline/translate.py --pack <name>` (start with `variant = "v0"`), then
   `bash mk/build.sh --pack <name> host` and `bash mk/run_host.sh --pack <name>` until the program reaches its main
   loop on the host; then `bash mk/build.sh --pack <name> psp`.
