// Runner: executes an ARM64 Linux program (the game loader plus the game's own library) on any 64-bit host.
//  - the ARM64 code runs in the Unicorn CPU emulator
//  - Linux system calls made by that code are carried out here on the host
//  - the program's threads are run one at a time and switched here, so one host thread owns the CPU emulator,
//    the window and the GL context
//  - graphics, sound, input and HTTP arrive as extra calls (hc.h) and are handled by the host_*.c files
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL.h>

#include "runner.h"
#include "ui.h"

uint8_t *arena;
char g_root[1024];
int g_quit, g_exit_code;
static FILE *g_logf;
static int g_verbose;

void rlog(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[4096];
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fputs(buf, stderr);
    if (g_logf) { fputs(buf, g_logf); fflush(g_logf); }
}

uint64_t now_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec; }
uint64_t wall_ns(void) { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec; }

// ---------------------------------------------------------------------------------------------------------
// Guest memory
// ---------------------------------------------------------------------------------------------------------
#define NPAGES (ARENA_SIZE / PAGE)
static uint8_t *page_used;
static uint64_t brk_start, brk_cur, mmap_hint = MMAP_LO;
static uint8_t scratch_zero[64];

static void report_crash(const char *why);

void *g2h(uint64_t a) {
    if (!a) return NULL;
    if (a < GUEST_BASE || a >= GUEST_BASE + ARENA_SIZE) {
        rlog("[runner] guest pointer outside memory: 0x%llx\n", (unsigned long long)a);
        report_crash("bad pointer passed to the host");
        exit(70);
    }
    return arena + (a - GUEST_BASE);
}
const char *gstr(uint64_t a) { return (const char *)g2h(a); }

static void pages_set(uint64_t addr, uint64_t len, int used) {
    uint64_t p0 = (addr - GUEST_BASE) / PAGE, n = (len + PAGE - 1) / PAGE;
    memset(page_used + p0, used, n);
}
static uint64_t mmap_find(uint64_t len) {
    uint64_t n = (len + PAGE - 1) / PAGE, lo = (MMAP_LO - GUEST_BASE) / PAGE, start = (mmap_hint - GUEST_BASE) / PAGE;
    for (int pass = 0; pass < 2; pass++) {
        uint64_t run = 0, i = pass ? lo : start;
        for (; i < NPAGES; i++) {
            if (page_used[i]) run = 0;
            else if (++run == n) { uint64_t first = i + 1 - n; mmap_hint = GUEST_BASE + (i + 1) * PAGE; return GUEST_BASE + first * PAGE; }
        }
    }
    return 0;
}
uint64_t guest_alloc(size_t len) {
    len = (len + PAGE - 1) & ~(PAGE - 1);
    uint64_t a = mmap_find(len);
    if (!a) return 0;
    pages_set(a, len, 1);
    os_commit(g2h(a), len);
    return a;
}
static void guest_free(uint64_t a, uint64_t len) {
    len = (len + PAGE - 1) & ~(PAGE - 1);
    if (a < MMAP_LO || a + len > GUEST_BASE + ARENA_SIZE) return;
    pages_set(a, len, 0);
    os_release(g2h(a), len);
    if (a < mmap_hint) mmap_hint = a;
}

// ---------------------------------------------------------------------------------------------------------
// Symbols for crash reports: the loader program's own table, and the game library once it reports itself.
// ---------------------------------------------------------------------------------------------------------
typedef struct { uint64_t addr, size; uint32_t name; } Sym;
typedef struct { Sym *syms; size_t n; char *str; uint64_t bias, lo, hi; char label[32]; } SymTab;
static SymTab symtabs[2];

static void load_syms(SymTab *t, const char *path, uint64_t bias, uint32_t want_type, const char *label) {
    FILE *f = os_fopen(path, "rb");
    if (!f) return;
    unsigned char eh[64];
    if (fread(eh, 1, 64, f) != 64) { fclose(f); return; }
    uint64_t shoff; uint16_t shnum, shentsize;
    memcpy(&shoff, eh + 0x28, 8); memcpy(&shentsize, eh + 0x3A, 2); memcpy(&shnum, eh + 0x3C, 2);
    unsigned char *sh = malloc((size_t)shnum * shentsize);
    fseek(f, (long)shoff, SEEK_SET);
    if (fread(sh, shentsize, shnum, f) != shnum) { fclose(f); free(sh); return; }
    for (int i = 0; i < shnum; i++) {
        unsigned char *s = sh + (size_t)i * shentsize;
        uint32_t type, link; uint64_t off, size;
        memcpy(&type, s + 4, 4); memcpy(&off, s + 0x18, 8); memcpy(&size, s + 0x20, 8); memcpy(&link, s + 0x28, 4);
        if (type != want_type) continue;
        unsigned char *ls = sh + (size_t)link * shentsize;
        uint64_t soff, ssize;
        memcpy(&soff, ls + 0x18, 8); memcpy(&ssize, ls + 0x20, 8);
        unsigned char *raw = malloc(size);
        t->str = malloc(ssize + 1);
        fseek(f, (long)off, SEEK_SET); if (fread(raw, 1, size, f) != size) break;
        fseek(f, (long)soff, SEEK_SET); if (fread(t->str, 1, ssize, f) != ssize) break;
        t->str[ssize] = 0;
        size_t n = size / 24;
        t->syms = malloc(n * sizeof(Sym));
        for (size_t k = 0; k < n; k++) {
            unsigned char *e = raw + k * 24;
            uint32_t name; uint16_t shndx; uint64_t value, sz;
            memcpy(&name, e, 4); memcpy(&shndx, e + 6, 2); memcpy(&value, e + 8, 8); memcpy(&sz, e + 16, 8);
            if (!shndx || (e[4] & 0xF) != 2) continue;      // defined functions only
            t->syms[t->n++] = (Sym){value, sz, name};
        }
        free(raw);
        break;
    }
    t->bias = bias;
    snprintf(t->label, sizeof t->label, "%s", label);
    free(sh);
    fclose(f);
}
void register_image(uint64_t base, uint64_t size, const char *host_path) {
    load_syms(&symtabs[1], host_path, base, 11 /* SHT_DYNSYM */, "libgame");
    symtabs[1].lo = base; symtabs[1].hi = base + size;
}
void symbolize(uint64_t addr, char *out, size_t cap) {
    for (int k = 1; k >= 0; k--) {
        SymTab *t = &symtabs[k];
        if (!t->n || addr < t->lo || addr >= t->hi) continue;
        uint64_t rel = addr - t->bias, best = 0; const char *name = NULL;
        for (size_t i = 0; i < t->n; i++) if (t->syms[i].addr <= rel && t->syms[i].addr >= best) { best = t->syms[i].addr; name = t->str + t->syms[i].name; }
        if (name) { snprintf(out, cap, "%s+0x%llx  %s+0x%llx", t->label, (unsigned long long)rel, name, (unsigned long long)(rel - best)); return; }
    }
    snprintf(out, cap, "0x%llx", (unsigned long long)addr);
}

// ---------------------------------------------------------------------------------------------------------
// Guest threads
// ---------------------------------------------------------------------------------------------------------
enum { T_FREE, T_RUN, T_FUTEX, T_SLEEP, T_JOB, T_DEAD };
typedef struct {
    int state, tid;
    CpuCtx *ctx;
    uint64_t futex, wake_ns, clear_tid;
    int has_timeout, job, set_ret;
    int64_t ret;
    int urgent;                  // sound threads: scheduled ahead of the others
    int running;                 // on a core right now
    int last_core;               // where it last ran: staying there avoids recompiling its code on another core
} GThread;
#define MAXT 256
#define MAXCORES 8
static GThread threads[MAXT];
// Cores. Each is one host thread running the scheduler below. Guest code runs without the lock; the lock covers the
// thread table, files, memory map and everything else the system calls touch.
static int n_cores = 1;
static SDL_mutex *big;
static SDL_cond *work;                       // signalled when a thread becomes runnable
static GThread *main_thread;                 // the game's first thread: owns the window, stays on core 0
static GThread *core_thread[MAXCORES];
static uint64_t slice_start[MAXCORES];
static __thread GThread *cur;
static __thread int my_core;
static int next_tid = 1000;
static unsigned long n_syscalls, n_hypercalls, n_switches;
static uint64_t last_present_ns, worst_gap;
static unsigned long n_frames, n_late;

