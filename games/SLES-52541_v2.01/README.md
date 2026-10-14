# Game pack SLES-52541 v2.01

This pack converts the PS2 version of Grand Theft Auto: San Andreas (PAL disc SLES-52541, version 2.01).
This project is not affiliated with, authorized or endorsed by Rockstar Games, Take-Two Interactive or Sony Interactive Entertainment.

The pack holds only our own code and the facts needed to work with that disc (addresses, sizes, checksums, file
names). It holds no game code and no game data: the converter makes everything game-derived on your PC, from
your own disc, and it never leaves your PC. The game is reserved under the licence: see [RESERVED.md](RESERVED.md).
Third-party notices of the pack: [NOTICE.md](NOTICE.md).

## What you need

- A disc image of **your own** PAL disc SLES-52541, **version 2.01** (made with a PC drive; an ISO). Another
  version or region is refused with a message: the boot program's size and SHA-256 are checked. The check only
  tells the version; it does not prove ownership.
- A PSP-2000 or PSP-3000 with custom firmware (ARK), about 4 GB free on the memory stick (with all sound files).
- The converter (the repository's README: download the release, or build from source).

## Convert

Window: start the converter, pick the disc image, pick the memory stick (or an output folder), press Convert.

Command line (from the repository folder):

```
python -m ps2rc.convert <your disc image> --out <output folder>
```

Then copy the converted folder (its name is shown at the end) to `PSP/GAME/` on the memory stick. Takes about
10 minutes on a 12-thread PC (most of it compiling and copying 3.9 GB). Options: `--audio min` (a small test folder),
`--movies skip`, `--lang ru` (Russian comments in the settings file), `--set draw_distance=60`.
`python -m ps2rc.convert verify <PSP folder>` checks a folder later; `update` repairs it and keeps your settings
and saves.

The settings file `ps2rc.cfg` next to `EBOOT.PBP` explains every setting (draw distance, fog, detail,
population, frame cap, overclock, controls, sound). `session.log` in the same folder shows what is in effect.
In a folder converted by version 0.1.0 the file still has its older name: the game reads it until an `update`
moves your values into `ps2rc.cfg` (the old file stays next to it as `.old`).

## The public build compared with the developer build

The converter's default build profile ("public") leaves out every module of the developer build that
re-implements a function of the game by hand. In their place:

- the game's sound driver and its VU0 micro program are translated from **your** disc on your PC by generic
  translators of PS2RC (the result is the same sound and the same arithmetic);
- the game's start-up code runs from facts in `pack.toml` instead of a hand-made copy;
- the game's own world update runs as translated; the settings `far_ai`, `far_freeze` and `far_collision`
  work through PS2RC's own far-entity policy on four of its calls (`sites.def`, `src/hle_far.c`): the same
  entities are skipped in the same frames as in the developer build;
- the native fast paths of some hot game functions and native versions of the render driver's state setters
  are not built.

Effects: the public build is **slower** than the developer build: about 1.6 ms per frame for the fast paths
(measured), plus an amount for the render-state setters that has not been measured on a PSP yet (Unknown).
Everything in the settings file works the same.

## Controls

The PSP has no L2, R2, L3, R3 and no right stick. **SELECT is a modifier**: hold it and press a second
button. SELECT itself still reaches the game as SELECT (the game changes the camera view when SELECT is
pressed), so the view may change once when you press SELECT; press it again to get the view back.

| PSP | PS2 |
|---|---|
| Cross, Circle, Square, Triangle | the same |
| L / R | L1 / R1 |
| START / SELECT | START / SELECT |
| D-pad | D-pad |
| analog stick | left stick |
| SELECT + L | L2 |
| SELECT + R | R2 |
| SELECT + Triangle | L3 (left stick click) |
| SELECT + Circle | R3 (right stick click) |
| SELECT + D-pad | right stick, full deflection (camera); two directions at once give a diagonal |
| SELECT + Up | right stick up (default); with `select_up=r3` or `select_up=l3` in `ps2rc.cfg` it is R3 / L3 instead |

Port functions: **L + R + SELECT** shows or hides the frames-per-second counter (`show_fps` in the settings
file sets it at start). HOME -> Exit returns to the PSP menu. Avoid L + R + Down + START: custom firmware uses it
for its own menu.

### What the game's button prompts mean on the PSP

The game shows PS2 buttons. The PS2 actions below follow the public PS2 control lists of the game (game guides, PlayStation section);
the PSP column is this port's mapping.

On foot:

| Action | PS2 | PSP |
|---|---|---|
| fire / attack | Circle | Circle |
| aim weapon, lock on | R1 | R |
| sprint | Cross | Cross |
| jump | Square | Square |
| enter / exit a vehicle | Triangle | Triangle |
| crouch | L3 | SELECT + Triangle |
| previous / next weapon or target | L2 / R2 | SELECT + L / SELECT + R |
| look behind | R3 | SELECT + Circle |
| move the camera | right stick | SELECT + D-pad |
| gang: follow / recruit (active mode) | D-pad Up | D-pad Up |
| gang: stop (passive mode) | D-pad Down | D-pad Down |
| talk: positive / negative answer | D-pad Right / Left | D-pad Right / Left |
| change the camera view | SELECT | SELECT |
| pause menu | START | START |

Cars and bikes:

| Action | PS2 | PSP |
|---|---|---|
| accelerate | Cross | Cross |
| brake / reverse | Square | Square |
| enter / exit | Triangle | Triangle |
| handbrake | R1 | R |
| vehicle weapon / fire (drive-by) | L1 | L |
| look left / look right | L2 / R2 | SELECT + L / SELECT + R |
| look behind | L2 + R2 | hold SELECT first, then press L and R together (pressing SELECT last while L and R are already held toggles the fps counter instead) |
| horn | L3 | SELECT + Triangle |
| start / stop a side mission (taxi, ambulance, police, fire truck) | R3 | SELECT + Circle |
| next / previous radio station | D-pad Up / Down | D-pad Up / Down |
| trip skip (taxi rides, some missions) | D-pad Right | D-pad Right |
| special control (hydraulics, turret, tow truck arm) | right stick | SELECT + D-pad |
| change the camera view | SELECT | SELECT |

Planes:

| Action | PS2 | PSP |
|---|---|---|
| accelerate / brake, reverse | Cross / Square | Cross / Square |
| roll left / right, pitch forward / back | left stick | analog stick |
| rudder left / right | L2 / R2 | SELECT + L / SELECT + R |
| fire / alternate fire | L1 / Circle | L / Circle |
| landing gear | R3 | SELECT + Circle |

Helicopters:

| Action | PS2 | PSP |
|---|---|---|
| ascend / descend | Cross / Square | Cross / Square |
| roll, pitch | left stick | analog stick |
| rudder left / right | L2 / R2 | SELECT + L / SELECT + R |
| fire / secondary fire | L1 / Circle | L / Circle |

Every PS2 control of the game is reachable on the PSP. Two-button PS2 combinations that also need SELECT (look
behind in a car) take a moment of practice: hold SELECT first.

### Hydraulics and the lowrider contest

Cars with hydraulics are driven with the **right stick** on the PS2: push it to raise or drop the front, the
back, a side or a single corner (diagonals). The game's help texts also use the stick clicks: R3 makes the car
hop, L3 locks the hydraulics in place (read from the game's own button codes; unverified on a PSP build).

On the PSP:

- **move the hydraulics**: hold SELECT and press the D-pad: Up = front, Down = back, Left / Right = the sides,
  two buttons together = a corner (Up + Left = front left, and so on). Keep `select_up` at its default (`stick`),
  otherwise Up gives a stick click instead of the front.
- **hop**: SELECT + Circle (R3). **Lock**: SELECT + Triangle (L3).

The lowrider contest (the bounce competition): the screen shows a row of arrows that move towards a marker;
push the right stick in the arrow's direction when the arrow reaches the marker, in time with the music. On the
PSP: **hold SELECT the whole time** (pressing it again changes the camera view) and **tap** the D-pad direction on
the beat - Up, Down, Left, Right, or two at once for a diagonal arrow. Release the D-pad between beats (each tap
is one move; the PSP's D-pad always gives the full stick deflection).

## Licence

PS2RC License 1.0, see [LICENSE](../../LICENSE). This game is reserved: [RESERVED.md](RESERVED.md). Never share a
converted folder, an EBOOT built from the game, or any file from the disc - every player converts their own disc.
