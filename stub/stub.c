// "Exvius Revival Desktop.exe": starts the launcher (app\launcher.py) with the Python that ships in this folder.
//
// It is also the one program here that can put an update in place, because when it runs nothing else of this
// program does: Python, the start window and the game window cannot be replaced while they are running.
// The launcher (app\updater.py) downloads an update, checks it, unpacks it into update\staged\files and writes
// update\staged\apply.txt, which names the items to replace. This program then, before starting the launcher:
//   - waits until the launcher that asked for it has closed, and until no program of this folder is running;
//   - for each item moves the installed one to update\backup\<old version>\ and the new one into its place,
//     noting every move in update\journal.txt before the next one;
//   - if a move fails, undoes every move made so far (the old version is whole again) and says why in
//     update\result.txt; if the computer was switched off half-way, the next start finds the journal and undoes
//     the moves the same way;
//   - starts the new version's copy of this program.
// Only the names in ITEMS are ever moved. client\, save\, data\, logs\, pictures\ and settings.json are not in it.
// What it does is written to logs\update.log.
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define TITLE L"Exvius Revival Desktop"
#define N_ITEMS 9
static const wchar_t *ITEMS[N_ITEMS] = {L"app", L"runner", L"server", L"source", L"python", L"Exvius Revival Desktop.exe",
                                        L"README.txt", L"LICENSES.txt", L"QUICK START.txt"};
static const char *ITEMS_A[N_ITEMS] = {"app", "runner", "server", "source", "python", "Exvius Revival Desktop.exe",
                                       "README.txt", "LICENSES.txt", "QUICK START.txt"};
#define PATH 1400
static wchar_t dir[1024];                       // this program's folder
static char from_v[40], to_v[40];               // versions, as apply.txt gives them (digits and dots only)
static int moves, fail_at, crash_at;            // test switches: see move()

static void in_dir(wchar_t *out, const wchar_t *rel) { _snwprintf(out, PATH, L"%ls\\%ls", dir, rel); out[PATH - 1] = 0; }
static int exists(const wchar_t *p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }

