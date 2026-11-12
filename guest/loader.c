// Feasibility probe: load the Android game library (libgame.so, ARM64) on plain ARM64 Linux, without Android.
// It maps the library, links its imports to glibc / small shims / logging stubs, runs its C++ static
// constructors, calls JNI_OnLoad with a fake Java VM, then drives the cocos2d-x start-up calls.
// Everything the library asks of "Android" (Java calls, GL, audio, assets) is logged so the size of the
// job can be measured. Not a port: GL and audio are fakes.
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include "shared.h"
#include "hyper.h"
#include "gl_names.h"

uint8_t *g_base;
size_t g_image_size;
static Elf64_Sym *g_symtab;
static const char *g_strtab;
static size_t g_nsyms;
static Elf64_Ehdr g_ehdr;
static Elf64_Phdr *g_phdr;
FILE *g_log;
int g_verbose = 1;

void logf_(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log ? g_log : stderr, fmt, ap);
    va_end(ap);
    fflush(g_log ? g_log : stderr);
}

// ---------------------------------------------------------------------------------------------------
// Symbols
// ---------------------------------------------------------------------------------------------------
size_t lib_sym_size(const char *name) {
    for (size_t i = 0; i < g_nsyms; i++) if (g_symtab[i].st_shndx != SHN_UNDEF && !strcmp(g_strtab + g_symtab[i].st_name, name)) return g_symtab[i].st_size;
    return 0;
}
void *lib_sym(const char *name) {
    for (size_t i = 0; i < g_nsyms; i++) {
        if (g_symtab[i].st_shndx != SHN_UNDEF && strcmp(g_strtab + g_symtab[i].st_name, name) == 0)
            return g_base + g_symtab[i].st_value;
    }
    return NULL;
}

const char *lib_addr_name(uintptr_t addr, uintptr_t *off) {
    if (addr < (uintptr_t)g_base || addr >= (uintptr_t)g_base + g_image_size) return NULL;
    uintptr_t rel = addr - (uintptr_t)g_base, best = 0;
    const char *name = NULL;
    for (size_t i = 0; i < g_nsyms; i++) {
        Elf64_Sym *s = &g_symtab[i];
        if (s->st_shndx == SHN_UNDEF || ELF64_ST_TYPE(s->st_info) != STT_FUNC) continue;
        if (s->st_value <= rel && s->st_value >= best) { best = s->st_value; name = g_strtab + s->st_name; }
    }
    if (off) *off = rel - best;
    return name;
}

// ---------------------------------------------------------------------------------------------------
// Logging stubs for imports we do not implement. Each import gets its own tiny trampoline so a call
// can be attributed to a name.
// ---------------------------------------------------------------------------------------------------
#define MAX_STUBS 1024
static const char *stub_names[MAX_STUBS];
static unsigned long stub_calls[MAX_STUBS];
static int n_stubs;
static uint32_t *stub_page;

uint64_t unresolved_c(uint64_t idx) {
    if (idx < MAX_STUBS) {
        if (stub_calls[idx]++ == 0 && g_verbose) logf_("[stub] first call: %s\n", stub_names[idx]);
    }
    return 0;
}
__asm__(".text\n.global stub_common\nstub_common:\n  mov x0, x17\n  b unresolved_c\n");
extern void stub_common(void);

