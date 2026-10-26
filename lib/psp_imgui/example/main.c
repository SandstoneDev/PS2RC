/* main.c - psp_imgui example: a settings screen driven by the D-pad, in C, on the PSP GE.
 * Build: make (pspsdk; see Makefile). Controls: Up / Down choose, Left / Right change, Cross activates, START quits. */
#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspctrl.h>
#include "psp_imgui.h"

PSP_MODULE_INFO("pimgui_example", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);

static unsigned int list[64 * 1024] __attribute__((aligned(64)));   /* 256 KB display list */
static char pool[256 * 1024] __attribute__((aligned(16)));           /* ImGui's memory (only while it exists) */
static volatile int running = 1;

static int exit_cb(int a, int b, void *c) { (void)a; (void)b; (void)c; running = 0; return 0; }
static int cb_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    sceKernelRegisterExitCallback(sceKernelCreateCallback("exit", exit_cb, NULL));
    sceKernelSleepThreadCB();
    return 0;
}

int main(void)
{
    SceUID th = sceKernelCreateThread("cb", cb_thread, 0x11, 0x1000, 0, NULL);
    if (th >= 0) sceKernelStartThread(th, 0, NULL);
    sceGuInit();
    sceGuStart(GU_DIRECT, list);
    sceGuDrawBuffer(GU_PSM_8888, (void *)0, 512);
    sceGuDispBuffer(480, 272, (void *)0x88000, 512);
    sceGuOffset(2048 - 240, 2048 - 136);
    sceGuViewport(2048, 2048, 480, 272);
    sceGuScissor(0, 0, 480, 272);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    pimgui_create(pool, sizeof pool, PIMGUI_NAV_ROWS);
    int focus = 0, volume = 80, quality = 1, vsync = 1, frames = 0;
    static const char *const q[] = {"low", "medium", "high"};
    while (running) {
        SceCtrlData pad;
        sceCtrlPeekBufferPositive(&pad, 1);
        if (pad.Buttons & PSP_CTRL_START) running = 0;
        sceGuStart(GU_DIRECT, list);
        sceGuClearColor(GU_RGBA(32, 48, 80, 255));
        sceGuClear(GU_COLOR_BUFFER_BIT);
        pimgui_new_frame(pad.Buttons, 1.0f / 60.0f);
        pimgui_window_begin("main", 20, 16, 440, 240);
        pimgui_text_colored(PIMGUI_RGBA(255, 190, 64, 255), "psp_imgui example  (frame %d)", frames++);
        pimgui_separator();
        pimgui_rows_begin("rows", &focus, 150);
        pimgui_row_int("Volume", &volume, 0, 100, 5, "%d %%");
        pimgui_row_choice("Quality", &quality, q, 3);
        pimgui_row_bool("Wait for vertical sync", &vsync);
        pimgui_row("A label that is much too long for its column scrolls as a marquee", "ok", -1.0f, 0, NULL, 0);
        if (pimgui_row_button("Quit")) running = 0;
        pimgui_rows_end();
        pimgui_separator();
        pimgui_marquee("Help line: text that does not fit is never shrunk - it scrolls, like the derma panels of "
                       "the PSP UI it was copied from.", 0, 1, pimgui_rows_focus_time(), 0);
        pimgui_hint(PIMGUI_UP | PIMGUI_DOWN, "choose");
        pimgui_hint(PIMGUI_LEFT | PIMGUI_RIGHT, "change");
        pimgui_hint(PIMGUI_START, "quit");
        pimgui_window_end();
        pimgui_render();
        sceGuFinish();
        sceGuSync(0, 0);
        if (vsync) sceDisplayWaitVblankStart();
        sceGuSwapBuffers();
    }
    pimgui_destroy();
    sceGuTerm();
    sceKernelExitGame();
    return 0;
}
