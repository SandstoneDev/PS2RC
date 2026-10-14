# Contributing

Thank you for helping. Every change goes through review, so please read this page
before your first pull request.

## How changes get in

1. Fork the repository and work on a branch of your fork.
2. Open a pull request against `main`. Describe **what** changed, **why**, and **how
   you checked it**: the tests you ran, and whether it ran in an emulator or on a real
   PSP (and which model).
3. The automatic checks must pass. For a pull request from a fork they start after a
   maintainer approves the run.
4. A maintainer reviews the change. Small, focused pull requests are reviewed fastest:
   one topic per pull request.

## What a change must never contain

These rules protect the project, its maintainers and you. A pull request that breaks
any of them is closed.

- **Only your own work.** Submit only code you wrote yourself, or code whose licence
  allows it to be here (say so in the pull request, and add it to `NOTICE.md`).
  Nothing copied from a game, a game engine, a middleware SDK or any SDK whose licence
  does not allow it, including the official Sony SDKs. Nothing taken from confidential
  documentation, and no citations of it (titles, page numbers, header names).
- **No code recovered from a game.** No output of tools that turn a compiled program
  back into source code, and no instruction listings of a game. PS2RC translates a
  game on the user's machine; nothing translated or recompiled from a real game is
  ever committed, not even a single function as an example.
- **No game data.** No code, textures, models, sounds, music, video, text, scripts,
  maps or tables taken from a game disc or game files, in any form: not as files, not
  as arrays in a header, not as base64, not as test fixtures. Tests use synthetic
  programs written for this repository.
- **No names from games.** Code, strings, comments and file names use neutral names
  (`sound_driver_rpc`, `world_update`, "the game's renderer"), not the names of a
  game, its publisher, its places or characters, or the class and function names of
  its code. A game pack refers to the functions it hooks by address and by a neutral
  label.
- **No secrets and no personal data.** No passwords, tokens, keys, IP addresses,
  e-mail addresses, real names or paths of your own machine (your user or home
  directory, your drives). Machine-specific paths belong in your untracked
  `local.toml`.

Game packs for other games live in your own fork or repository, under the terms of
[LICENSE](LICENSE): free of charge, with the credit line, and without any game data.

## Code

- Python for the translator and tools, C for the runtime and the HLE, built with
  `-Wall`. Do not add new warnings.
- Match the surrounding code: naming, layout and comment density. Comments explain
  *why*, in English, and follow the rules above like everything else.
- Plain ASCII punctuation in code and documents: a hyphen, not an en or em dash.
- Keep the PSP in mind: 64 MB of RAM on a PSP-2000/3000, a small data cache, a GE
  that does not clip triangles for you, and a Media Engine that shares nothing with
  the main CPU's cache. Measure on hardware before claiming a performance change: an
  emulator tells you whether something works, not how fast it is.
- A change to the translator must keep the generated code correct for every test
  program: run the test suite, and add a test for any new instruction pattern or
  analysis rule.

## Tests

```bash
python -m pytest -q -m "not wsl and not oracle and not psp"
```

runs the tests that need only Python. The `wsl` tests also need a native gcc and the
PSP toolchain, `oracle` tests need the PCSX2 emulator, and `psp` tests need PPSSPP or
a PSP. See [docs/PORTING_GUIDE.md](docs/PORTING_GUIDE.md) for the full testing
discipline.

## Licence of your contribution

By opening a pull request you confirm the points in "What a change must never
contain", and you agree to the contribution terms in section 3 of
[LICENSE](LICENSE).