static void say(const char *format, ...) {      // one line in logs\update.log
    wchar_t p[PATH]; in_dir(p, L"logs"); CreateDirectoryW(p, NULL);
    in_dir(p, L"logs\\update.log");
    HANDLE f = CreateFileW(p, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    char line[1200]; SYSTEMTIME t; GetLocalTime(&t);
    int n = snprintf(line, 64, "%04d-%02d-%02d %02d:%02d:%02d  ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    va_list ap; va_start(ap, format); n += vsnprintf(line + n, sizeof line - n - 2, format, ap); va_end(ap);
    if (n > (int)sizeof line - 2) n = sizeof line - 2;
    line[n++] = '\n';
    DWORD w; WriteFile(f, line, (DWORD)n, &w, NULL); CloseHandle(f);
}
static int write_text(const wchar_t *rel, const char *text, int append) {
    wchar_t p[PATH]; in_dir(p, rel);
    HANDLE f = CreateFileW(p, append ? FILE_APPEND_DATA : GENERIC_WRITE, 0, NULL, append ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    DWORD w; BOOL ok = WriteFile(f, text, (DWORD)strlen(text), &w, NULL);
    FlushFileBuffers(f); CloseHandle(f);
    return ok && w == strlen(text);
}
static int read_text(const wchar_t *rel, char *out, int size) {
    wchar_t p[PATH]; in_dir(p, rel);
    HANDLE f = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    DWORD r = 0; ReadFile(f, out, (DWORD)size - 1, &r, NULL); CloseHandle(f);
    out[r] = 0;
    return 1;
}
static void drop(const wchar_t *rel) { wchar_t p[PATH]; in_dir(p, rel); DeleteFileW(p); }
static int version_ok(const char *v) {          // it becomes a folder name: digits and dots only
    if (!*v || strlen(v) > 30) return 0;
    for (; *v; v++) if (!((*v >= '0' && *v <= '9') || *v == '.')) return 0;
    return 1;
}
static int item_index(const char *name) { for (int i = 0; i < N_ITEMS; i++) if (!strcmp(name, ITEMS_A[i])) return i; return -1; }

// One move, tried for five seconds: a virus scanner may hold a file for a moment after it was written.
// Tests: EXVIUS_UPDATE_TEST_FAIL=N makes the Nth move fail; EXVIUS_UPDATE_TEST_STOP=N ends this program right
// after the Nth move, as a power cut would.
static int move(const wchar_t *from, const wchar_t *to) {
    moves++;
    if (fail_at && moves == fail_at) { say("test: move %d made to fail", moves); SetLastError(ERROR_ACCESS_DENIED); return 0; }
    for (int i = 0; i < 25; i++) {
        if (MoveFileExW(from, to, MOVEFILE_WRITE_THROUGH)) {
            if (crash_at && moves == crash_at) { say("test: stopping after move %d", moves); ExitProcess(3); }
            return 1;
        }
        Sleep(200);
    }
    return 0;
}
static void places(int i, wchar_t *live, wchar_t *back, wchar_t *fresh) {
    wchar_t rel[PATH];
    in_dir(live, ITEMS[i]);
    _snwprintf(rel, PATH, L"update\\backup\\%hs\\%ls", from_v, ITEMS[i]); rel[PATH - 1] = 0; in_dir(back, rel);
    _snwprintf(rel, PATH, L"update\\staged\\files\\%ls", ITEMS[i]); rel[PATH - 1] = 0; in_dir(fresh, rel);
}
// Undo, last move first. out[i]: the installed item was moved to the backup. in[i]: the new one was moved into place.
// What is on the disk counts for more than what was noted: the backup folder was empty when the replacing began, so
// an item found in it was moved there by this update, whether or not the note of it was written before a power cut.
static int undo(const int *out, const int *in) {
    int whole = 1;
    wchar_t live[PATH], back[PATH], fresh[PATH];
    for (int i = N_ITEMS - 1; i >= 0; i--) {
        places(i, live, back, fresh);
        if (out[i] || exists(back)) {
            if (!exists(back)) { whole = 0; say("undo: the backup of %s is missing", ITEMS_A[i]); continue; }
            if (exists(live) && (exists(fresh) || !MoveFileExW(live, fresh, MOVEFILE_WRITE_THROUGH))) { whole = 0; say("undo: could not take the new %s out again (error %lu)", ITEMS_A[i], GetLastError()); continue; }
            if (!MoveFileExW(back, live, MOVEFILE_WRITE_THROUGH)) { whole = 0; say("undo: could not put %s back (error %lu)", ITEMS_A[i], GetLastError()); }
        } else if (in[i] && exists(live) && !exists(fresh)) {       // an item the old version did not have
            if (!MoveFileExW(live, fresh, MOVEFILE_WRITE_THROUGH)) { whole = 0; say("undo: could not take the new %s out again (error %lu)", ITEMS_A[i], GetLastError()); }
        }
    }
    return whole;
}
static void finish(int ok, const char *reason) {
    char text[900];
    snprintf(text, sizeof text, "%s\n%s\n%s\n%s\n", ok ? "ok" : "failed", from_v, to_v, reason);
    write_text(L"update\\result.txt", text, 0);
    drop(L"update\\staged\\apply.txt");
    drop(L"update\\journal.txt");
    say("%s: %s -> %s. %s", ok ? "updated" : "not updated", from_v, to_v, reason);
}

// Is a program of this folder still running? A running program's file cannot be opened for writing.
static int in_use(const wchar_t *rel) {
    wchar_t p[PATH]; in_dir(p, rel);
    if (!exists(p)) return 0;
    HANDLE f = CreateFileW(p, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) { CloseHandle(f); return 0; }
    return GetLastError() == ERROR_SHARING_VIOLATION;
}
static int programs_running(void) { return in_use(L"python\\pythonw.exe") || in_use(L"python\\python.exe") || in_use(L"runner\\runner.exe"); }

// The previous run of this program stopped in the middle of replacing (a power cut): undo what it had done.
static void recover(void) {
    char text[4000]; int out[N_ITEMS] = {0}, in[N_ITEMS] = {0}, done = 0;
    if (!read_text(L"update\\journal.txt", text, sizeof text)) return;
    from_v[0] = to_v[0] = 0;
    for (char *line = strtok(text, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        int i;
        if (!strncmp(line, "from ", 5)) snprintf(from_v, sizeof from_v, "%s", line + 5);
        else if (!strncmp(line, "to ", 3)) snprintf(to_v, sizeof to_v, "%s", line + 3);
        else if (!strncmp(line, "out ", 4) && (i = item_index(line + 4)) >= 0) out[i] = 1;
        else if (!strncmp(line, "in ", 3) && (i = item_index(line + 3)) >= 0) in[i] = 1;
        else if (!strcmp(line, "done")) done = 1;
    }
    if (done || !version_ok(from_v)) { drop(L"update\\journal.txt"); return; }
    say("the last update (%s -> %s) was interrupted; putting the old files back", from_v, to_v);
    int whole = undo(out, in);
    finish(0, whole ? "The update was interrupted before it had finished (the computer or the program was shut down). "
                      "The previous version was put back. Nothing else was changed."
                    : "The update was interrupted and some files could not be put back. Unpack the full zip of the release over this folder; "
                      "your save, settings and game data are untouched.");
}

// Put the unpacked update in place. Returns 1 when the files were replaced.
static int apply(DWORD wait_pid) {
    char text[4000]; int wanted[N_ITEMS] = {0}, out[N_ITEMS] = {0}, in[N_ITEMS] = {0}, n = 0;
    wchar_t live[PATH], back[PATH], fresh[PATH], p[PATH];
    if (!read_text(L"update\\staged\\apply.txt", text, sizeof text)) return 0;
    from_v[0] = to_v[0] = 0;
    int header = 0, bad = 0;
    for (char *line = strtok(text, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        if (!strcmp(line, "exvius-update 1")) header = 1;
        else if (!strncmp(line, "from ", 5)) snprintf(from_v, sizeof from_v, "%s", line + 5);
        else if (!strncmp(line, "to ", 3)) snprintf(to_v, sizeof to_v, "%s", line + 3);
        else if (!strncmp(line, "item ", 5)) { int i = item_index(line + 5); if (i < 0) bad = 1; else { wanted[i] = 1; n++; } }
        else bad = 1;
    }
    if (!header || bad || !n || !version_ok(from_v) || !version_ok(to_v)) {
        say("update\\staged\\apply.txt is not what this program writes; the update is left alone");
        if (!version_ok(from_v)) strcpy(from_v, "0");
        if (!version_ok(to_v)) strcpy(to_v, "0");
        finish(0, "The unpacked update was not understood, so it was not installed. Nothing was changed.");
        return 0;
    }
    say("update %s -> %s is unpacked; putting it in place", from_v, to_v);
    if (wait_pid) {                              // the start window that asked for this is closing
        HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, wait_pid);
        if (h) { WaitForSingleObject(h, 30000); CloseHandle(h); }
    }
    for (int i = 0; i < 60 && programs_running(); i++) Sleep(250);
    if (programs_running()) {                    // the game or a second start window is open: next time
        say("a program of this folder is still running; the update waits for the next start");
        return 0;
    }
    for (int i = 0; i < N_ITEMS; i++) if (wanted[i]) {
        places(i, live, back, fresh);
        if (!exists(fresh)) { finish(0, "The unpacked update is not complete, so it was not installed. Nothing was changed."); return 0; }
    }
    in_dir(p, L"update\\backup"); CreateDirectoryW(p, NULL);
    _snwprintf(p, PATH, L"%ls\\update\\backup\\%hs", dir, from_v); p[PATH - 1] = 0; CreateDirectoryW(p, NULL);
    char line[200];
    snprintf(line, sizeof line, "from %s\nto %s\n", from_v, to_v);
    if (!exists(p) || !write_text(L"update\\journal.txt", line, 0)) {
        finish(0, "This folder cannot be written to, so the update was not installed. Nothing was changed.");
        return 0;
    }
    const wchar_t *env = _wgetenv(L"EXVIUS_UPDATE_TEST_FAIL"); fail_at = env ? _wtoi(env) : 0;
    env = _wgetenv(L"EXVIUS_UPDATE_TEST_STOP"); crash_at = env ? _wtoi(env) : 0;
    for (int i = 0; i < N_ITEMS; i++) if (wanted[i]) {
        places(i, live, back, fresh);
        DWORD error = 0; const char *what = NULL;
        if (exists(live)) {
            if (exists(back)) { what = "a backup of it is already there"; }
            else if (!move(live, back)) { error = GetLastError(); what = "it could not be moved aside (is something in it open in another program?)"; }
            else { out[i] = 1; snprintf(line, sizeof line, "out %s\n", ITEMS_A[i]); write_text(L"update\\journal.txt", line, 1); }
        }
        if (!what) {
            if (!move(fresh, live)) { error = GetLastError(); what = "the new one could not be moved into place"; }
            else { in[i] = 1; snprintf(line, sizeof line, "in %s\n", ITEMS_A[i]); write_text(L"update\\journal.txt", line, 1); }
        }
        if (what) {
            say("%s: %s (error %lu); undoing", ITEMS_A[i], what, error);
            int whole = undo(out, in);
            char reason[700];
            if (whole) snprintf(reason, sizeof reason, "The update to %s could not be installed: \"%s\": %s (Windows error %lu). "
                                "The previous version was put back. Nothing else was changed.", to_v, ITEMS_A[i], what, error);
            else snprintf(reason, sizeof reason, "The update to %s failed at \"%s\" and some files could not be put back. Unpack the full zip of the "
                          "release over this folder; your save, settings and game data are untouched.", to_v, ITEMS_A[i]);
            finish(0, reason);
            return 0;
        }
    }
    write_text(L"update\\journal.txt", "done\n", 1);
    finish(1, "");
    return 1;
}

static int start(const wchar_t *command) {
    wchar_t cmd[4096]; wcsncpy(cmd, command, 4095); cmd[4095] = 0;
    STARTUPINFOW si; PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof si); si.cb = sizeof si;
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) return 0;
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return 1;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show) {
    (void)inst; (void)prev; (void)show;
    wchar_t cmd[4096], p[PATH];
    DWORD n = GetModuleFileNameW(NULL, dir, 1024);
    if (!n || n >= 1024) return 1;
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (slash) *slash = 0;
    DWORD wait_pid = 0;                          // "--apply-after <pid>": the start window that unpacked an update
    const wchar_t *after = cmdline ? wcsstr(cmdline, L"--apply-after") : NULL;
    if (after) { wait_pid = (DWORD)_wtoi(after + 13); cmdline = L""; }
    in_dir(p, L"update\\journal.txt");
    if (exists(p)) recover();
    in_dir(p, L"update\\staged\\apply.txt");
    if (exists(p) && apply(wait_pid)) {          // replaced: the new version's copy of this program takes over
        _snwprintf(cmd, 4096, L"\"%ls\\Exvius Revival Desktop.exe\"", dir); cmd[4095] = 0;
        if (start(cmd)) return 0;
    }
    _snwprintf(cmd, 4096, L"\"%ls\\python\\pythonw.exe\" \"%ls\\app\\launcher.py\" %ls", dir, dir, cmdline ? cmdline : L"");
    cmd[4095] = 0;
    if (!start(cmd)) {
        MessageBoxW(NULL, L"Could not start the launcher. The \"python\" and \"app\" folders must sit next to this program, "
                          L"exactly as they were unpacked.\n\nIf this happened right after an update, unpack the release's full zip "
                          L"over this folder: your save, settings and game data stay as they are.", TITLE, MB_ICONERROR);
        return 1;
    }
    return 0;
}