static GThread *thread_new(void) {
    for (int i = 0; i < MAXT; i++) if (threads[i].state == T_FREE) {
        GThread *t = &threads[i];
        memset(t, 0, sizeof *t);
        t->tid = next_tid++;
        t->ctx = cpu_ctx_new();
        t->state = T_RUN;
        return t;
    }
    fatal("too many guest threads");
    return NULL;
}
static __thread int want_switch;
static void block(int state) { cur->state = state; }
void block_current_sleep(uint64_t until) { cur->wake_ns = until; cur->ret = 0; cur->set_ret = 1; block(T_SLEEP); }
void block_current_on_job(int job) { cur->job = job; cur->ret = job; cur->set_ret = 1; block(T_JOB); }

static int futex_wake(uint64_t addr, int n) {
    int woke = 0;
    for (int i = 0; i < MAXT && woke < n; i++)
        if (threads[i].state == T_FUTEX && threads[i].futex == addr) { threads[i].state = T_RUN; threads[i].ret = 0; threads[i].set_ret = 1; woke++; }
    if (woke) SDL_CondBroadcast(work);
    return woke;
}
static int wake_due(void) {
    uint64_t now = now_ns();
    int woke = 0;
    for (int i = 0; i < MAXT; i++) {
        GThread *t = &threads[i];
        if (t->state == T_SLEEP && now >= t->wake_ns) { t->state = T_RUN; woke++; }
        else if (t->state == T_FUTEX && t->has_timeout && now >= t->wake_ns) { t->state = T_RUN; t->ret = -110; t->set_ret = 1; woke++; }
        else if (t->state == T_JOB && http_done(t->job)) { t->state = T_RUN; woke++; }
    }
    if (woke) SDL_CondBroadcast(work);
    return woke;
}

static void report_crash(const char *why) {
    uint64_t pc = cpu_pc(), lr = cpu_x(30), sp = cpu_sp(), fp = cpu_x(29), x[4] = {cpu_x(0), cpu_x(1), cpu_x(2), cpu_x(3)};
    char s[512];
    rlog("\n*** guest stopped: %s (thread %d)\n", why, cur ? cur->tid : -1);
    symbolize(pc, s, sizeof s); rlog("  pc  %s\n", s);
    symbolize(lr, s, sizeof s); rlog("  lr  %s\n", s);
    rlog("  sp 0x%llx  x0 0x%llx  x1 0x%llx  x2 0x%llx  x3 0x%llx\n", (unsigned long long)sp, (unsigned long long)x[0],
         (unsigned long long)x[1], (unsigned long long)x[2], (unsigned long long)x[3]);
    for (int i = 0; i < 24 && fp >= GUEST_BASE && fp + 16 < GUEST_BASE + ARENA_SIZE; i++) {
        uint64_t *f = (uint64_t *)(arena + (fp - GUEST_BASE));
        symbolize(f[1], s, sizeof s); rlog("    at %s\n", s);
        if (f[0] <= fp) break;
        fp = f[0];
    }
    if (!getenv("FFBE_CONTROL")) {                 // tell the person, unless a test script is driving the game
        char msg[1400];
        snprintf(msg, sizeof msg, "The game stopped unexpectedly.\n\nThe details were written to:\n%s/runner.log", g_root);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Exvius Revival Desktop", msg, NULL);
    }
}
void fatal(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char buf[1024]; vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    rlog("[runner] fatal: %s\n", buf);
    if (cur) report_crash(buf);
    exit(70);
}

// ---------------------------------------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------------------------------------
enum { FD_FREE, FD_STD, FD_FILE, FD_DIR, FD_NULL };
typedef struct { int kind, hfd; void *dir, *dir2; char path[1024]; char pend[512]; int pend_dir, has_pend; } GFd;   // dir2: the server's side of a merged folder
#define MAXFD 512
static GFd fds[MAXFD];

int guest_path(uint64_t gaddr, char *out, size_t cap) {
    const char *p = gstr(gaddr);
    if (!p) return -14;
    if (!strncmp(p, GUEST_ROOT, 5) && (p[5] == '/' || !p[5])) snprintf(out, cap, "%s%s", g_root, p + 5);
    else if (p[0] != '/') snprintf(out, cap, "%s/%s", g_root, p);
    else return -2;
    // The game writes some paths with a doubled slash ("area//Ver1_x.cpk"). One slash, so names can be compared.
    char *w = out + (out[0] && out[1] ? 2 : 0);
    for (char *r = w; *r; r++) if (!(*r == '/' && w > out && w[-1] == '/')) *w++ = *r;
    *w = 0;
    return 0;
}
static int fd_alloc(void) { for (int i = 3; i < MAXFD; i++) if (fds[i].kind == FD_FREE) return i; return -24; }

