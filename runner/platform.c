// The few things that differ between Windows and POSIX hosts: reserving memory, files with UTF-8 names,
// folders, random bytes, the local time zone, a text prompt and opening a web page.
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "runner.h"

static void fill_stat(void *out, int is_dir, int64_t size, int64_t mtime) {
    uint8_t *o = out;
    memset(o, 0, 128);
    uint64_t dev = 1, ino = 1; uint32_t mode = is_dir ? 0040755 : 0100644, nlink = 1; int32_t blk = 4096; int64_t blocks = (size + 511) / 512;
    memcpy(o, &dev, 8); memcpy(o + 8, &ino, 8); memcpy(o + 16, &mode, 4); memcpy(o + 20, &nlink, 4);
    memcpy(o + 48, &size, 8); memcpy(o + 56, &blk, 4); memcpy(o + 64, &blocks, 8);
    memcpy(o + 72, &mtime, 8); memcpy(o + 88, &mtime, 8); memcpy(o + 104, &mtime, 8);
}

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <direct.h>
#include <shellapi.h>

static wchar_t *wide(const char *s, wchar_t *buf, int cap) { MultiByteToWideChar(CP_UTF8, 0, s, -1, buf, cap); return buf; }
uint8_t *os_reserve(size_t size) { return VirtualAlloc(NULL, size, MEM_RESERVE, PAGE_READWRITE); }
void os_commit(void *p, size_t len) { if (!VirtualAlloc(p, len, MEM_COMMIT, PAGE_READWRITE)) fatal("out of memory (commit of %llu KB failed)", (unsigned long long)(len >> 10)); }
void os_release(void *p, size_t len) { VirtualFree(p, len, MEM_DECOMMIT); }
void os_guard(void *p, size_t len) { (void)p; (void)len; }
int os_open(const char *path, int lf, int mode) {
    (void)mode;
    wchar_t w[1200];
    int f = _O_BINARY | ((lf & 3) == 1 ? _O_WRONLY : (lf & 3) == 2 ? _O_RDWR : _O_RDONLY);
    if (lf & 0x40) f |= _O_CREAT; if (lf & 0x80) f |= _O_EXCL; if (lf & 0x200) f |= _O_TRUNC; if (lf & 0x400) f |= _O_APPEND;
    int h = _wopen(wide(path, w, 1200), f, _S_IREAD | _S_IWRITE);
    return h < 0 ? -errno : h;
}
int os_stat(const char *path, void *out) {
    wchar_t w[1200]; struct __stat64 st;
    if (_wstat64(wide(path, w, 1200), &st) != 0) return -errno;
    fill_stat(out, (st.st_mode & _S_IFDIR) != 0, st.st_size, st.st_mtime);
    return 0;
}
int os_fstat(int h, void *out) { struct __stat64 st; if (_fstat64(h, &st) != 0) return -errno; fill_stat(out, 0, st.st_size, st.st_mtime); return 0; }
int os_mkdir(const char *p) { wchar_t w[1200]; return _wmkdir(wide(p, w, 1200)) ? -errno : 0; }
int os_unlink(const char *p) { wchar_t w[1200]; return _wunlink(wide(p, w, 1200)) ? -errno : 0; }
int os_rmdir(const char *p) { wchar_t w[1200]; return _wrmdir(wide(p, w, 1200)) ? -errno : 0; }
int os_rename(const char *a, const char *b) {
    wchar_t wa[1200], wb[1200];
    return MoveFileExW(wide(a, wa, 1200), wide(b, wb, 1200), MOVEFILE_REPLACE_EXISTING) ? 0 : -2;
}
int os_truncate(int h, int64_t len) { return _chsize_s(h, len) ? -errno : 0; }
// Windows has no read-at-a-position call that leaves the file position alone, so a positioned read moves the position
// and puts it back. A lock per file handle keeps that from mixing with an ordinary read on the same file.
static CRITICAL_SECTION file_locks[64]; static int file_locks_ready;
static CRITICAL_SECTION *file_lock(int h) {
    if (!file_locks_ready) { for (int i = 0; i < 64; i++) InitializeCriticalSection(&file_locks[i]); file_locks_ready = 1; }
    return &file_locks[(unsigned)h & 63];
}
void os_files_init(void) { file_lock(0); }
int64_t os_lseek(int h, int64_t off, int whence) {
    CRITICAL_SECTION *l = file_lock(h); EnterCriticalSection(l);
    int64_t r = _lseeki64(h, off, whence); if (r < 0) r = -errno;
    LeaveCriticalSection(l);
    return r;
}
static int64_t read_all(int h, void *buf, size_t n) {
    int64_t total = 0;
    while (n) { unsigned c = n > (1u << 30) ? (1u << 30) : (unsigned)n; int r = _read(h, buf, c); if (r < 0) return total ? total : -errno; if (!r) break; total += r; buf = (char *)buf + r; n -= r; }
    return total;
}
static int64_t write_all(int h, const void *buf, size_t n);
int64_t os_read(int h, void *buf, size_t n) {
    CRITICAL_SECTION *l = file_lock(h); EnterCriticalSection(l);
    int64_t r = read_all(h, buf, n);
    LeaveCriticalSection(l);
    return r;
}
int64_t os_pread(int h, void *buf, size_t n, int64_t off) {
    CRITICAL_SECTION *l = file_lock(h); EnterCriticalSection(l);
    int64_t keep = _lseeki64(h, 0, 1), r;
    if (_lseeki64(h, off, 0) < 0) r = -errno; else r = read_all(h, buf, n);
    _lseeki64(h, keep, 0);
    LeaveCriticalSection(l);
    return r;
}
int64_t os_pwrite(int h, const void *buf, size_t n, int64_t off) {
    CRITICAL_SECTION *l = file_lock(h); EnterCriticalSection(l);
    int64_t keep = _lseeki64(h, 0, 1), r;
    if (_lseeki64(h, off, 0) < 0) r = -errno; else r = write_all(h, buf, n);
    _lseeki64(h, keep, 0);
    LeaveCriticalSection(l);
    return r;
}
// A crash of the runner itself (not of the game, which is reported with its own stack): Windows only shows
// "runner.exe has stopped working". Leave what it was and where in runner.log, so that a report can be read.
static LONG WINAPI own_crash(EXCEPTION_POINTERS *x) {
    static LONG once;
    if (InterlockedExchange(&once, 1)) return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t at = (uintptr_t)x->ExceptionRecord->ExceptionAddress, base = (uintptr_t)GetModuleHandleW(NULL);
    HMODULE in = NULL; char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)at, &in) && in)
        GetModuleFileNameA(in, name, sizeof name);
    const char *leaf = strrchr(name, '\\'); leaf = leaf ? leaf + 1 : name;
    rlog("[runner] the runner itself stopped: exception 0x%08lx at %p (%s+0x%llx; runner.exe is at %p)",
         (unsigned long)x->ExceptionRecord->ExceptionCode, (void *)at, leaf, (unsigned long long)(in ? at - (uintptr_t)in : 0), (void *)base);
    if (x->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && x->ExceptionRecord->NumberParameters >= 2)
        rlog(", %s address %p", x->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading", (void *)x->ExceptionRecord->ExceptionInformation[1]);
    rlog("\n[runner] 0xc000001d means this processor lacks an instruction the program uses; 0xc0000005 is a bad memory access\n");
    return EXCEPTION_CONTINUE_SEARCH;
}
// A game should not be slowed because another window is in front or the PC is saving power.
void os_tune_process(void) {
    SetUnhandledExceptionFilter(own_crash);
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
    typedef struct { ULONG Version, ControlMask, StateMask; } Throttling;
    BOOL (WINAPI *set_info)(HANDLE, int, LPVOID, DWORD) = (void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetProcessInformation");
    if (set_info) { Throttling t = {1, 1, 0}; set_info(GetCurrentProcess(), 4 /* ProcessPowerThrottling */, &t, sizeof t); }   // never "efficiency mode"
}
static int64_t write_all(int h, const void *buf, size_t n) {
    int64_t total = 0;
    while (n) { unsigned c = n > (1u << 30) ? (1u << 30) : (unsigned)n; int r = _write(h, buf, c); if (r < 0) return total ? total : -errno; total += r; buf = (const char *)buf + r; n -= r; }
    return total;
}
int64_t os_write(int h, const void *buf, size_t n) {
    CRITICAL_SECTION *l = file_lock(h); EnterCriticalSection(l);
    int64_t r = write_all(h, buf, n);
    LeaveCriticalSection(l);
    return r;
}
void os_close(int h) { _close(h); }
typedef struct { HANDLE h; WIN32_FIND_DATAW fd; int first; } WDir;
void *os_opendir(const char *path) {
    wchar_t w[1200], pat[1210];
    DWORD attr = GetFileAttributesW(wide(path, w, 1200));
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) return NULL;
    _snwprintf(pat, 1210, L"%ls/*", w);
    WDir *d = calloc(1, sizeof *d);
    d->h = FindFirstFileW(pat, &d->fd);
    d->first = d->h != INVALID_HANDLE_VALUE;
    return d;
}
int os_readdir(void *dv, char *name, size_t cap, int *is_dir) {
    WDir *d = dv;
    for (;;) {
        if (d->h == INVALID_HANDLE_VALUE) return 0;
        if (!d->first && !FindNextFileW(d->h, &d->fd)) return 0;
        d->first = 0;
        if (!wcscmp(d->fd.cFileName, L".") || !wcscmp(d->fd.cFileName, L"..")) continue;
        WideCharToMultiByte(CP_UTF8, 0, d->fd.cFileName, -1, name, (int)cap, NULL, NULL);
        *is_dir = (d->fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        return 1;
    }
}
void os_closedir(void *dv) { WDir *d = dv; if (d->h != INVALID_HANDLE_VALUE) FindClose(d->h); free(d); }
BOOLEAN NTAPI SystemFunction036(PVOID, ULONG);
void os_random(void *buf, size_t n) { SystemFunction036(buf, (ULONG)n); }
int os_utc_offset_seconds(void) {
    TIME_ZONE_INFORMATION tz;
    DWORD r = GetTimeZoneInformation(&tz);
    return -(int)(tz.Bias + (r == TIME_ZONE_ID_DAYLIGHT ? tz.DaylightBias : tz.StandardBias)) * 60;
}
FILE *os_fopen(const char *path, const char *mode) { wchar_t w[1200], m[8]; return _wfopen(wide(path, w, 1200), wide(mode, m, 8)); }
void os_args(int *argc, char ***argv) {                 // the command line as UTF-8, whatever the system code page is
    int n = 0;
    wchar_t **wv = CommandLineToArgvW(GetCommandLineW(), &n);
    if (!wv || n <= 0) return;
    char **out = calloc((size_t)n + 1, sizeof *out);
    for (int i = 0; i < n; i++) { int len = WideCharToMultiByte(CP_UTF8, 0, wv[i], -1, NULL, 0, NULL, NULL); out[i] = malloc((size_t)len); WideCharToMultiByte(CP_UTF8, 0, wv[i], -1, out[i], len, NULL, NULL); }
    *argc = n; *argv = out;
}
void os_open_url(const char *url) { wchar_t w[2048]; ShellExecuteW(NULL, L"open", wide(url, w, 2048), NULL, NULL, SW_SHOWNORMAL); }

// A minimal modal text prompt, for the game's name-entry style fields.
static wchar_t prompt_text[512]; static int prompt_ok; static HWND prompt_edit;
static LRESULT CALLBACK prompt_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_COMMAND && (LOWORD(w) == 1 || LOWORD(w) == 2)) {
        if (LOWORD(w) == 1) { GetWindowTextW(prompt_edit, prompt_text, 512); prompt_ok = 1; }
        DestroyWindow(h); return 0;
    }
    if (m == WM_CLOSE) { DestroyWindow(h); return 0; }
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}
int os_text_input(const char *title, const char *initial, char *out, size_t cap) {
    static int registered;
    HINSTANCE inst = GetModuleHandleW(NULL);
    if (!registered) { WNDCLASSW wc = {0}; wc.lpfnWndProc = prompt_proc; wc.hInstance = inst; wc.lpszClassName = L"FfbePrompt"; wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); wc.hCursor = LoadCursor(NULL, IDC_ARROW); RegisterClassW(&wc); registered = 1; }
    wchar_t wt[256], wi[512];
    wide(title && *title ? title : "Enter text", wt, 256); wide(initial ? initial : "", wi, 512);
    prompt_ok = 0;
    HWND w = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, L"FfbePrompt", wt, WS_CAPTION | WS_SYSMENU | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 380, 150, NULL, NULL, inst, NULL);
    prompt_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", wi, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 16, 16, 332, 26, w, NULL, inst, NULL);
    CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 176, 60, 80, 28, w, (HMENU)1, inst, NULL);
    CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE, 268, 60, 80, 28, w, (HMENU)2, inst, NULL);
    SetFocus(prompt_edit);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) { SendMessageW(w, WM_COMMAND, 1, 0); continue; }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) { SendMessageW(w, WM_COMMAND, 2, 0); continue; }
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    if (prompt_ok) WideCharToMultiByte(CP_UTF8, 0, prompt_text, -1, out, (int)cap, NULL, NULL);
    return prompt_ok;
}

