# psp_imgui - Dear ImGui on the PSP

A small, self-contained layer that runs [Dear ImGui](https://github.com/ocornut/imgui) on the PlayStation Portable:

- **GE renderer** (sceGu): ImGui's triangles as indexed `GU_TRIANGLES` in through mode, one GE scissor per draw
  command, the font atlas as a 4-bit CLUT texture (`GU_PSM_T4`, 4 KB), any other texture through `PimguiTex`.
- **PSP pad input**: edges and auto-repeat for every button, plus two navigation modes - `PIMGUI_NAV_ROWS` (the
  D-pad drives the row lists below) or `PIMGUI_NAV_IMGUI` (the pad becomes ImGui's own gamepad navigation).
- **Pool allocator**: ImGui allocates only from a block you give it (a static array or a partition block); nothing is
  allocated while no context exists, and `pimgui_destroy()` gives everything back. Peak / failed allocations are
  reported.
- **Baked font**: ImGui's default font (ProggyClean, 13 px - pixel exact and readable at 480x272) is baked offline into
  `pimgui_font13.h` (`lib/psp_imgui/tools/bake_font.sh`), so the PSP build needs neither the TTF nor stb_truetype.
- **PSP widgets**: row lists (one focused row; Up / Down moves, Left / Right changes the value at once, Cross
  activates; the focused row is kept in view), a **marquee** for text that does not fit, and PSP button icons
  (Cross / Circle / Square / Triangle / L / R / START / SELECT / D-pad) for hint lines.
- **C API** (`psp_imgui.h`) for C programs; C++ code can call `ImGui::` directly between `pimgui_new_frame()` and
  `pimgui_render()`.

## Legibility rule: never shrink text

At 480x272 a scaled-down font is unreadable. Text that does not fit its box is left-aligned and cut with "..." while it
is not focused; the focused one scrolls as a **marquee**: a pause at the start (0.8 s), a slide at 30 px/s until its end
is visible, a pause, and back (ping-pong) - the behaviour of the derma panels of an earlier PSP UI port. The help popup
of an application should show long text wrapped (`ImGui::TextWrapped`), not smaller.

## Size and memory (measured, psp-gcc 15.2, `-Os`)

| item | bytes |
|---|---|
| code of an application that uses windows, tabs, text, row lists, marquee, icons (ImGui + psp_imgui, after `--gc-sections`) | ~210 KB |
| the whole ImGui core compiled (before dead-code removal) | ~570 KB at -O2 |
| font: texture + glyph table (read-only data) | 4 KB + 3.7 KB |
| static RAM | < 1 KB (+ your pool) |
| pool for a full-screen settings UI with ~60 rows | 150-230 KB peak (32-bit; the host test shows the 64-bit figure) |
| GE list per frame | ~24 bytes per vertex + 2 per index: ~60 KB for 2,500 vertices |

Compile with `-ffunction-sections -fdata-sections` and partial-link the UI objects with `psp-ld -r --gc-sections -u
<each function you call>` (example/Makefile, PS2RC's mk/Makefile.psp) - ImGui is big, but most of it falls away. Do
not put `-Wl,--gc-sections` on the final EBOOT link: it also drops the PSP import stub sections ("no nid section").

## Build

Compile the C++ files with psp-g++ and these flags:

```
-Os -G0 -fno-exceptions -fno-rtti -fno-threadsafe-statics -ffunction-sections -fdata-sections
'-DIMGUI_USER_CONFIG="pimgui_imconfig.h"' -I<this dir> -I<this dir>/third_party/imgui
```

Sources: `psp_imgui.cpp`, `third_party/imgui/imgui.cpp`, `third_party/imgui/imgui_draw.cpp`, `third_party/imgui/imgui_widgets.cpp`,
`third_party/imgui/imgui_tables.cpp`. Link with `-lpspgu -lm`. `example/` is a complete EBOOT (`make` with pspsdk).
`pimgui_imconfig.h` switches off the demo, debug tools, file / shell functions, default allocator and font, and asserts
on the PSP.

## Use (C)

```c
#include "psp_imgui.h"
static char pool[256 * 1024] __attribute__((aligned(16)));
int focus = 0, volume = 80;

pimgui_create(pool, sizeof pool, PIMGUI_NAV_ROWS);
for (;;) {
    sceCtrlPeekBufferPositive(&pad, 1);
    sceGuStart(GU_DIRECT, list);            /* your draw buffer, offset 2048 - 240 / 2048 - 136, through mode */
    pimgui_new_frame(pad.Buttons, 1.0f / 60.0f);
    pimgui_window_begin("main", 0, 0, 480, 272);
    pimgui_rows_begin("rows", &focus, 200);
    pimgui_row_int("Volume", &volume, 0, 100, 5, "%d %%");
    if (pimgui_row_button("Quit")) break;
    pimgui_rows_end();
    pimgui_marquee("A help line longer than the screen scrolls when it is active.", 0, 1, pimgui_rows_focus_time(), 0);
    pimgui_hint(PIMGUI_LEFT | PIMGUI_RIGHT, "change");
    pimgui_window_end();
    pimgui_render();                         /* appends GE commands to the open list */
    sceGuFinish(); sceGuSync(0, 0); sceDisplayWaitVblankStart(); sceGuSwapBuffers();
}
pimgui_destroy();                            /* the pool is free again */
```

Memory from the kernel instead of a static array (it then costs nothing while the UI is closed):

```c
SceUID uid = sceKernelAllocPartitionMemory(PSP_MEMORY_PARTITION_USER, "ui", PSP_SMEM_High, 320 * 1024, NULL);
pimgui_create(sceKernelGetBlockHeadAddr(uid), 320 * 1024, PIMGUI_NAV_ROWS);
...
pimgui_destroy();
sceKernelFreePartitionMemory(uid);
```

`pimgui_render()` leaves the GE with: scissor = the full screen, blending on (SRC_ALPHA, ONE_MINUS_SRC_ALPHA), alpha test
GREATER 0, texturing on, depth test off - restore your own state afterwards. It writes vertices with `sceGuGetMemory`
(cached lists are written back from the dcache; uncached list views need nothing).

## Used by

PS2RC's in-game settings menu (`hle/psp/hle_menu.cpp`, opened with L + R + Cross + D-pad Down in game) - a complete example of a
modal menu over a paused game: frozen last frame under the menu, tabs on L / R, a help popup on Square, values
applied live, a save that keeps the comments of the settings file.

## Versions and licences

- Dear ImGui **1.91.9b** (tag `v1.91.9b`), files in `third_party/imgui/` unchanged, SHA-256 in `third_party/imgui_files.sha256`; MIT licence,
  (c) 2014-2025 Omar Cornut - `third_party/imgui/LICENSE.txt`.
- ProggyClean.ttf (baked into `pimgui_font13.h`), (c) Tristan Grimmer, MIT licence.
- psp_imgui's own files: see `LICENSE.md`.

Credit: Dear ImGui by Omar Cornut and contributors (https://github.com/ocornut/imgui).