// ---------------------------------------------------------------------------------------------------------
// Reading the server's data in place.
// The game keeps what it downloads under <root>/game/files, in the same layout as the server's data folder
// (FFBE_OVERLAY). When a file is missing from the game's folder but present in the server's, the game is
// shown the server's copy, read-only, so it never has to download and store a second copy. Anything the
// game writes goes to its own folder and wins from then on; anything it deletes from the server's side is
// only hidden (listed in game/files-removed.txt), never deleted there.
// ---------------------------------------------------------------------------------------------------------
static char g_lower[1024], g_upper[1100], g_removed_path[1100];
static char **removed; static int n_removed, cap_removed;
static int host_exists(const char *p) { uint8_t st[160]; return os_stat(p, st) == 0; }
static const char *overlay_rel(const char *host) {          // the part after <root>/game/files/, or NULL
    size_t n = strlen(g_upper);
    if (!g_lower[0] || strncmp(host, g_upper, n) != 0) return NULL;
    return host[n] == '/' ? host + n + 1 : host[n] == 0 ? "" : NULL;
}
static int is_removed(const char *rel) {
    for (int i = 0; i < n_removed; i++) {
        size_t n = strlen(removed[i]);
        if (!strncmp(rel, removed[i], n) && (rel[n] == 0 || rel[n] == '/')) return 1;
    }
    return 0;
}
static void mark_removed(const char *rel) {
    if (is_removed(rel)) return;
    if (n_removed == cap_removed) { cap_removed = cap_removed ? cap_removed * 2 : 64; removed = realloc(removed, sizeof *removed * (size_t)cap_removed); }
    removed[n_removed++] = strdup(rel);
    FILE *f = os_fopen(g_removed_path, "a");
    if (f) { fprintf(f, "%s\n", rel); fclose(f); }
}
// Files the server makes itself at start-up and keeps outside its data folder (the Vortex banner pack, 242 MB, rebuilt
// when the banners change). The server says where they are (/__desktop/files: "name in the game's folder <tab> file on
// this computer" per line), and the game is shown them in place like the rest instead of downloading a copy each time.
static struct { char rel[300], host[1100]; } extra[16];
static int n_extra;
static SDL_atomic_t extra_ready;
static int extra_thread(void *ud) {
    (void)ud;
    uint8_t *body = NULL; size_t len = 0;
    if (http_fetch("http://local/__desktop/files", "GET", NULL, NULL, 0, &body, &len) == 200 && body) {
        for (char *line = strtok((char *)body, "\n"); line && n_extra < 16; line = strtok(NULL, "\n")) {
            char *tab = strchr(line, '\t');
            if (!tab) continue;
            *tab = 0;
            line[strcspn(line, "\r")] = 0; tab[1 + strcspn(tab + 1, "\r")] = 0;
            snprintf(extra[n_extra].rel, sizeof extra[n_extra].rel, "%s", line);
            snprintf(extra[n_extra].host, sizeof extra[n_extra].host, "%s", tab + 1);
            for (char *q = extra[n_extra].host; *q; q++) if (*q == '\\') *q = '/';
            rlog("[files] %s is read in place from %s\n", extra[n_extra].rel, extra[n_extra].host);
            n_extra++;
        }
    }
    free(body);
    SDL_AtomicSet(&extra_ready, 1);
    return 0;
}
static int extra_of(const char *rel, char *out, size_t cap) {
    if (!strstr(rel, ".cpk")) return 0;
    for (int waited = 0; !SDL_AtomicGet(&extra_ready) && waited < 120000 && strstr(rel, "spdungeon1"); waited += 20) SDL_Delay(20);   // still being built
    for (int i = 0; i < n_extra; i++) if (!strcmp(rel, extra[i].rel)) { snprintf(out, cap, "%s", extra[i].host); return host_exists(out); }
    return 0;
}
// The game has a copy of its own again (it downloaded or wrote the file): the name is no longer hidden. Without this, a
// file the game deletes just before downloading it afresh would stay hidden on the server's side for good.
static void unmark_removed(const char *host) {
    const char *rel = overlay_rel(host);
    if (!rel || !*rel) return;
    int found = 0;
    for (int i = 0; i < n_removed; i++) if (!strcmp(removed[i], rel)) { free(removed[i]); removed[i] = removed[--n_removed]; found = 1; i--; }
    if (!found) return;
    FILE *f = os_fopen(g_removed_path, "w");
    if (f) { for (int i = 0; i < n_removed; i++) fprintf(f, "%s\n", removed[i]); fclose(f); }
}
void overlay_file_written(const char *host) { SDL_LockMutex(big); unmark_removed(host); SDL_UnlockMutex(big); }
static int lower_of(const char *host, char *out, size_t cap) {       // 1 when the server's folder has this path and it is not hidden
    const char *rel = overlay_rel(host);
    if (!rel || is_removed(rel)) return 0;
    snprintf(out, cap, "%s%s%s", g_lower, *rel ? "/" : "", rel);
    return host_exists(out) || extra_of(rel, out, cap);
}
static void resolve_read(char *host, size_t cap) {                    // use the server's copy when the game has none
    char l[1200];
    if (g_lower[0] && !host_exists(host) && lower_of(host, l, sizeof l)) snprintf(host, cap, "%s", l);
}
static void make_upper_parents(const char *host) {                    // folders that exist only on the server's side
    if (!overlay_rel(host)) return;
    char p[1100]; snprintf(p, sizeof p, "%s", host);
    for (char *s = p + strlen(g_upper) + 1; *s; s++) if (*s == '/') { *s = 0; os_mkdir(p); *s = '/'; }
}
static void copy_up(const char *host) {                               // the game wants to change a file it only has on the server's side
    char l[1200];
    if (host_exists(host) || !lower_of(host, l, sizeof l)) return;
    make_upper_parents(host);
    int in = os_open(l, 0, 0), out = in >= 0 ? os_open(host, 1 | 0x40 | 0x200, 0644) : -1;
    if (in >= 0 && out >= 0) { static char buf[1 << 16]; int64_t n; while ((n = os_read(in, buf, sizeof buf)) > 0) os_write(out, buf, (size_t)n); }
    if (in >= 0) os_close(in);
    if (out >= 0) os_close(out);
}
static void overlay_init(void) {
    const char *l = getenv("FFBE_OVERLAY");
    snprintf(g_upper, sizeof g_upper, "%s/game/files", g_root);
    snprintf(g_removed_path, sizeof g_removed_path, "%s/game/files-removed.txt", g_root);
    if (!l || !*l) return;
    snprintf(g_lower, sizeof g_lower, "%s", l);
    for (char *p = g_lower; *p; p++) if (*p == '\\') *p = '/';
    size_t n = strlen(g_lower);
    if (n && g_lower[n - 1] == '/') g_lower[n - 1] = 0;
    if (!host_exists(g_lower)) { rlog("[files] %s not found; the game will download its files instead\n", g_lower); g_lower[0] = 0; return; }
    FILE *f = os_fopen(g_removed_path, "r");
    if (f) {
        char line[1100];
        while (fgets(line, sizeof line, f)) { line[strcspn(line, "\r\n")] = 0; if (line[0]) { if (n_removed == cap_removed) { cap_removed = cap_removed ? cap_removed * 2 : 64; removed = realloc(removed, sizeof *removed * (size_t)cap_removed); } removed[n_removed++] = strdup(line); } }
        fclose(f);
    }
    rlog("[files] reading game data in place from %s (%d hidden)\n", g_lower, n_removed);
}
static void overlay_extras(void) {                                    // once the server's address is known
    if (!g_lower[0] || !g_server[0]) { SDL_AtomicSet(&extra_ready, 1); return; }
    SDL_Thread *t = SDL_CreateThread(extra_thread, "files", NULL);
    if (t) SDL_DetachThread(t); else SDL_AtomicSet(&extra_ready, 1);
}

static int64_t sys_openat(uint64_t gpath, int flags) {
    const char *p = gstr(gpath);
    int fd = fd_alloc();
    if (fd < 0) return fd;
    if (p && (!strcmp(p, "/dev/urandom") || !strcmp(p, "/dev/random") || !strcmp(p, "/dev/null"))) { fds[fd].kind = FD_NULL; fds[fd].hfd = p[5] == 'n' ? 0 : 1; return fd; }
    char host[1100];
    int e = guest_path(gpath, host, sizeof host);
    if (e) { if (g_verbose > 1) rlog("[open] outside the game folder: %s\n", p); return e; }
    char low[1200];
    void *d = os_opendir(host), *d2 = lower_of(host, low, sizeof low) ? os_opendir(low) : NULL;
    if (d || d2) { fds[fd].kind = FD_DIR; fds[fd].dir = d; fds[fd].dir2 = d2; fds[fd].has_pend = 0; snprintf(fds[fd].path, sizeof fds[fd].path, "%s", host); return fd; }
    if (flags & 0x4000) return -20;
    if (flags & (3 | 0x40 | 0x200 | 0x400)) {            // writing: always in the game's own folder
        if (!(flags & 0x200)) copy_up(host);
        make_upper_parents(host);
        if (flags & 0x40) unmark_removed(host);
    } else resolve_read(host, sizeof host);
    int h = os_open(host, flags, 0644);
    if (g_verbose > 1) rlog("[open] %s -> %d\n", p, h);
    if (h < 0) return h;
    fds[fd].kind = FD_FILE; fds[fd].hfd = h;
    snprintf(fds[fd].path, sizeof fds[fd].path, "%s", host);
    return fd;
}
static int64_t sys_read(int fd, void *buf, size_t n) {
    if (fd < 0 || fd >= MAXFD) return -9;
    if (fd == 0) return 0;
    if (fds[fd].kind == FD_NULL) { if (!fds[fd].hfd) return 0; os_random(buf, n); return (int64_t)n; }
    if (fds[fd].kind != FD_FILE) return -9;
    // A read from disk can take a while (the first read of a big pack especially): let the other cores carry on.
    int h = fds[fd].hfd;
    SDL_UnlockMutex(big);
    int64_t r = os_read(h, buf, n);
    SDL_LockMutex(big);
    return r;
}
static int64_t sys_write(int fd, const void *buf, size_t n) {
    if (fd == 1 || fd == 2) { fwrite(buf, 1, n, stderr); if (g_logf) { fwrite(buf, 1, n, g_logf); fflush(g_logf); } return (int64_t)n; }
    if (fd < 0 || fd >= MAXFD) return -9;
    if (fds[fd].kind == FD_NULL) return (int64_t)n;
    if (fds[fd].kind != FD_FILE) return -9;
    return os_write(fds[fd].hfd, buf, n);
}
static int64_t sys_getdents(int fd, uint8_t *buf, size_t cap) {
    if (fd < 0 || fd >= MAXFD || fds[fd].kind != FD_DIR) return -9;
    size_t off = 0;
    for (;;) {
        char name[512]; int is_dir;
        if (fds[fd].has_pend) { strcpy(name, fds[fd].pend); is_dir = fds[fd].pend_dir; fds[fd].has_pend = 0; }
        else if (fds[fd].dir && os_readdir(fds[fd].dir, name, sizeof name, &is_dir)) {}
        else {                                           // then the server's side: names the game's folder lacks and has not hidden
            int got = 0;
            while (fds[fd].dir2 && os_readdir(fds[fd].dir2, name, sizeof name, &is_dir)) {
                char up[1700]; snprintf(up, sizeof up, "%s/%s", fds[fd].path, name);
                const char *rel = overlay_rel(up);
                if (!strcmp(name, ".") || !strcmp(name, "..")) { if (fds[fd].dir) continue; got = 1; break; }
                if (host_exists(up) || (rel && is_removed(rel))) continue;
                got = 1; break;
            }
            if (!got) break;
        }
        size_t reclen = (19 + strlen(name) + 1 + 7) & ~7UL;
        if (off + reclen > cap) { strcpy(fds[fd].pend, name); fds[fd].pend_dir = is_dir; fds[fd].has_pend = 1; break; }
        uint64_t ino = 1000 + off; int64_t doff = (int64_t)(off + reclen); uint16_t rl = (uint16_t)reclen;
        memcpy(buf + off, &ino, 8); memcpy(buf + off + 8, &doff, 8); memcpy(buf + off + 16, &rl, 2);
        buf[off + 18] = is_dir ? 4 : 8;
        strcpy((char *)buf + off + 19, name);
        off += reclen;
    }
    return (int64_t)off;
}
static int64_t sys_close(int fd) {
    if (fd < 3 || fd >= MAXFD || fds[fd].kind == FD_FREE) return fd >= 0 && fd < 3 ? 0 : -9;
    if (fds[fd].kind == FD_FILE) os_close(fds[fd].hfd);
    if (fds[fd].kind == FD_DIR) { if (fds[fd].dir) os_closedir(fds[fd].dir); if (fds[fd].dir2) os_closedir(fds[fd].dir2); fds[fd].dir = fds[fd].dir2 = NULL; }
    fds[fd].kind = FD_FREE;
    return 0;
}