#else
#include <dirent.h>
#include <sys/mman.h>
#include <unistd.h>

uint8_t *os_reserve(size_t size) { void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0); return p == MAP_FAILED ? NULL : p; }
void os_commit(void *p, size_t len) { (void)p; (void)len; }
void os_release(void *p, size_t len) { madvise(p, len, MADV_DONTNEED); }
void os_guard(void *p, size_t len) { mprotect(p, len, PROT_NONE); }
int os_open(const char *path, int lf, int mode) {
    int f = lf & 3;
    if (lf & 0x40) f |= O_CREAT; if (lf & 0x80) f |= O_EXCL; if (lf & 0x200) f |= O_TRUNC; if (lf & 0x400) f |= O_APPEND;
    int h = open(path, f, mode);
    return h < 0 ? -errno : h;
}
int os_stat(const char *path, void *out) { struct stat st; if (stat(path, &st) != 0) return -errno; fill_stat(out, S_ISDIR(st.st_mode), st.st_size, st.st_mtime); return 0; }
int os_fstat(int h, void *out) { struct stat st; if (fstat(h, &st) != 0) return -errno; fill_stat(out, S_ISDIR(st.st_mode), st.st_size, st.st_mtime); return 0; }
int os_mkdir(const char *p) { return mkdir(p, 0755) ? -errno : 0; }
int os_unlink(const char *p) { return unlink(p) ? -errno : 0; }
int os_rmdir(const char *p) { return rmdir(p) ? -errno : 0; }
int os_rename(const char *a, const char *b) { return rename(a, b) ? -errno : 0; }
int os_truncate(int h, int64_t len) { return ftruncate(h, len) ? -errno : 0; }
int64_t os_lseek(int h, int64_t off, int whence) { int64_t r = lseek(h, off, whence); return r < 0 ? -errno : r; }
int64_t os_read(int h, void *buf, size_t n) { int64_t r = read(h, buf, n); return r < 0 ? -errno : r; }
int64_t os_write(int h, const void *buf, size_t n) { int64_t r = write(h, buf, n); return r < 0 ? -errno : r; }
int64_t os_pread(int h, void *buf, size_t n, int64_t off) { int64_t r = pread(h, buf, n, off); return r < 0 ? -errno : r; }
int64_t os_pwrite(int h, const void *buf, size_t n, int64_t off) { int64_t r = pwrite(h, buf, n, off); return r < 0 ? -errno : r; }
void os_files_init(void) {}
void os_tune_process(void) {}
void os_close(int h) { close(h); }
void *os_opendir(const char *path) { return opendir(path); }
int os_readdir(void *d, char *name, size_t cap, int *is_dir) {
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(name, cap, "%s", e->d_name);
        *is_dir = e->d_type == DT_DIR;
        return 1;
    }
    return 0;
}
void os_closedir(void *d) { closedir(d); }
void os_random(void *buf, size_t n) { FILE *f = fopen("/dev/urandom", "rb"); if (f) { if (fread(buf, 1, n, f) != n) memset(buf, 7, n); fclose(f); } }
int os_utc_offset_seconds(void) { time_t t = time(NULL); struct tm l; localtime_r(&t, &l); return (int)l.tm_gmtoff; }
FILE *os_fopen(const char *path, const char *mode) { return fopen(path, mode); }
void os_args(int *argc, char ***argv) { (void)argc; (void)argv; }
void os_open_url(const char *url) { rlog("[runner] the game asked to open %s\n", url); }
int os_text_input(const char *title, const char *initial, char *out, size_t cap) {
    const char *v = getenv("FFBE_TEXT_INPUT");
    rlog("[runner] text prompt \"%s\" (initial \"%s\") answered from FFBE_TEXT_INPUT\n", title ? title : "", initial ? initial : "");
    if (!v) return 0;
    snprintf(out, cap, "%s", v);
    return 1;
}
#endif
