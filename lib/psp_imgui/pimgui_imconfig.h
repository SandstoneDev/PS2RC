/* pimgui_imconfig.h - Dear ImGui compile-time configuration of psp_imgui (passed as IMGUI_USER_CONFIG).
 *
 * The vendored imgui/ files are unchanged; everything PSP-specific is decided here:
 *   - no demo / metrics / debug tools, no obsolete API, no file / shell / clipboard functions, no SSE;
 *   - the PSP build has no default allocator (psp_imgui gives ImGui its own pool allocator) and no embedded TTF:
 *     the font atlas is baked offline (tools/bake_font.cpp -> pimgui_font13.h), so stb_truetype is never
 *     referenced and drops out at link time;
 *   - asserts are compiled out on the PSP (their strings alone cost tens of KB); the host test build keeps them.
 * Define PIMGUI_HOST for a host (x86) build: tests and the font baker.
 */
#ifndef PIMGUI_IMCONFIG_H
#define PIMGUI_IMCONFIG_H

#define IMGUI_DISABLE_DEMO_WINDOWS
#define IMGUI_DISABLE_DEBUG_TOOLS
#define IMGUI_DISABLE_OBSOLETE_FUNCTIONS
#define IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS
#define IMGUI_DISABLE_WIN32_FUNCTIONS
#define IMGUI_DISABLE_SSE

#ifndef PIMGUI_HOST
#define IMGUI_DISABLE_FILE_FUNCTIONS            /* no ImFileOpen & co (io.IniFilename / LogFilename are NULL) */
#define IMGUI_DISABLE_DEFAULT_ALLOCATORS        /* psp_imgui.cpp: ImGui::SetAllocatorFunctions(pool) */
#define IMGUI_DISABLE_DEFAULT_FONT              /* the baked atlas replaces ProggyClean.ttf + stb_truetype */
#define IM_ASSERT(_EXPR) ((void)0)
#endif

#endif