// ---------------------------------------------------------------------------------------------------------
// System calls
// ---------------------------------------------------------------------------------------------------------
static void put_ts(uint64_t gaddr, uint64_t ns) { int64_t *p = g2h(gaddr); if (p) { p[0] = (int64_t)(ns / 1000000000ULL); p[1] = (int64_t)(ns % 1000000000ULL); } }
static uint64_t get_ts(uint64_t gaddr) { int64_t *p = g2h(gaddr); return p ? (uint64_t)p[0] * 1000000000ULL + (uint64_t)p[1] : 0; }
static unsigned char unknown_seen[512];

static int64_t do_syscall(uint64_t nr, uint64_t *a) {
    switch (nr) {
    case 17: { char *b = g2h(a[0]); if (a[1] < 6) return -34; strcpy(b, GUEST_ROOT); return 6; }
    case 23: case 24: return -24;
    case 25: return 0;                                   // fcntl
    case 29: return -25;                                 // ioctl: not a terminal
    case 32: case 82: case 83: return 0;                 // flock, fsync
    case 34: { char h[1100]; int e = guest_path(a[1], h, sizeof h); if (e) return e; make_upper_parents(h); return os_mkdir(h); }
    case 35: {                                           // unlinkat: the server's copy is hidden, never deleted
        char h[1100], l[1200]; int e = guest_path(a[1], h, sizeof h);
        if (e) return e;
        int below = lower_of(h, l, sizeof l);
        int r = (a[2] & 0x200) ? os_rmdir(h) : os_unlink(h);
        if (g_verbose > 1) rlog("[remove] %s -> %d%s\n", gstr(a[1]), r, below ? " (the server's copy is hidden instead)" : "");
        if (below) { mark_removed(overlay_rel(h)); return 0; }
        return r;
    }
    case 38: case 276: {
        char h1[1100], h2[1100], l[1200]; int e = guest_path(a[1], h1, sizeof h1); if (!e) e = guest_path(a[3], h2, sizeof h2);
        if (e) return e;
        int below = lower_of(h1, l, sizeof l);
        copy_up(h1); make_upper_parents(h2);
        int r = os_rename(h1, h2);
        if (!r && below) mark_removed(overlay_rel(h1));
        if (!r) unmark_removed(h2);
        return r;
    }
    case 43: case 44: { uint64_t *s = g2h(a[1]); memset(s, 0, 120); s[0] = 0xEF53; s[1] = 4096; s[2] = 1 << 26; s[3] = 1 << 25; s[4] = 1 << 25; s[8] = 255; return 0; }
    case 46: return (a[0] < MAXFD && fds[a[0]].kind == FD_FILE) ? os_truncate(fds[a[0]].hfd, (int64_t)a[1]) : -9;
    case 48: case 439: { char h[1300]; uint8_t st[160]; int e = guest_path(a[1], h, sizeof h); if (e) return e; resolve_read(h, sizeof h); return os_stat(h, st); }
    case 49: return 0;
    case 56: return sys_openat(a[1], (int)a[2]);
    case 57: return sys_close((int)a[0]);
    case 59: return -38;
    case 61: return sys_getdents((int)a[0], g2h(a[1]), a[2]);
    case 62: return ((int)a[0] >= 0 && a[0] < MAXFD && fds[a[0]].kind == FD_FILE) ? os_lseek(fds[a[0]].hfd, (int64_t)a[1], (int)a[2]) : -29;
    case 63: return sys_read((int)a[0], g2h(a[1]), a[2]);
    case 64: return sys_write((int)a[0], g2h(a[1]), a[2]);
    case 65: case 66: {
        uint64_t *iov = g2h(a[1]); int64_t total = 0;
        for (uint64_t i = 0; i < a[2]; i++) {
            if (!iov[2 * i + 1]) continue;
            int64_t r = nr == 65 ? sys_read((int)a[0], g2h(iov[2 * i]), iov[2 * i + 1]) : sys_write((int)a[0], g2h(iov[2 * i]), iov[2 * i + 1]);
            if (r < 0) return total ? total : r;
            total += r;
            if ((uint64_t)r < iov[2 * i + 1]) break;
        }
        return total;
    }
    case 67: case 68: {                                  // pread / pwrite: like read, without holding up the other cores
        if ((int)a[0] < 0 || a[0] >= MAXFD || fds[a[0]].kind != FD_FILE) return -9;
        int h = fds[a[0]].hfd;
        void *buf = g2h(a[1]);
        SDL_UnlockMutex(big);
        int64_t r = nr == 67 ? os_pread(h, buf, a[2], (int64_t)a[3]) : os_pwrite(h, buf, a[2], (int64_t)a[3]);
        SDL_LockMutex(big);
        return r;
    }
    case 72: case 73: {                                  // pselect6 / ppoll used as a sleep
        uint64_t tsp = nr == 72 ? a[4] : a[2];
        if (tsp) block_current_sleep(now_ns() + get_ts(tsp)); else block_current_sleep(now_ns() + 1000000);
        return 0;
    }
    case 78: return -22;
    case 79: {
        const char *p = gstr(a[1]);
        if ((!p || !*p) && (a[3] & 0x1000)) {
            int fd = (int)a[0];
            if (fd >= 0 && fd < 3) { uint32_t *s = g2h(a[2]); memset(s, 0, 128); s[4] = 0020620; return 0; }
            if (fd < 0 || fd >= MAXFD) return -9;
            if (fds[fd].kind == FD_FILE) return os_fstat(fds[fd].hfd, g2h(a[2]));
            if (fds[fd].kind == FD_DIR) { char h[1300]; snprintf(h, sizeof h, "%s", fds[fd].path); resolve_read(h, sizeof h); return os_stat(h, g2h(a[2])); }
            return -9;
        }
        char h[1300]; int e = guest_path(a[1], h, sizeof h);
        if (e) return e;
        resolve_read(h, sizeof h);
        int r = os_stat(h, g2h(a[2]));
        if (g_verbose > 1) rlog("[stat] %s -> %d\n", p, r);
        return r;
    }
    case 80: {
        int fd = (int)a[0];
        if (fd >= 0 && fd < 3) { uint32_t *s = g2h(a[1]); memset(s, 0, 128); s[4] = 0020620; return 0; }
        if (fd < 0 || fd >= MAXFD) return -9;
        if (fds[fd].kind == FD_FILE) return os_fstat(fds[fd].hfd, g2h(a[1]));
        if (fds[fd].kind == FD_DIR) return os_stat(fds[fd].path, g2h(a[1]));
        return -9;
    }
    case 93: {
        if (cur->clear_tid) { *(uint32_t *)g2h(cur->clear_tid) = 0; futex_wake(cur->clear_tid, 1); }
        block(T_DEAD);
        int alive = 0;
        for (int i = 0; i < MAXT; i++) if (threads[i].state != T_FREE && threads[i].state != T_DEAD) alive++;
        if (!alive) { g_quit = 1; g_exit_code = (int)a[0]; }
        return 0;
    }
    case 94: g_quit = 1; g_exit_code = (int)a[0]; block(T_DEAD); return 0;
    case 96: cur->clear_tid = a[0]; return cur->tid;
    case 98: {
        int op = (int)a[1] & 0x7F;
        uint32_t *u = g2h(a[0]);
        if (op == 0 || op == 9) {
            if (*u != (uint32_t)a[2]) return -11;
            cur->futex = a[0];
            cur->has_timeout = a[3] != 0;
            if (a[3]) {
                uint64_t t = get_ts(a[3]);
                if (op == 0) cur->wake_ns = now_ns() + t;
                else if ((int)a[1] & 256) { uint64_t w = wall_ns(); cur->wake_ns = now_ns() + (t > w ? t - w : 0); }
                else cur->wake_ns = t;
            }
            block(T_FUTEX);
            return 0;
        }
        if (op == 1 || op == 10) return futex_wake(a[0], (int)a[2]);
        if (op == 3 || op == 4) {
            if (op == 4 && *u != (uint32_t)a[5]) return -11;
            int n = futex_wake(a[0], (int)a[2]);
            for (int i = 0; i < MAXT; i++) if (threads[i].state == T_FUTEX && threads[i].futex == a[0]) threads[i].futex = a[4];
            return n;
        }
        if (op == 5) return futex_wake(a[0], (int)a[2]) + futex_wake(a[4], (int)a[3]);
        return -38;
    }
    case 99: return 0;
    case 101: block_current_sleep(now_ns() + get_ts(a[0])); return 0;
    case 113: case 114: {
        uint64_t t = nr == 114 ? 1 : (a[0] == 0 || a[0] == 5) ? wall_ns() : now_ns();
        put_ts(a[1], t);
        return 0;
    }
    case 115: {
        uint64_t t = get_ts(a[2]);
        if (a[1] & 1) { if (a[0] == 0) { uint64_t w = wall_ns(); t = now_ns() + (t > w ? t - w : 0); } }
        else t += now_ns();
        block_current_sleep(t);
        return 0;
    }
    case 122: return 0;
    case 123: { uint8_t *m = g2h(a[2]); size_t n = a[1] < 8 ? a[1] : 8; memset(m, 0, n); m[0] = 3; return (int64_t)n; }
    case 124: want_switch = 1; return 0;
    case 129: case 130: case 131: {
        int sig = (int)(nr == 131 ? a[2] : a[1]);
        if (sig == 6 || sig == 11) { report_crash("the guest program aborted"); g_quit = 1; g_exit_code = 134; block(T_DEAD); }
        return 0;
    }
    case 132: case 139: return 0;
    case 134: { if (a[2]) memset(g2h(a[2]), 0, 32); return 0; }
    case 135: { if (a[2]) memset(g2h(a[2]), 0, 8); return 0; }
    case 140: {                                          // setpriority(which, who, nice): the game raises its sound threads this way
        int who = (int)a[1], nice = (int)a[2];
        for (int i = 0; i < MAXT; i++) if (threads[i].state != T_FREE && (who == 0 ? &threads[i] == cur : threads[i].tid == who)) {
            threads[i].urgent = nice < 0;
            if (g_verbose) rlog("[sched] thread %d asks for priority %d%s\n", threads[i].tid, nice, threads[i].urgent ? " (treated as a sound thread)" : "");
        }
        return 0;
    }
    case 141: case 167: case 118: case 119: case 120: case 121: return 0;
    case 160: {
        char *u = g2h(a[0]); memset(u, 0, 65 * 6);
        strcpy(u, "Linux"); strcpy(u + 65, "ffbe"); strcpy(u + 130, "5.15.0"); strcpy(u + 195, "#1"); strcpy(u + 260, "aarch64");
        return 0;
    }
    case 163: case 261: {
        uint64_t out = nr == 163 ? a[1] : a[3];
        int res = (int)(nr == 163 ? a[0] : a[1]);
        if (out) { uint64_t *r = g2h(out); r[0] = res == 3 ? 8ULL << 20 : res == 7 ? 1024 : ~0ULL; r[1] = res == 7 ? 4096 : ~0ULL; }
        return 0;
    }
    case 165: memset(g2h(a[1]), 0, 144); return 0;
    case 169: { if (a[0]) { uint64_t w = wall_ns(); int64_t *tv = g2h(a[0]); tv[0] = (int64_t)(w / 1000000000ULL); tv[1] = (int64_t)(w % 1000000000ULL / 1000); } return 0; }
    case 172: return 1000;
    case 173: return 1;
    case 174: case 175: case 176: case 177: return 1000;
    case 178: return cur->tid;
    case 179: { uint64_t *s = g2h(a[0]); memset(s, 0, 112); s[0] = now_ns() / 1000000000ULL; s[4] = 4ULL << 30; s[5] = 2ULL << 30; ((uint32_t *)s)[26] = 1; return 0; }
    case 198: case 199: return -97;
    case 214: {
        if (!a[0] || a[0] < brk_start) return (int64_t)brk_cur;
        if (a[0] >= MMAP_LO) return (int64_t)brk_cur;
        if (a[0] > brk_cur) { uint64_t lo = (brk_cur + PAGE - 1) & ~(PAGE - 1), hi = (a[0] + PAGE - 1) & ~(PAGE - 1); if (hi > lo) { os_commit(g2h(lo), hi - lo); pages_set(lo, hi - lo, 1); } }
        else if (a[0] < brk_cur) { uint64_t lo = (a[0] + PAGE - 1) & ~(PAGE - 1), hi = (brk_cur + PAGE - 1) & ~(PAGE - 1); if (hi > lo) { os_release(g2h(lo), hi - lo); os_commit(g2h(lo), hi - lo); } }
        brk_cur = a[0];
        return (int64_t)brk_cur;
    }
    case 215: guest_free(a[0], a[1]); return 0;
    case 216: return -12;
    case 220: {
        uint64_t flags = a[0];
        GThread *t = thread_new();
        uint64_t sp_old = cpu_sp(), tls_old = cpu_tls();
        cpu_set_x(0, 0);
        if (a[1]) cpu_set_sp(a[1]);
        if (flags & 0x80000) cpu_set_tls(a[3]);
        cpu_save(t->ctx);
        cpu_set_sp(sp_old);
        cpu_set_tls(tls_old);
        if (flags & 0x100000) *(uint32_t *)g2h(a[2]) = (uint32_t)t->tid;
        if (flags & 0x1000000) *(uint32_t *)g2h(a[4]) = (uint32_t)t->tid;
        if (flags & 0x200000) t->clear_tid = a[4];
        t->last_core = -1;
        SDL_CondBroadcast(work);
        return t->tid;
    }
    case 222: {
        uint64_t len = (a[1] + PAGE - 1) & ~(PAGE - 1), addr;
        if (a[3] & 0x10) {                               // MAP_FIXED
            addr = a[0];
            if (addr < GUEST_BASE || addr + len > GUEST_BASE + ARENA_SIZE) return -12;
            os_release(g2h(addr), len); os_commit(g2h(addr), len); pages_set(addr, len, 1);
        } else {
            addr = guest_alloc(len);
            if (!addr) return -12;
        }
        if (!(a[3] & 0x20) && (int)a[4] >= 0 && a[4] < MAXFD && fds[a[4]].kind == FD_FILE) os_pread(fds[a[4]].hfd, g2h(addr), a[1], (int64_t)a[5]);
        return (int64_t)addr;
    }
    case 226: case 227: case 228: case 229: case 233: return 0;
    case 260: return -10;
    case 278: os_random(g2h(a[0]), a[1]); return (int64_t)a[1];
    case 283: case 291: case 293: case 435: return -38;
    }
    if (nr < sizeof unknown_seen && !unknown_seen[nr]) { unknown_seen[nr] = 1; rlog("[runner] system call %llu is not implemented (returning ENOSYS)\n", (unsigned long long)nr); }
    return -38;
}