void *make_stub(const char *name) {
    if (!stub_page) {
        stub_page = mmap(NULL, MAX_STUBS * 32, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    }
    for (int i = 0; i < n_stubs; i++) if (strcmp(stub_names[i], name) == 0) return stub_page + i * 8;
    int i = n_stubs++;
    stub_names[i] = name;
    uint32_t *p = stub_page + i * 8;
    p[0] = 0x58000090;  // ldr x16, [pc, #16]
    p[1] = 0x580000B1;  // ldr x17, [pc, #20]
    p[2] = 0xD61F0200;  // br x16
    p[3] = 0xD503201F;  // nop
    *(uint64_t *)(p + 4) = (uint64_t)stub_common;
    *(uint64_t *)(p + 6) = (uint64_t)i;
    __builtin___clear_cache((char *)p, (char *)(p + 8));
    return p;
}

// ---------------------------------------------------------------------------------------------------
// Debug aid: TRACE_FUNCS=symbol,symbol,... logs every call of those library functions with their first
// four arguments and the caller. Each traced function's first four instructions are moved to a small
// trampoline, so only functions whose prologue is position independent can be traced.
// ---------------------------------------------------------------------------------------------------
static const char *trace_names[64];
static unsigned long trace_hits[64];
static char trace_wants_ret[64];
static __thread struct { uint64_t lr; int idx; } trace_stack[64];
static __thread int trace_depth;
static int readable(uint64_t a, size_t n) { return g_runner && a >= 0x10000 && hc(HC_PAGE_OK, (long)a, 0, 0, 0) && hc(HC_PAGE_OK, (long)(a + n - 1), 0, 0, 0); }
static int printable(const unsigned char *p, int n) { for (int i = 0; i < n; i++) if (p[i] < 0x20 || p[i] > 0x7E) return 0; return n > 0; }
// Describe a value: a number, or the text it points at (a C string, or a libc++ std::string object).
static void describe_value(uint64_t v, char *out, size_t cap) {
    snprintf(out, cap, "%#lx", v);
    if (!readable(v, 32)) return;
    const unsigned char *p = (const unsigned char *)v;
    if (!(p[0] & 1) && p[0] >= 2 && (p[0] >> 1) <= 22 && printable(p + 1, p[0] >> 1) && p[1 + (p[0] >> 1)] == 0) { snprintf(out, cap, "str\"%.*s\"", p[0] >> 1, p + 1); return; }
    if (p[0] & 1) {
        uint64_t len = *(const uint64_t *)(p + 8), ptr = *(const uint64_t *)(p + 16);
        if (len > 0 && len < 100000 && readable(ptr, len < 80 ? len : 80) && printable((const unsigned char *)ptr, len < 80 ? (int)len : 80)) { snprintf(out, cap, "str\"%.*s\"%s", len < 80 ? (int)len : 80, (const char *)ptr, len > 80 ? "..." : ""); return; }
    }
    int n = 0; while (n < 80 && p[n] >= 0x20 && p[n] <= 0x7E) n++;
    if (n >= 2 && (p[n] == 0)) snprintf(out, cap, "\"%.*s\"", n, p);
}
uint64_t trace_c(uint64_t idx, uint64_t *regs, uint64_t lr) {     // returns 1 when the return should be reported too
    if (trace_hits[idx]++ >= 400) return 0;
    uintptr_t off = 0;
    const char *caller = lib_addr_name((uintptr_t)lr, &off);
    char a[6][120];
    for (int i = 0; i < 6; i++) describe_value(regs[i], a[i], sizeof a[i]);
    logf_("[trace] %s(%s, %s, %s, %s, %s, %s) from %s+%#lx\n", trace_names[idx], a[0], a[1], a[2], a[3], a[4], a[5], caller ? caller : "?", (unsigned long)off);
    if (!trace_wants_ret[idx] || trace_depth >= 64) return 0;
    trace_stack[trace_depth].lr = lr; trace_stack[trace_depth].idx = (int)idx; trace_depth++;
    return 1;
}
uint64_t trace_ret_c(uint64_t value) {                            // returns the address to go back to
    trace_depth--;
    char a[120]; describe_value(value, a, sizeof a);
    logf_("[trace]   %s -> %s\n", trace_names[trace_stack[trace_depth].idx], a);
    return trace_stack[trace_depth].lr;
}
__asm__(".text\n.global trace_common\ntrace_common:\n"          // on entry [sp] = index, [sp+8] = where to continue
        "  stp x29, x30, [sp, #-96]!\n  stp x0, x1, [sp, #16]\n  stp x2, x3, [sp, #32]\n  stp x4, x5, [sp, #48]\n  stp x6, x7, [sp, #64]\n  str x8, [sp, #80]\n"
        "  sub sp, sp, #128\n  stp q0, q1, [sp]\n  stp q2, q3, [sp, #32]\n  stp q4, q5, [sp, #64]\n  stp q6, q7, [sp, #96]\n"
        "  ldr x0, [sp, #224]\n  add x1, sp, #144\n  mov x2, x30\n  bl trace_c\n  mov x9, x0\n"
        "  ldp q0, q1, [sp]\n  ldp q2, q3, [sp, #32]\n  ldp q4, q5, [sp, #64]\n  ldp q6, q7, [sp, #96]\n  add sp, sp, #128\n"
        "  ldp x0, x1, [sp, #16]\n  ldp x2, x3, [sp, #32]\n  ldp x4, x5, [sp, #48]\n  ldp x6, x7, [sp, #64]\n  ldr x8, [sp, #80]\n"
        "  ldp x29, x30, [sp], #96\n  ldp x16, x17, [sp], #16\n"
        "  cbz x9, 1f\n  adr x30, trace_ret\n1:  br x17\n"
        "trace_ret:\n  stp x0, x1, [sp, #-48]!\n  stp q0, q1, [sp, #16]\n  bl trace_ret_c\n  mov x30, x0\n  ldp q0, q1, [sp, #16]\n  ldp x0, x1, [sp], #48\n  ret\n");
extern void trace_common(void);
static void install_traces(void) {
    const char *list = getenv("TRACE_FUNCS");
    if (!list || !*list) return;
    uint32_t *page = mmap(NULL, 64 * 128, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    char *copy = strdup(list);
    int n = 0;
    for (char *name = strtok(copy, ","); name && n < 64; name = strtok(NULL, ",")) {
        size_t nl = strlen(name);
        if (nl > 2 && !strcmp(name + nl - 2, "+r")) { name[nl - 2] = 0; trace_wants_ret[n] = 1; }      // name+r: also log the return value
        uint32_t *fn = name[0] == '@' ? (uint32_t *)(g_base + strtoul(name + 1, NULL, 16)) : lib_sym(name);     // @hex = offset in the library
        if (!fn || (name[0] != '@' && lib_sym_size(name) < 16)) { logf_("[trace] no such function, or too short to trace: %s\n", name); trace_wants_ret[n] = 0; continue; }
        int ok = 1;
        for (int i = 0; i < 4; i++) {
            uint32_t op = fn[i];
            if ((op & 0x1F000000) == 0x10000000 || (op & 0x7C000000) == 0x14000000 || (op & 0x7E000000) == 0x34000000 ||
                (op & 0x7E000000) == 0x36000000 || (op & 0xFF000010) == 0x54000000 || (op & 0x3B000000) == 0x18000000) ok = 0;
        }
        if (!ok) { logf_("[trace] cannot trace %s (its first instructions depend on their address)\n", name); trace_wants_ret[n] = 0; continue; }
        uint32_t *t = page + n * 32;                                   // one 128-byte slot per function
        memcpy(t, fn, 16);                                             // the moved prologue ...
        t[4] = 0x58000050; t[5] = 0xD61F0200;                          // ... then ldr x16, [pc, #8]; br x16
        *(uint64_t *)(t + 6) = (uint64_t)(fn + 4);
        t[8] = 0x580000D0;                                             // ldr x16, [pc, #24]   index
        t[9] = 0x580000F1;                                             // ldr x17, [pc, #28]   continue at
        t[10] = 0xA9BF47F0;                                            // stp x16, x17, [sp, #-16]!
        t[11] = 0x580000F0;                                            // ldr x16, [pc, #28]   trace_common
        t[12] = 0xD61F0200;                                            // br x16
        t[13] = 0xD503201F;
        *(uint64_t *)(t + 14) = (uint64_t)n;
        *(uint64_t *)(t + 16) = (uint64_t)t;
        *(uint64_t *)(t + 18) = (uint64_t)trace_common;
        fn[0] = 0x58000050; fn[1] = 0xD61F0200;                        // ldr x16, [pc, #8]; br x16
        *(uint64_t *)(fn + 2) = (uint64_t)(t + 8);
        __builtin___clear_cache((char *)fn, (char *)(fn + 4));
        trace_names[n] = name;
        logf_("[trace] watching %s\n", name);
        n++;
    }
    __builtin___clear_cache((char *)page, (char *)page + 64 * 128);
}
// The game's movies are played by Android's own decoder, which does not exist here: a movie scene opens and closes
// again at once. A "notice movie" (the trailer the Home screen and a summon banner show unasked) stops the music
// before it starts and leaves starting it again to what follows the movie, so on this program the music died every
// time one came up: on returning Home from anywhere, on opening a banner, after a summon. NoticeMovieScene::initialize
// is LibraryMovieScene::initialize followed by a tail call "stopBgm(true)"; that tail call becomes a plain return.
// Nothing is changed if the function does not look exactly like that (another version of the game).
static void keep_music_through_notice_movies(void) {
    uint32_t *fn = lib_sym("_ZN16NoticeMovieScene10initializeEv");
    if (!fn || lib_sym_size("_ZN16NoticeMovieScene10initializeEv") != 28) return;
    // stp x29,x30,[sp,#-16]! ; mov x29,sp ; bl ; bl ; mov w1,#1 ; ldp x29,x30,[sp],#16 ; b
    if (fn[0] != 0xA9BF7BFD || fn[1] != 0x910003FD || (fn[2] >> 26) != 0x25 || (fn[3] >> 26) != 0x25 ||
        fn[4] != 0x52800021 || fn[5] != 0xA8C17BFD || (fn[6] >> 26) != 0x05) return;
    fn[6] = 0xD65F03C0;                                               // ret
    __builtin___clear_cache((char *)fn, (char *)(fn + 7));
    logf_("[loader] notice movies no longer stop the music\n");
}

void stub_report(void) {
    logf_("\n== unimplemented imports that were called ==\n");
    for (int i = 0; i < n_stubs; i++) if (stub_calls[i]) logf_("  %8lu  %s\n", stub_calls[i], stub_names[i]);
    int never = 0;
    for (int i = 0; i < n_stubs; i++) if (!stub_calls[i]) never++;
    logf_("  (%d other unimplemented imports were never called)\n", never);
}

// ---------------------------------------------------------------------------------------------------
// Loading and relocation
// ---------------------------------------------------------------------------------------------------
extern void *shim_lookup(const char *name);

static int n_glibc, n_shim, n_stub, n_internal;

int g_runner;
extern void *libc_lookup(const char *name);
// In the runner, a GL function is three instructions: put its call number in x8, trap to the host, return.
static uint32_t *hc_page; static int n_hc;
static void *make_hc_stub(int nr) {
    if (!hc_page) hc_page = mmap(NULL, 16384, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uint32_t *p = hc_page + n_hc++ * 4;
    p[0] = 0xD2800008u | ((uint32_t)nr << 5);   // movz x8, #nr
    p[1] = 0xD4000001u;                         // svc #0
    p[2] = 0xD65F03C0u;                         // ret
    p[3] = 0xD503201Fu;
    __builtin___clear_cache((char *)p, (char *)(p + 4));
    return p;
}
static void *resolve_import(const char *name) {
    if (g_runner) for (int i = 0; i < GL_COUNT; i++) if (!strcmp(GL_NAMES[i], name)) { n_shim++; return make_hc_stub(HC_GL_BASE + i); }
    void *p = shim_lookup(name);
    if (p) { n_shim++; return p; }
    p = libc_lookup(name);
    if (p) { n_glibc++; return p; }
    n_stub++;
    return make_stub(name);
}

static uintptr_t sym_value(Elf64_Sym *s) {
    if (s->st_shndx != SHN_UNDEF) { n_internal++; return (uintptr_t)g_base + s->st_value; }
    const char *name = g_strtab + s->st_name;
    if (!*name) return 0;
    return (uintptr_t)resolve_import(name);
}

static void apply_rela(Elf64_Rela *r, size_t n) {
    for (size_t i = 0; i < n; i++, r++) {
        uint64_t *where = (uint64_t *)(g_base + r->r_offset);
        uint32_t type = ELF64_R_TYPE(r->r_info), sidx = ELF64_R_SYM(r->r_info);
        switch (type) {
        case R_AARCH64_RELATIVE: *where = (uint64_t)g_base + r->r_addend; break;
        case R_AARCH64_GLOB_DAT:
        case R_AARCH64_JUMP_SLOT:
        case R_AARCH64_ABS64: *where = sym_value(&g_symtab[sidx]) + r->r_addend; break;
        default: logf_("unhandled relocation type %u\n", type); exit(3);
        }
    }
}

static void load_library(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror(path); exit(2); }
    pread(fd, &g_ehdr, sizeof g_ehdr, 0);
    Elf64_Phdr *ph = malloc(g_ehdr.e_phnum * sizeof *ph);
    pread(fd, ph, g_ehdr.e_phnum * sizeof *ph, g_ehdr.e_phoff);
    uint64_t top = 0;
    for (int i = 0; i < g_ehdr.e_phnum; i++)
        if (ph[i].p_type == PT_LOAD && ph[i].p_vaddr + ph[i].p_memsz > top) top = ph[i].p_vaddr + ph[i].p_memsz;
    g_image_size = (top + 0xFFFF) & ~0xFFFFUL;
    g_base = mmap(NULL, g_image_size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g_base == MAP_FAILED) { perror("mmap"); exit(2); }
    Elf64_Dyn *dyn = NULL;
    for (int i = 0; i < g_ehdr.e_phnum; i++) {
        if (ph[i].p_type == PT_LOAD) pread(fd, g_base + ph[i].p_vaddr, ph[i].p_filesz, ph[i].p_offset);
        if (ph[i].p_type == PT_DYNAMIC) dyn = (Elf64_Dyn *)(g_base + ph[i].p_vaddr);
    }
    g_phdr = (Elf64_Phdr *)(g_base + g_ehdr.e_phoff);
    // Section headers, only for the size of .dynsym.
    Elf64_Shdr *sh = malloc(g_ehdr.e_shnum * sizeof *sh);
    pread(fd, sh, g_ehdr.e_shnum * sizeof *sh, g_ehdr.e_shoff);
    for (int i = 0; i < g_ehdr.e_shnum; i++) if (sh[i].sh_type == SHT_DYNSYM) g_nsyms = sh[i].sh_size / sizeof(Elf64_Sym);
    close(fd);

    Elf64_Rela *rela = NULL, *jmprel = NULL;
    size_t relasz = 0, pltrelsz = 0;
    void (**init_array)(void) = NULL;
    size_t init_arraysz = 0;
    for (Elf64_Dyn *d = dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
        case DT_SYMTAB: g_symtab = (Elf64_Sym *)(g_base + d->d_un.d_ptr); break;
        case DT_STRTAB: g_strtab = (const char *)(g_base + d->d_un.d_ptr); break;
        case DT_RELA: rela = (Elf64_Rela *)(g_base + d->d_un.d_ptr); break;
        case DT_RELASZ: relasz = d->d_un.d_val; break;
        case DT_JMPREL: jmprel = (Elf64_Rela *)(g_base + d->d_un.d_ptr); break;
        case DT_PLTRELSZ: pltrelsz = d->d_un.d_val; break;
        case DT_INIT_ARRAY: init_array = (void (**)(void))(g_base + d->d_un.d_ptr); break;
        case DT_INIT_ARRAYSZ: init_arraysz = d->d_un.d_val; break;
        }
    }
    logf_("[load] image %zu MB at %p, %zu dynamic symbols\n", g_image_size >> 20, g_base, g_nsyms);
    if (g_runner) hc(HC_IMAGE, (long)g_base, (long)g_image_size, (long)path, 0);
    apply_rela(rela, relasz / sizeof *rela);
    apply_rela(jmprel, pltrelsz / sizeof *jmprel);
    logf_("[load] relocations: %zu + %zu applied\n", relasz / sizeof *rela, pltrelsz / sizeof *jmprel);
    logf_("[load] import bindings: %d to glibc as-is, %d to shims, %d to logging stubs (distinct unimplemented: %d)\n",
          n_glibc, n_shim, n_stub, n_stubs);
    __builtin___clear_cache((char *)g_base, (char *)g_base + g_image_size);

    size_t n = init_arraysz / sizeof(void *);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    size_t ran = 0;
    for (size_t i = 0; i < n; i++) {
        if (init_array[i] && (intptr_t)init_array[i] != -1) { init_array[i](); ran++; }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    logf_("[load] ran %zu static constructors in %.2f s\n", ran, (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9);
}

// The library's own unwinder (C++ exceptions) finds its tables through dl_iterate_phdr.
int my_dl_iterate_phdr(int (*cb)(struct dl_phdr_info *, size_t, void *), void *data) {
    struct dl_phdr_info info;
    memset(&info, 0, sizeof info);
    info.dlpi_addr = (ElfW(Addr))g_base;
    info.dlpi_name = "libgame.so";
    info.dlpi_phdr = g_phdr;
    info.dlpi_phnum = g_ehdr.e_phnum;
    int r = cb(&info, sizeof info, data);
    if (r) return r;
    return dl_iterate_phdr(cb, data);
}

// ---------------------------------------------------------------------------------------------------
// Crash report with names from the library's own symbol table
// ---------------------------------------------------------------------------------------------------
static void describe(const char *label, uintptr_t addr) {
    uintptr_t off;
    const char *n = lib_addr_name(addr, &off);
    if (n) logf_("  %s libgame+0x%lx  %s+0x%lx\n", label, addr - (uintptr_t)g_base, n, off);
    else logf_("  %s %p (outside libgame)\n", label, (void *)addr);
}

static void on_crash(int sig, siginfo_t *si, void *uc_) {
    ucontext_t *uc = uc_;
    logf_("\n*** signal %d, fault address %p\n", sig, si->si_addr);
    describe("pc ", uc->uc_mcontext.pc);
    describe("lr ", uc->uc_mcontext.regs[30]);
    uintptr_t fp = uc->uc_mcontext.regs[29];
    for (int i = 0; i < 24 && fp > 0x1000; i++) {
        uintptr_t *f = (uintptr_t *)fp;
        if (msync((void *)(fp & ~0xFFFUL), 4096, MS_ASYNC) != 0) break;
        describe("  at", f[1]);
        if (f[0] <= fp) break;
        fp = f[0];
    }
    stub_report();
    jni_report();
    _exit(70);
}

// ---------------------------------------------------------------------------------------------------
extern void gltrace_open(const char *), gltrace_frame(int), gltrace_size(int, int);
extern void jni_run_queue(void);
extern void *jni_new_prim_array(int len, int elem, const void *data);
extern int g_pending_notification_cb;

int main(int argc, char **argv) {
    // loader <libgame.so> <apk path reported to the game> <folder holding assets/ and files/> [frames]
    if (argc < 4) { fprintf(stderr, "usage: loader libgame.so <apk> <workdir> [frames]\n"); return 1; }
    g_log = stderr;
    g_runner = getenv("FFBE_RUNNER") != NULL;
    if (getenv("VERBOSE")) g_verbose = atoi(getenv("VERBOSE"));
    const char *apk = argv[2], *work = argv[3];
    int frames = argc > 4 ? atoi(argv[4]) : 0;        // 0 = until the window is closed
    if (!g_runner) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = on_crash;
        sa.sa_flags = SA_SIGINFO;
        sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL); sigaction(SIGILL, &sa, NULL); sigaction(SIGABRT, &sa, NULL);
        if (getenv("TRACE")) gltrace_open(getenv("TRACE"));
    }
    shim_init(apk, work);
    load_library(argv[1]);

    JavaVM_ *vm = jni_vm();
    JNIEnv_ *env = jni_env();
    install_traces();
    keep_music_through_notice_movies();
    int (*onload)(void *, void *) = lib_sym("JNI_OnLoad");
    int v = onload(vm, NULL);
    logf_("[jni] JNI_OnLoad returned 0x%x\n", v);
    if (frames < 0) { stub_report(); jni_report(); return 0; }

    void (*setApkPath)(void *, void *, void *) = lib_sym("Java_org_cocos2dx_lib_Cocos2dxHelper_nativeSetApkPath");
    void (*setContext)(void *, void *, void *, void *) = lib_sym("Java_org_cocos2dx_lib_Cocos2dxHelper_nativeSetContext");
    void *(*getAttrs)(void *, void *) = lib_sym("Java_org_cocos2dx_lib_Cocos2dxActivity_getGLContextAttrs");
    void (*nativeInit)(void *, void *, int, int) = lib_sym("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeInit");
    void (*surfaceChanged)(void *, void *, int, int) = lib_sym("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeOnSurfaceChanged");
    void (*render)(void *, void *) = lib_sym("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeRender");
    void (*touchBegin)(void *, void *, int, float, float) = lib_sym("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesBegin");
    void (*touchEnd)(void *, void *, int, float, float) = lib_sym("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesEnd");
    void (*touchMove)(void *, void *, void *, void *, void *) = lib_sym("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesMove");
    int (*keyEvent)(void *, void *, int, int) = lib_sym("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeKeyEvent");
    void (*onPause)(void *, void *) = lib_sym("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeOnPause");
    void (*onResume)(void *, void *) = lib_sym("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeOnResume");
    void (*notifCb)(void *, void *, int) = lib_sym("Java_com_square_1enix_android_1googleplay_FFBEWW_LapisJNI_notificationPermissionPromptCallback");
    int (*webParam)(void *, void *, void *) = lib_sym("Java_com_square_1enix_android_1googleplay_FFBEWW_LapisJNI_webViewOnReceivedParam");
    void (*backButton)(void *, void *) = lib_sym("Java_com_square_1enix_android_1googleplay_FFBEWW_LapisJNI_backButtonCallback");
    extern int g_web_visible;
    void *activity = jni_new_object("com/square_enix/android_googleplay/FFBEWW/Lapis");
    void *assets = jni_new_object("android/content/res/AssetManager");

    int W = getenv("WIDTH") ? atoi(getenv("WIDTH")) : 720, H = getenv("HEIGHT") ? atoi(getenv("HEIGHT")) : 1280;
    if (g_runner) hc(HC_WINDOW_SIZE, (long)&W, (long)&H, 0, 0);
    gltrace_size(W, H);
    setApkPath(env, activity, jni_new_string(apk));
    setContext(env, activity, activity, assets);
    getAttrs(env, activity);
    void (*insertText)(void *, void *, void *) = lib_sym("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeInsertText");
    void (*deleteBackward)(void *, void *) = lib_sym("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeDeleteBackward");
    extern int g_ime_open;
    logf_("[boot] starting the game at %dx%d\n", W, H);
    nativeInit(env, activity, W, H);
    surfaceChanged(env, activity, W, H);
    // Android resumes an activity once when it starts, and the game counts on it: its first "resume" only notes that
    // it has happened, and every later one brings the game back from the background (drawing, sound). Without this
    // first call, the first time the window came back from being minimised was taken for that first resume, and the
    // game stayed stopped and silent.
    onResume(env, activity);

    struct { int f; float x, y; } taps[64]; int ntaps = 0;
    for (const char *t = getenv("TAPS"); t && *t && ntaps < 64;) { if (sscanf(t, "%d:%f:%f", &taps[ntaps].f, &taps[ntaps].x, &taps[ntaps].y) == 3) ntaps++; t = strchr(t, ','); if (t) t++; }
    const char *notif = getenv("NOTIF_CB") ? getenv("NOTIF_CB") : g_runner ? "1" : NULL;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    double sum = 0, worst = 0; int cnt = 0, paused = 0;
    for (int i = 0; frames == 0 || i < frames; i++) {
        if (g_runner) {
            struct HcEvent ev;
            while (hc(HC_POLL_EVENT, (long)&ev, 0, 0, 0)) {
                if (ev.type == HEV_QUIT) { logf_("[boot] window closed\n"); jni_report(); exit(0); }
                if (ev.type == HEV_TOUCH_DOWN) touchBegin(env, activity, 0, ev.x, ev.y);
                if (ev.type == HEV_TOUCH_UP) touchEnd(env, activity, 0, ev.x, ev.y);
                if (ev.type == HEV_TOUCH_MOVE) { int id = 0; touchMove(env, activity, jni_new_prim_array(1, 4, &id), jni_new_prim_array(1, 4, &ev.x), jni_new_prim_array(1, 4, &ev.y)); }
                if (ev.type == HEV_TEXT && g_ime_open && insertText) {
                    char u[8]; int c = ev.key, n = 0;
                    if (c < 0x80) u[n++] = (char)c;
                    else if (c < 0x800) { u[n++] = (char)(0xC0 | c >> 6); u[n++] = (char)(0x80 | (c & 0x3F)); }
                    else if (c < 0x10000) { u[n++] = (char)(0xE0 | c >> 12); u[n++] = (char)(0x80 | ((c >> 6) & 0x3F)); u[n++] = (char)(0x80 | (c & 0x3F)); }
                    else { u[n++] = (char)(0xF0 | c >> 18); u[n++] = (char)(0x80 | ((c >> 12) & 0x3F)); u[n++] = (char)(0x80 | ((c >> 6) & 0x3F)); u[n++] = (char)(0x80 | (c & 0x3F)); }
                    u[n] = 0;
                    insertText(env, activity, jni_new_string(u));
                }
                if (ev.type == HEV_KEY_DOWN && g_ime_open && ev.key == 67 && deleteBackward) { deleteBackward(env, activity); continue; }
                if (ev.type == HEV_KEY_DOWN && g_ime_open && ev.key == 66 && insertText) { insertText(env, activity, jni_new_string("\n")); continue; }
                if ((ev.type == HEV_KEY_DOWN || ev.type == HEV_KEY_UP) && g_ime_open && (ev.key == 66 || ev.key == 67)) continue;
                if (ev.type == HEV_KEY_DOWN || ev.type == HEV_KEY_UP) {       // Backspace doubles as Android's Back when nothing is being typed
                    int code = ev.key == 67 ? 4 : ev.key;
                    // Android gives Back to the game twice over: as a key event, and (what the game's screens act on) as
                    // the activity's onBackPressed, which calls LapisJNI.backButtonCallback.
                    if (code == 4) { keyEvent(env, activity, code, ev.type == HEV_KEY_DOWN); if (ev.type == HEV_KEY_UP && backButton) backButton(env, NULL); }
                }
                if (ev.type == HEV_PAUSE && !paused) { paused = 1; onPause(env, activity); }
                if (ev.type == HEV_RESUME && paused) { paused = 0; onResume(env, activity); }
            }
        }
        for (int k = 0; k < ntaps; k++) {
            if (taps[k].f == i) { logf_("[input] tap at %.0f,%.0f (frame %d)\n", taps[k].x, taps[k].y, i); touchBegin(env, activity, 0, taps[k].x, taps[k].y); }
            if (taps[k].f + 4 == i) touchEnd(env, activity, 0, taps[k].x, taps[k].y);
        }
        jni_run_queue();
        if (g_runner && g_web_visible && webParam) {                 // a link in the in-game page that speaks to the game (ffbecall://...)
            char text[2048];
            if (hc(HC_WEB_PARAM, (long)text, sizeof text, 0, 0)) { logf_("[web] link to the game: %s\n", text); webParam(env, NULL, jni_new_string(text)); }
        }
        if (g_pending_notification_cb == 1 && notif) { g_pending_notification_cb = 2; if (notifCb) notifCb(env, NULL, atoi(notif)); }
        struct timespec r0, r1;
        clock_gettime(CLOCK_MONOTONIC, &r0);
        if (!paused) render(env, activity);
        clock_gettime(CLOCK_MONOTONIC, &r1);
        double ms = (r1.tv_sec - r0.tv_sec) * 1e3 + (r1.tv_nsec - r0.tv_nsec) / 1e6;
        sum += ms; cnt++; if (ms > worst) worst = ms;
        if (cnt == 600) { if (g_verbose) logf_("[perf] frames %d-%d: average %.2f ms, worst %.1f ms per frame\n", i - 599, i, sum / cnt, worst); sum = worst = 0; cnt = 0; }
        long paced = 0;
        if (g_runner) paced = hc(HC_PRESENT, 0, 0, 0, 0); else gltrace_frame(i);
        // 60 frames a second: the game's own animations are timed in real time. Either the display sets the pace (the
        // runner waited for its refresh while showing the frame), or the clock does.
        if (paced == 1) { clock_gettime(CLOCK_MONOTONIC, &next); continue; }
        next.tv_nsec += 16666667;
        if (next.tv_nsec >= 1000000000) { next.tv_nsec -= 1000000000; next.tv_sec++; }
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > next.tv_sec || (now.tv_sec == next.tv_sec && now.tv_nsec > next.tv_nsec)) next = now;
        else clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    }
    logf_("[boot] rendered %d frames\n", frames);
    stub_report();
    jni_report();
    exit(0);
}
