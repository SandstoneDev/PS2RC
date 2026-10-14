/* launcher.c - "PS2RC Converter.exe": starts the converter window with the bundle's own Python
 * (<exe dir>\python\pythonw.exe <exe dir>\app\gui\run_gui.py: Qt plugin paths set, a message box
 * with a log when Qt cannot start), so the player needs no Python installation.
 * Build (MSYS2 UCRT64 or any MinGW-w64): gcc -O2 -municode -mwindows -static -o "PS2RC Converter.exe" launcher.c */
#include <windows.h>
#include <wchar.h>

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show)
{
    (void)inst; (void)prev; (void)show;
    wchar_t dir[MAX_PATH], cmd[3 * MAX_PATH + 64];
    DWORD n = GetModuleFileNameW(NULL, dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return 1;
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (slash) *slash = 0;
    _snwprintf(cmd, sizeof cmd / sizeof cmd[0] - 1, L"\"%ls\\python\\pythonw.exe\" \"%ls\\app\\gui\\run_gui.py\" %ls",
               dir, dir, cmdline ? cmdline : L"");
    cmd[sizeof cmd / sizeof cmd[0] - 1] = 0;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    ZeroMemory(&pi, sizeof pi);
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) {
        MessageBoxW(NULL, L"Cannot start python\\pythonw.exe next to this program.\n"
                          L"Keep the whole folder together (python, app, toolchain).", L"PS2RC Converter",
                    MB_ICONERROR);
        return 1;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