static int64_t do_hypercall(uint64_t nr, uint64_t *a) {
    switch (nr) {
    case HC_PRESENT: {
        video_present();
        uint64_t now = now_ns(), gap = last_present_ns ? now - last_present_ns : 0;
        last_present_ns = now;
        n_frames++;
        if (gap > 25000000ULL) n_late++;                 // a 60-per-second frame that took more than one and a half frame times
        if (gap > worst_gap) worst_gap = gap;
        return video_paced();
    }
    case HC_POLL_EVENT: return video_poll(g2h(a[0]));
    case HC_WINDOW_SIZE: video_size(g2h(a[0]), g2h(a[1])); return 0;
    case HC_HTTP_START: { int job = http_start(g2h(a[0])); block_current_on_job(job); return 0; }
    case HC_HTTP_READ: http_read((int)a[0], g2h(a[1]), g2h(a[2])); return 0;
    case HC_HTTP_FREE: http_free((int)a[0]); return 0;
    case HC_AUDIO_OPEN: return audio_open((int)a[0], (int)a[1], (int)a[2]);
    case HC_AUDIO_WRITE: return audio_write((int)a[0], g2h(a[1]), a[2]);
    case HC_AUDIO_QUEUED: return audio_queued_ms((int)a[0]);
    case HC_AUDIO_CLOSE: audio_close((int)a[0]); return 0;
    case HC_AUDIO_CTL: audio_ctl((int)a[0], (int)a[1], (int)a[2]); return 0;
    case HC_IMAGE: { char h[1100]; if (!guest_path(a[2], h, sizeof h)) register_image(a[0], a[1], h); return 0; }
    case HC_TEXT_INPUT: return os_text_input(gstr(a[0]), gstr(a[1]), g2h(a[2]), a[3]);
    case HC_OPEN_URL: {                              // game hosts no longer exist: send those links to the local server
        const char *url = gstr(a[0]); char local[2048];
        const char *host = strstr(url, "://"), *path = host ? strchr(host + 3, '/') : NULL;
        size_t hl = host ? (path ? (size_t)(path - host - 3) : strlen(host + 3)) : 0;
        if (host && g_server[0] && ((hl >= 7 && !strncmp(host + 3 + hl - 7, "gumi.sg", 7)) || (hl >= 10 && !strncmp(host + 3 + hl - 10, "exvius.com", 10)))) {
            snprintf(local, sizeof local, "http://%s%s", g_server, path ? path : "/");
            url = local;
        }
        rlog("[runner] opening %s\n", url);
        os_open_url(url);
        return 0;
    }
    case HC_MESSAGE: SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, gstr(a[0]), gstr(a[1]), NULL); return 0;
    case HC_CLIPBOARD: SDL_SetClipboardText(gstr(a[0])); return 0;
    case HC_PAGE_OK: return guest_page_ok(a[0]);
    case HC_THREAD_URGENT: if (cur) cur->urgent = (int)a[0]; return 0;
    case HC_WEB_SHOW: {                              // game hosts no longer exist: the page comes from the local server
        web_show(gstr(a[0]), a[1] ? gstr(a[1]) : "", (int)a[2], (int)a[3], (int)a[4], (int)a[5]);
        return 0;
    }
    case HC_WEB_VISIBLE: web_set_visible((int)a[0]); return 0;
    case HC_WEB_STATE: return web_state();
    case HC_WEB_PARAM: return web_take_param(g2h(a[0]), a[1]);
    case HC_MD5: md5_bytes(g2h(a[0]), a[1], g2h(a[2])); return 0;
    case HC_DECODE_LINES: return decode_lines((int)a[0], g2h(a[1]), a[2] ? g2h(a[2]) : NULL, g2h(a[3]), a[4]);
    case HC_AES: aes_blocks((int)a[0], g2h(a[1]), a[2] ? g2h(a[2]) : NULL, g2h(a[3]), a[4]); return 0;
    }
    rlog("[runner] unknown host call 0x%llx\n", (unsigned long long)nr);
    return -38;
}

