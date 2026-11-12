// "Exvius Revival Desktop.exe": starts the launcher (app\launcher.py) with the Python that ships in this folder.
// It exists only so there is something to double-click; all the work is in the launcher.
#include <windows.h>
#include <wchar.h>

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show) {
    (void)inst; (void)prev; (void)show;
    wchar_t dir[1024], cmd[4096];
    DWORD n = GetModuleFileNameW(NULL, dir, 1024);
    if (!n || n >= 1024) return 1;
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (slash) *slash = 0;
    _snwprintf(cmd, 4096, L"\"%ls\\python\\pythonw.exe\" \"%ls\\app\\launcher.py\" %ls", dir, dir, cmdline ? cmdline : L"");
    cmd[4095] = 0;
    STARTUPINFOW si; PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof si); si.cb = sizeof si;
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) {
        MessageBoxW(NULL, L"Could not start the launcher. The \"python\" and \"app\" folders must sit next to this program, "
                          L"exactly as they were unpacked.", L"Exvius Revival Desktop", MB_ICONERROR);
        return 1;
    }
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return 0;
}