int guest_page_ok(uint64_t addr) { return addr >= GUEST_BASE && addr < GUEST_BASE + ARENA_SIZE && page_used[(addr - GUEST_BASE) / PAGE]; }

// Host calls that touch only guest memory, the window or the sound device. They run without the lock so that a slow one
// (a frame being shown, a table being decrypted, a text prompt) does not hold up the other cores.
static int unlocked_call(uint64_t nr) {
    return nr >= HC_GL_BASE || nr == HC_PRESENT || nr == HC_POLL_EVENT || nr == HC_WINDOW_SIZE || nr == HC_AES || nr == HC_MD5 || nr == HC_DECODE_LINES ||
           nr == HC_TEXT_INPUT || nr == HC_MESSAGE || nr == HC_CLIPBOARD || nr == HC_OPEN_URL ||
           (nr >= HC_AUDIO_OPEN && nr <= HC_AUDIO_CTL) || (nr >= HC_WEB_SHOW && nr <= HC_WEB_PARAM);
}
// Returns 1 when the thread can simply continue, 0 when the core should go back to the scheduler.
static int handle_svc(void) {
    uint64_t r[9];
    for (int i = 0; i < 9; i++) r[i] = cpu_x(i);
    int64_t ret;
    if (unlocked_call(r[8])) {
        n_hypercalls++;
        ret = r[8] >= HC_GL_BASE ? (int64_t)gl_call((int)(r[8] - HC_GL_BASE), r, cpu_sp()) : do_hypercall(r[8], r);
        cpu_set_x(0, (uint64_t)ret);
        return !g_quit;
    }
    SDL_LockMutex(big);
    if (r[8] >= HC_BASE) { n_hypercalls++; ret = do_hypercall(r[8], r); }
    else { n_syscalls++; ret = do_syscall(r[8], r); }
    // Decide while still holding the lock: once it is released another core may wake this thread again, and the value
    // it wakes with (set_ret) must then be the one that counts.
    int runnable = cur->state == T_RUN, keep = runnable && !want_switch && !g_quit;
    SDL_UnlockMutex(big);
    if (runnable) cpu_set_x(0, (uint64_t)ret);
    return keep;
}

// ---------------------------------------------------------------------------------------------------------
// Loading the guest program and running
// ---------------------------------------------------------------------------------------------------------
static uint64_t push_str(uint64_t *sp, const char *s) { size_t n = strlen(s) + 1; *sp -= n; memcpy(g2h(*sp), s, n); return *sp; }

static void load_guest(const char *path, int argc, char **argv, char **envp, int envc) {
    FILE *f = os_fopen(path, "rb");
    if (!f) fatal("cannot open the guest program %s", path);
    fseek(f, 0, SEEK_END); long size = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *img = malloc((size_t)size);
    if (fread(img, 1, (size_t)size, f) != (size_t)size) fatal("cannot read %s", path);
    fclose(f);
    uint64_t entry, phoff; uint16_t phnum, phentsize;
    memcpy(&entry, img + 0x18, 8); memcpy(&phoff, img + 0x20, 8); memcpy(&phentsize, img + 0x36, 2); memcpy(&phnum, img + 0x38, 2);
    uint64_t top = 0, first = ~0ULL;
    for (int i = 0; i < phnum; i++) {
        uint8_t *ph = img + phoff + (size_t)i * phentsize;
        uint32_t type; uint64_t off, vaddr, filesz, memsz;
        memcpy(&type, ph, 4); memcpy(&off, ph + 8, 8); memcpy(&vaddr, ph + 16, 8); memcpy(&filesz, ph + 32, 8); memcpy(&memsz, ph + 40, 8);
        if (type != 1) continue;
        uint64_t lo = vaddr & ~(PAGE - 1), hi = (vaddr + memsz + PAGE - 1) & ~(PAGE - 1);
        os_commit(g2h(lo), hi - lo);
        pages_set(lo, hi - lo, 1);
        memcpy(g2h(vaddr), img + off, filesz);
        if (hi > top) top = hi;
        if (vaddr - off < first) first = vaddr - off;
    }
    brk_start = brk_cur = top;
    load_syms(&symtabs[0], path, 0, 2 /* SHT_SYMTAB */, "loader");
    symtabs[0].lo = first; symtabs[0].hi = top;
    free(img);

    uint64_t stack = guest_alloc(8 << 20), sp = stack + (8 << 20);
    uint64_t gargv[32], genv[64];
    uint8_t rnd[16]; os_random(rnd, 16);
    sp -= 16; memcpy(g2h(sp), rnd, 16); uint64_t grnd = sp;
    uint64_t gplat = push_str(&sp, "aarch64");
    for (int i = 0; i < argc; i++) gargv[i] = push_str(&sp, argv[i]);
    for (int i = 0; i < envc; i++) genv[i] = push_str(&sp, envp[i]);
    sp &= ~15ULL;
    uint64_t aux[] = {3, first + phoff, 4, phentsize, 5, phnum, 6, 4096, 9, entry, 16, 0x3, 17, 100, 23, 0,
                      25, grnd, 15, gplat, 31, gargv[0], 11, 1000, 12, 1000, 13, 1000, 14, 1000, 0, 0};
    size_t words = 1 + argc + 1 + envc + 1 + sizeof aux / 8;
    if (words & 1) sp -= 8;
    sp -= words * 8;
    uint64_t *w = g2h(sp);
    *w++ = (uint64_t)argc;
    for (int i = 0; i < argc; i++) *w++ = gargv[i];
    *w++ = 0;
    for (int i = 0; i < envc; i++) *w++ = genv[i];
    *w++ = 0;
    memcpy(w, aux, sizeof aux);

    GThread *t = thread_new();
    main_thread = t;
    cpu_set_sp(sp);
    cpu_set_pc(entry);
    cpu_set_x(0, 0);
    cpu_save(t->ctx);
}

static void dump_threads(void) {
    static const char *names[] = {"free", "running", "waiting on a lock", "sleeping", "waiting for the host", "dead"};
    rlog("[runner] no frame for %.1f s; guest threads:\n", (now_ns() - last_present_ns) / 1e9);
    for (int i = 0; i < MAXT; i++) {
        GThread *t = &threads[i];
        if (t->state == T_FREE || !t->ctx) continue;
        uint64_t pc = cpu_ctx_reg(t->ctx, 32), fp = cpu_ctx_reg(t->ctx, 29); char s[400];
        symbolize(pc, s, sizeof s);
        rlog("  thread %d (%s%s): %s\n", t->tid, names[t->state], t->state == T_FUTEX && t->has_timeout ? ", with timeout" : "", s);
        for (int k = 0; k < 7 && fp >= GUEST_BASE && fp + 16 < GUEST_BASE + ARENA_SIZE; k++) {
            uint64_t *f = (uint64_t *)(arena + (fp - GUEST_BASE));
            symbolize(f[1], s, sizeof s); rlog("      at %s\n", s);
            if (f[0] <= fp) break;
            fp = f[0];
        }
    }
}
// Which thread should this core run next? Core 0 runs only the game's main thread (when there is more than one core),
// so drawing never waits behind loading or sound. The other cores share the rest: sound threads first, then threads
// that last ran here.
static GThread *pick(int core) {
    if (n_cores > 1 && core == 0) return main_thread && main_thread->state == T_RUN && !main_thread->running ? main_thread : NULL;
    static __thread int rr, urgent_run;
    GThread *best = NULL; int best_score = -1, best_k = 0;
    for (int i = 0; i < MAXT; i++) {
        int k = (rr + i) % MAXT; GThread *t = &threads[k];
        if (t->state != T_RUN || t->running || (n_cores > 1 && t == main_thread)) continue;
        // After four sound turns in a row an ordinary thread gets one, so a busy sound thread cannot stop the rest.
        int score = (t->urgent && urgent_run < 4 ? 2 : 0) + (t->last_core == core ? 1 : 0);
        if (score > best_score) { best = t; best_score = score; best_k = k; }
    }
    if (best) { rr = best_k + 1; urgent_run = best->urgent ? urgent_run + 1 : 0; }
    return best;
}
// Diagnosis (FFBE_PROFILE=1): while a frame is overdue, the game's main thread is stopped once a millisecond and the
// place it was at is counted; each 5-second report then names the busiest places. This is how to find out what a
// loading stall is spent on.
static int g_profile;
static int g_keepalive = 1;                  // FFBE_NO_KEEPALIVE=1 turns the reading of events during stalls off (for comparison)
static volatile int keepalive_due;
#define MAXPROF 4096
static struct { uint64_t pc, lr; unsigned n; } prof[MAXPROF];
static unsigned prof_total;
static void prof_add(uint64_t pc, uint64_t lr) {
    unsigned h = (unsigned)((pc * 0x9E3779B97F4A7C15ULL) >> 52) % MAXPROF;
    for (int k = 0; k < 64; k++) {
        unsigned i = (h + k) % MAXPROF;
        if (!prof[i].n || prof[i].pc == pc) { prof[i].pc = pc; prof[i].lr = lr; prof[i].n++; prof_total++; return; }
    }
}
static void prof_report(void) {
    if (prof_total < 50) { memset(prof, 0, sizeof prof); prof_total = 0; return; }
    // Group by function (the symbol under the address), then list the top ten.
    static struct { char name[160]; unsigned n; } fn[512]; int nfn = 0;
    for (int i = 0; i < MAXPROF; i++) if (prof[i].n) {
        char s[400], callers[400]; symbolize(prof[i].pc, s, sizeof s);
        char *plus = strrchr(s, '+'); if (plus) *plus = 0;
        char *name = strstr(s, "  "); name = name ? name + 2 : s;
        if (!strncmp(name, "0x", 2)) { symbolize(prof[i].lr, callers, sizeof callers); char *p2 = strrchr(callers, '+'); if (p2) *p2 = 0; char *c = strstr(callers, "  "); snprintf(s, sizeof s, "(called from %s)", c ? c + 2 : callers); name = s; }
        int k = 0; while (k < nfn && strcmp(fn[k].name, name)) k++;
        if (k == nfn) { if (nfn == 512) continue; snprintf(fn[nfn].name, sizeof fn[nfn].name, "%.150s", name); fn[nfn++].n = 0; }
        fn[k].n += prof[i].n;
    }
    rlog("[profile] %u samples of the main thread while frames were overdue:\n", prof_total);
    for (int top = 0; top < 10; top++) {
        int best = -1;
        for (int k = 0; k < nfn; k++) if (fn[k].n && (best < 0 || fn[k].n > fn[best].n)) best = k;
        if (best < 0) break;
        rlog("    %5.1f%%  %s\n", fn[best].n * 100.0 / prof_total, fn[best].name);
        fn[best].n = 0;
    }
    memset(prof, 0, sizeof prof); prof_total = 0;
}
static int any_alive(void) { for (int i = 0; i < MAXT; i++) if (threads[i].state != T_FREE && threads[i].state != T_DEAD) return 1; return 0; }

static void core_loop(int core) {
    cpu_bind(core);
    my_core = core;
    SDL_LockMutex(big);
    while (!g_quit) {
        wake_due();
        GThread *t = pick(core);
        if (!t) {
            if (!any_alive()) break;
            // Nothing to run here. The main core waits out the last moments before its thread's wake-up time itself,
            // so frames start on time; otherwise wait to be told (the ticker also checks every millisecond).
            if (n_cores > 1 && core == 0 && main_thread && main_thread->state == T_SLEEP) {
                uint64_t now = now_ns(), due = main_thread->wake_ns;
                if (due > now + 2000000ULL) SDL_CondWaitTimeout(work, big, (Uint32)((due - now) / 1000000ULL - 1));
                else { SDL_UnlockMutex(big); while (now_ns() < due && !g_quit) SDL_Delay(0); SDL_LockMutex(big); }
            } else SDL_CondWaitTimeout(work, big, 2);
            continue;
        }
        t->running = 1; t->last_core = core;
        cur = t; core_thread[core] = t; slice_start[core] = now_ns();
        cpu_restore(t->ctx); n_switches++;
        if (t->set_ret) { cpu_set_x(0, (uint64_t)t->ret); t->set_ret = 0; }
        want_switch = 0;
        SDL_UnlockMutex(big);
        // Run this thread until it blocks, exits, or its turn ends.
        for (;;) {
            int why = cpu_run();
            // Core 0 is the thread that owns the window. If the game has not asked for the window's events for a
            // while (it is busy loading), read them here so that Windows does not take the window for dead.
            if (core == 0 && keepalive_due) { keepalive_due = 0; video_keepalive(); if (why == CPU_SLICE && !g_quit && !want_switch) continue; }
            if (why == CPU_SVC) { if (handle_svc()) continue; break; }
            if (g_profile && why == CPU_SLICE && t == main_thread) { prof_add(cpu_pc(), cpu_x(30)); if (!g_quit) continue; }
            if (why == CPU_FAULT) { SDL_LockMutex(big); report_crash(cpu_fault_text()); g_quit = 1; g_exit_code = 70; SDL_UnlockMutex(big); }
            break;
        }
        SDL_LockMutex(big);
        if (t->state == T_DEAD) { cpu_ctx_free(t->ctx); t->ctx = NULL; t->state = T_FREE; }
        else cpu_save(t->ctx);
        t->running = 0;
        cur = NULL; core_thread[core] = NULL;
        if (t->state == T_RUN) SDL_CondBroadcast(work);          // it may suit another core better
    }
    SDL_UnlockMutex(big);
}
static int worker_core(void *ud) { core_loop((int)(intptr_t)ud); return 0; }

// Once a millisecond: wake threads whose sleep is over, end turns that have run long enough while others wait, and
// make room at once when a sound thread is waiting.
static int ticker(void *ud) {
    (void)ud;
    uint64_t last_report = now_ns();
    while (!g_quit) {
        SDL_Delay(1);
        SDL_LockMutex(big);
        wake_due();
        uint64_t now = now_ns();
        int waiting = 0, waiting_urgent = 0, idle = 0, first = n_cores > 1 ? 1 : 0;
        for (int i = 0; i < MAXT; i++) {
            GThread *t = &threads[i];
            if (t->state == T_RUN && !t->running && !(n_cores > 1 && t == main_thread)) { waiting++; if (t->urgent) waiting_urgent++; }
        }
        for (int c = first; c < n_cores; c++) if (!core_thread[c]) idle++;
        if (waiting) SDL_CondBroadcast(work);
        if (waiting > idle) for (int c = first; c < n_cores; c++) {
            GThread *t = core_thread[c];
            if (!t) continue;
            if (now - slice_start[c] >= 3000000ULL || (waiting_urgent > idle && !t->urgent)) cpu_request_stop(c);
        }
        if (g_profile && n_cores > 1 && last_present_ns && now - last_present_ns > 60000000ULL) cpu_request_stop(0);
        if (g_keepalive && video_quiet_ns() > 150000000ULL) { keepalive_due = 1; cpu_request_stop(0); }
        if (g_verbose && last_present_ns && now - last_present_ns > 4000000000ULL && now - last_report > 5000000000ULL) dump_threads();
        if (g_verbose && now - last_report > 5000000000ULL) {
            last_report = now;
            rlog("[runner] 5 s: %lu frames, %lu late, longest %.0f ms; %d sound gaps, least in hand %d ms; %lu system calls, %lu host calls, %lu thread switches; window unread for at most %d ms\n",
                 n_frames, n_late, worst_gap / 1e6, audio_gaps(), audio_low_ms(), n_syscalls, n_hypercalls, n_switches, video_longest_quiet_ms());
            n_syscalls = n_hypercalls = n_switches = n_frames = n_late = 0; worst_gap = 0;
            if (g_profile) prof_report();
        }
        SDL_UnlockMutex(big);
    }
    for (int c = 0; c < n_cores; c++) cpu_request_stop(c);
    SDL_CondBroadcast(work);
    return 0;
}

int main(int argc, char **argv) {
    // runner <root folder> <guest program> [guest args...]
    os_args(&argc, &argv);
    if (argc < 3) { fprintf(stderr, "usage: runner <game folder> <guest program> [args]\n"); return 2; }
    snprintf(g_root, sizeof g_root, "%s", argv[1]);
    for (char *p = g_root; *p; p++) if (*p == '\\') *p = '/';
    size_t rl = strlen(g_root);
    if (rl && g_root[rl - 1] == '/') g_root[rl - 1] = 0;
    char logpath[1100];
    snprintf(logpath, sizeof logpath, "%s/runner.log", g_root);
    g_logf = os_fopen(logpath, "w");
    if (getenv("FFBE_VERBOSE")) g_verbose = atoi(getenv("FFBE_VERBOSE"));
    if (getenv("FFBE_PROFILE")) { g_profile = 1; if (!g_verbose) g_verbose = 1; }
    if (getenv("FFBE_NO_KEEPALIVE")) g_keepalive = 0;
    os_files_init();
    os_tune_process();
    if (getenv("FFBE_TEST_CRASH")) { volatile int *nowhere = NULL; *nowhere = 1; }      // for tests: the runner itself crashes
    overlay_init();
    audio_load_settings();
    if (getenv("FFBE_SERVER")) snprintf(g_server, sizeof g_server, "%s", getenv("FFBE_SERVER"));

    if (SDL_Init(SDL_INIT_TIMER) != 0) fatal("SDL: %s", SDL_GetError());
    overlay_extras();
    uint8_t *reserved = os_reserve(ARENA_SIZE + GUEST_BASE);
    if (!reserved) fatal("cannot reserve %llu MB of address space", (unsigned long long)(ARENA_SIZE >> 20));
    os_guard(reserved, GUEST_BASE);                 // guest addresses below 64 KB (null pointers) fault
    arena = reserved + GUEST_BASE;
    page_used = calloc(NPAGES, 1);
    (void)scratch_zero;
    // Cores: one for the game's main thread and, when the PC has them to spare, two for everything else.
    int host_cpus = SDL_GetCPUCount(), want = host_cpus >= 4 ? 3 : host_cpus >= 2 ? 2 : 1;
    if (getenv("FFBE_CORES")) want = atoi(getenv("FFBE_CORES"));
    if (want < 1) want = 1;
    if (want > MAXCORES) want = MAXCORES;
    n_cores = cpu_init(arena, GUEST_BASE, ARENA_SIZE, getenv("FFBE_CODE_CACHE_MB") ? atoi(getenv("FFBE_CODE_CACHE_MB")) : 512, want);
    big = SDL_CreateMutex();
    work = SDL_CreateCond();
    rlog("[runner] %d core%s (this PC has %d)\n", n_cores, n_cores == 1 ? "" : "s", host_cpus);

    // Guest environment: local time zone as a fixed offset, and a marker that it runs under the runner.
    char tz[48], *envp[20]; int envc = 0;
    int off = os_utc_offset_seconds();
    snprintf(tz, sizeof tz, "TZ=<%+03d%02d>%+d:%02d", off / 3600, abs(off) % 3600 / 60, -off / 3600, abs(off) % 3600 / 60);
    envp[envc++] = tz;
    envp[envc++] = "FFBE_RUNNER=1";
    static char passthru[11][2048];
    const char *names[] = {"TAPS", "NOTIF_CB", "VERBOSE", "TRACE_METHOD", "WIDTH", "HEIGHT", "FFBE_FONT", "FFBE_FRAMES", "TRACE_FUNCS",
                           "FFBE_NO_SETUP_RESTART", "FFBE_QUIET_RESTART"};
    for (int i = 0; i < 11; i++) if (getenv(names[i])) { snprintf(passthru[i], 2048, "%s=%s", names[i], getenv(names[i])); envp[envc++] = passthru[i]; }

    load_guest(argv[2], argc - 2, argv + 2, envp, envc);
    SDL_Thread *tk = SDL_CreateThread(ticker, "ticker", NULL), *workers[MAXCORES] = {0};
    for (int c = 1; c < n_cores; c++) workers[c] = SDL_CreateThread(worker_core, "core", (void *)(intptr_t)c);
    core_loop(0);
    g_quit = 1;
    SDL_WaitThread(tk, NULL);
    for (int c = 1; c < n_cores; c++) SDL_WaitThread(workers[c], NULL);
    video_shutdown();
    rlog("[runner] finished with code %d\n", g_exit_code);
    if (g_logf) fclose(g_logf);
    return g_exit_code;
}
