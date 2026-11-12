// Stand-ins for the parts of Android's C library, asset manager, OpenGL ES and OpenSL ES whose data layouts
// or behaviour differ from glibc. Everything not listed here binds straight to glibc by name.
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <netdb.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "shared.h"
#include "hyper.h"

const char *g_apk_path, *g_work_dir, *g_assets_dir;
static char assets_dir_buf[1024];

// ---- stdio: bionic's stdin/stdout/stderr are slots in an array called __sF ---------------------------
static char bionic_sF[3][152];
static FILE *xf(FILE *f) {
    if ((char *)f >= bionic_sF[0] && (char *)f < bionic_sF[0] + sizeof bionic_sF) {
        int i = ((char *)f - bionic_sF[0]) / 152;
        return i == 0 ? stdin : i == 1 ? stdout : stderr;
    }
    return f;
}
static int my_fprintf(FILE *f, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vfprintf(xf(f), fmt, ap); va_end(ap); return r; }
static int my_vfprintf(FILE *f, const char *fmt, va_list ap) { return vfprintf(xf(f), fmt, ap); }
static int my_fputs(const char *s, FILE *f) { return fputs(s, xf(f)); }
static int my_fputc(int c, FILE *f) { return fputc(c, xf(f)); }
static size_t my_fwrite(const void *p, size_t a, size_t b, FILE *f) { return fwrite(p, a, b, xf(f)); }
static int my_fflush(FILE *f) { return fflush(f ? xf(f) : NULL); }
static void my_setbuf(FILE *f, char *b) { setbuf(xf(f), b); }
static int my_fileno(FILE *f) { return fileno(xf(f)); }
static int my_getc(FILE *f) { return getc(xf(f)); }
static char *my_fgets(char *s, int n, FILE *f) { return fgets(s, n, xf(f)); }

// ---- bionic-only helpers --------------------------------------------------------------------------------
static int *my_errno(void) { return &errno; }
static size_t my_strlen_chk(const char *s, size_t n) { (void)n; return strlen(s); }
static char *my_strchr_chk(const char *s, int c, size_t n) { (void)n; return strchr(s, c); }
static char *my_strrchr_chk(const char *s, int c, size_t n) { (void)n; return strrchr(s, c); }
static char *my_strncpy_chk2(char *d, const char *s, size_t n, size_t dl, size_t sl) { (void)dl; (void)sl; return strncpy(d, s, n); }
static mode_t my_umask_chk(mode_t m) { return umask(m); }
static void my_FD_SET_chk(int fd, fd_set *set, size_t n) { (void)n; FD_SET(fd, set); }
static void my_FD_CLR_chk(int fd, fd_set *set, size_t n) { (void)n; FD_CLR(fd, set); }
static int my_FD_ISSET_chk(int fd, fd_set *set, size_t n) { (void)n; return FD_ISSET(fd, set); }
static void my_set_abort_message(const char *m) { logf_("[abort message] %s\n", m ? m : ""); }
static uintptr_t my_stack_chk_guard = 0x5a5a1234deadbeefUL;
static int my_cxa_atexit(void (*f)(void *), void *a, void *d) { (void)f; (void)a; (void)d; return 0; }
static void my_cxa_finalize(void *d) { (void)d; }

static int my_log_print(int prio, const char *tag, const char *fmt, ...) {
    char buf[2048];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    logf_("[logcat %d %s] %s\n", prio, tag ? tag : "", buf);
    return 0;
}
static int my_log_vprint(int prio, const char *tag, const char *fmt, va_list ap) {
    char buf[2048];
    vsnprintf(buf, sizeof buf, fmt, ap);
    logf_("[logcat %d %s] %s\n", prio, tag ? tag : "", buf);
    return 0;
}
static int my_log_write(int prio, const char *tag, const char *msg) { logf_("[logcat %d %s] %s\n", prio, tag ? tag : "", msg ? msg : ""); return 0; }
static void my_log_assert(const char *cond, const char *tag, const char *fmt, ...) {
    char buf[2048] = "";
    if (fmt) { va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap); }
    logf_("[logcat ASSERT %s] %s %s\n", tag ? tag : "", cond ? cond : "", buf);
    abort();
}

// bionic's sysconf numbering differs from glibc's.
static long my_sysconf(int name) {
    switch (name) {
    case 0x27: case 0x28: return sysconf(_SC_PAGESIZE);
    case 0x60: return sysconf(_SC_NPROCESSORS_CONF);
    case 0x61: return sysconf(_SC_NPROCESSORS_ONLN);
    case 0x62: return sysconf(_SC_PHYS_PAGES);
    case 0x63: return sysconf(_SC_AVPHYS_PAGES);
    case 0x06: return sysconf(_SC_CLK_TCK);
    }
    logf_("[sysconf] unmapped bionic name 0x%x\n", name);
    return -1;
}

// ---- signals: bionic's sigset_t is 8 bytes, glibc's is 128; the game only installs crash handlers --------
static int my_sigaction(int s, const void *a, void *o) { (void)s; (void)a; if (o) memset(o, 0, 32); return 0; }
static void *my_signal(int s, void *h) { (void)s; (void)h; return NULL; }
static int my_sigprocmask(int h, const void *s, void *o) { (void)h; (void)s; if (o) memset(o, 0, 8); return 0; }
static int my_sigfillset(uint64_t *s) { *s = ~0UL; return 0; }
static int my_sigdelset(uint64_t *s, int n) { *s &= ~(1UL << (n - 1)); return 0; }

// ---- setjmp/longjmp: bionic's jmp_buf is 256 bytes, glibc's is 312 ---------------------------------------
__asm__(
    ".text\n.global my_setjmp\nmy_setjmp:\n"
    "  stp x19, x20, [x0, #0]\n  stp x21, x22, [x0, #16]\n  stp x23, x24, [x0, #32]\n"
    "  stp x25, x26, [x0, #48]\n  stp x27, x28, [x0, #64]\n  stp x29, x30, [x0, #80]\n"
    "  mov x2, sp\n  str x2, [x0, #96]\n"
    "  stp d8, d9, [x0, #104]\n  stp d10, d11, [x0, #120]\n  stp d12, d13, [x0, #136]\n  stp d14, d15, [x0, #152]\n"
    "  mov x0, #0\n  ret\n"
    ".global my_longjmp\nmy_longjmp:\n"
    "  ldp x19, x20, [x0, #0]\n  ldp x21, x22, [x0, #16]\n  ldp x23, x24, [x0, #32]\n"
    "  ldp x25, x26, [x0, #48]\n  ldp x27, x28, [x0, #64]\n  ldp x29, x30, [x0, #80]\n"
    "  ldr x2, [x0, #96]\n  mov sp, x2\n"
    "  ldp d8, d9, [x0, #104]\n  ldp d10, d11, [x0, #120]\n  ldp d12, d13, [x0, #136]\n  ldp d14, d15, [x0, #152]\n"
    "  cmp w1, #0\n  csinc w0, w1, wzr, ne\n  br x30\n");
extern int my_setjmp(void *);
extern void my_longjmp(void *, int);

// ---- pthreads: bionic's mutex is 40 bytes and its semaphore 16; keep a pointer to a real one in the slot ---
static pthread_mutex_t init_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t *new_mutex(int type) {
    pthread_mutex_t *m = malloc(sizeof *m);
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, type == 1 ? PTHREAD_MUTEX_RECURSIVE : type == 2 ? PTHREAD_MUTEX_ERRORCHECK : PTHREAD_MUTEX_NORMAL);
    pthread_mutex_init(m, &a);
    pthread_mutexattr_destroy(&a);
    return m;
}
static pthread_mutex_t *get_mutex(uintptr_t *slot) {
    uintptr_t v = *slot;
    if (v > 0xFFFF) return (pthread_mutex_t *)v;
    pthread_mutex_lock(&init_lock);
    v = *slot;
    if (v <= 0xFFFF) { v = (uintptr_t)new_mutex((v >> 14) & 3); *slot = v; }
    pthread_mutex_unlock(&init_lock);
    return (pthread_mutex_t *)v;
}
static int my_mutex_init(uintptr_t *slot, const long *attr) { *slot = (uintptr_t)new_mutex(attr ? (*attr & 3) : 0); return 0; }
static int my_mutex_destroy(uintptr_t *slot) { if (*slot > 0xFFFF) { pthread_mutex_destroy((pthread_mutex_t *)*slot); free((void *)*slot); } *slot = 0; return 0; }
static int my_mutex_lock(uintptr_t *slot) { return pthread_mutex_lock(get_mutex(slot)); }
static int my_mutex_trylock(uintptr_t *slot) { return pthread_mutex_trylock(get_mutex(slot)); }
static int my_mutex_unlock(uintptr_t *slot) { return pthread_mutex_unlock(get_mutex(slot)); }
static int my_mutexattr_init(long *a) { *a = 0; return 0; }
static int my_mutexattr_destroy(long *a) { (void)a; return 0; }
static int my_mutexattr_settype(long *a, int t) { *a = t; return 0; }
static int my_cond_wait(pthread_cond_t *c, uintptr_t *m) { return pthread_cond_wait(c, get_mutex(m)); }
static int my_cond_timedwait(pthread_cond_t *c, uintptr_t *m, const struct timespec *t) { return pthread_cond_timedwait(c, get_mutex(m), t); }
static int my_cond_init(pthread_cond_t *c, const void *attr) { (void)attr; return pthread_cond_init(c, NULL); }

static sem_t *get_sem(uintptr_t *slot) { return (sem_t *)*slot; }
static int my_sem_init(uintptr_t *slot, int shared, unsigned v) { sem_t *s = malloc(sizeof *s); *slot = (uintptr_t)s; return sem_init(s, shared, v); }
static int my_sem_destroy(uintptr_t *slot) { sem_destroy(get_sem(slot)); free(get_sem(slot)); *slot = 0; return 0; }
static int my_sem_post(uintptr_t *slot) { return sem_post(get_sem(slot)); }
static int my_sem_wait(uintptr_t *slot) { return sem_wait(get_sem(slot)); }

static int n_threads;
static int my_pthread_create(pthread_t *t, const void *attr, void *(*fn)(void *), void *arg) {
    (void)attr;
    uintptr_t off;
    const char *n = lib_addr_name((uintptr_t)fn, &off);
    if (n && strstr(n, "firebase")) {      // push-notification polling thread: not needed, and it spins without Android's file watcher
        logf_("[thread] not starting %s\n", "the Firebase messaging thread");
        *t = pthread_self();
        return 0;
    }
    if (g_verbose) logf_("[thread] #%d starts at %s\n", ++n_threads, n);
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 8 << 20);
    int r = pthread_create(t, &a, fn, arg);
    pthread_attr_destroy(&a);
    return r;
}
static int my_attr_noop(void *a) { (void)a; return 0; }
static int my_attr_noop2(void *a, void *b) { (void)a; (void)b; return 0; }

// ---- dl* -------------------------------------------------------------------------------------------------
static void *my_dlopen(const char *name, int flags) { (void)flags; logf_("[dlopen] %s\n", name ? name : "(self)"); return (void *)0x1; }
static void *my_dlsym(void *h, const char *name) {
    (void)h;
    void *p = lib_sym(name);
    if (!p) p = shim_lookup(name);
    logf_("[dlsym] %s -> %s\n", name, p ? "found" : "missing");
    return p;
}
static int my_dlclose(void *h) { (void)h; return 0; }
static const char *my_dlerror(void) { return NULL; }

// ---- getaddrinfo: bionic orders ai_canonname before ai_addr ----------------------------------------------
struct bionic_addrinfo { int flags, family, socktype, protocol; socklen_t addrlen; char *canonname; struct sockaddr *addr; struct bionic_addrinfo *next; };
static int my_getaddrinfo(const char *node, const char *service, const void *hints, void **res) {
    (void)service; (void)hints; (void)res;
    logf_("[net] the game tried to resolve %s directly; not supported\n", node ? node : "");
    return -2;
}
static void my_freeaddrinfo(void *a) { (void)a; }

// ---- Android asset manager, served from the APK's unpacked assets/ folder --------------------------------
typedef struct { FILE *f; long len; char path[1024]; } Asset;
static unsigned long asset_opens, asset_misses;
static void *my_AAssetManager_fromJava(void *env, void *obj) { (void)env; (void)obj; return (void *)0xA55E7; }
static Asset *my_AAssetManager_open(void *mgr, const char *name, int mode) {
    (void)mgr; (void)mode;
    char p[1200];
    snprintf(p, sizeof p, "%s/%s", g_assets_dir, name);
    FILE *f = fopen(p, "rb");
    asset_opens++;
    if (!f) { asset_misses++; if (g_verbose > 1) logf_("[asset] miss %s\n", name); return NULL; }
    if (g_verbose > 1) logf_("[asset] open %s\n", name);
    Asset *a = calloc(1, sizeof *a);
    a->f = f;
    fseek(f, 0, SEEK_END); a->len = ftell(f); fseek(f, 0, SEEK_SET);
    snprintf(a->path, sizeof a->path, "%s", p);
    return a;
}
static void my_AAsset_close(Asset *a) { if (a) { fclose(a->f); free(a); } }
static long my_AAsset_getLength(Asset *a) { return a->len; }
static int my_AAsset_read(Asset *a, void *buf, size_t n) { return (int)fread(buf, 1, n, a->f); }
static long my_AAsset_seek(Asset *a, long off, int whence) { fseek(a->f, off, whence); return ftell(a->f); }
static int my_AAsset_openFileDescriptor(Asset *a, long *start, long *len) { *start = 0; *len = a->len; return open(a->path, O_RDONLY); }
typedef struct { DIR *d; } AssetDir;
static AssetDir *my_AAssetManager_openDir(void *mgr, const char *name) {
    (void)mgr;
    char p[1200];
    snprintf(p, sizeof p, "%s/%s", g_assets_dir, name);
    AssetDir *ad = calloc(1, sizeof *ad);
    ad->d = opendir(p);
    return ad;
}
static const char *my_AAssetDir_getNextFileName(AssetDir *ad) {
    if (!ad->d) return NULL;
    struct dirent *e;
    while ((e = readdir(ad->d))) if (e->d_type == DT_REG) return e->d_name;
    return NULL;
}
static void my_AAssetDir_close(AssetDir *ad) { if (ad->d) closedir(ad->d); free(ad); }

// ---- OpenGL ES: a fake that says yes to everything, enough for the engine to believe it has a context ----
static unsigned gl_ids = 100;
static unsigned long gl_draws, gl_tex_uploads, gl_tex_bytes, gl_shaders, gl_programs;
static const char *my_glGetString(unsigned name) {
    switch (name) {
    case 0x1F00: return "Loader";
    case 0x1F01: return "Fake GLES";
    case 0x1F02: return "OpenGL ES 2.0 (fake)";
    case 0x1F03: return "GL_OES_rgb8_rgba8 GL_OES_depth24 GL_OES_packed_depth_stencil";
    case 0x8B8C: return "OpenGL ES GLSL ES 1.00";
    }
    return "";
}
static void my_glGetIntegerv(unsigned p, int *out) {
    switch (p) {
    case 0x0D33: *out = 4096; break;        // MAX_TEXTURE_SIZE
    case 0x8872: case 0x8B4D: *out = 8; break;
    case 0x8869: *out = 16; break;          // MAX_VERTEX_ATTRIBS
    case 0x0BA2: out[0] = 0; out[1] = 0; out[2] = 1280; out[3] = 720; break;
    default: *out = 0;
    }
}
static unsigned my_glGetError(void) { return 0; }
static unsigned my_glCreateShader(unsigned t) { (void)t; gl_shaders++; return ++gl_ids; }
static unsigned my_glCreateProgram(void) { gl_programs++; return ++gl_ids; }
static void my_glGetShaderiv(unsigned s, unsigned p, int *out) { (void)s; *out = p == 0x8B81 ? 1 : 0; }
static void my_glGetProgramiv(unsigned s, unsigned p, int *out) { (void)s; *out = p == 0x8B82 ? 1 : 0; }
static void gen_ids(int n, unsigned *out) { for (int i = 0; i < n; i++) out[i] = ++gl_ids; }
static unsigned my_glCheckFramebufferStatus(unsigned t) { (void)t; return 0x8CD5; }
static int my_glGetUniformLocation(unsigned p, const char *n) { (void)p; (void)n; return (int)(++gl_ids); }
static int my_glGetAttribLocation(unsigned p, const char *n) { (void)p; (void)n; return 0; }
static void my_glDrawArrays(unsigned m, int f, int c) { (void)m; (void)f; (void)c; gl_draws++; }
static void my_glDrawElements(unsigned m, int c, unsigned t, const void *i) { (void)m; (void)c; (void)t; (void)i; gl_draws++; }
static void my_glTexImage2D(unsigned t, int l, int f, int w, int h, int b, unsigned fmt, unsigned ty, const void *px) {
    (void)t; (void)l; (void)f; (void)b; (void)fmt; (void)ty; (void)px;
    gl_tex_uploads++; gl_tex_bytes += (unsigned long)w * h * 4;
    if (g_verbose > 1) logf_("[gl] texture %dx%d\n", w, h);
}
static void my_glCompressedTexImage2D(unsigned t, int l, unsigned f, int w, int h, int b, int size, const void *px) {
    (void)t; (void)l; (void)f; (void)b; (void)px; (void)w; (void)h;
    gl_tex_uploads++; gl_tex_bytes += size;
}
void asset_report(void) {
    logf_("  asset opens %lu (%lu not found in the APK, looked for inside the packs instead)\n", asset_opens, asset_misses);
}

// ---- OpenSL ES: silent fake objects so the audio engine initialises ---------------------------------------
typedef struct SLFake { void **vtbl; int kind; } SLFake;
static void *sl_obj_vtbl[16], *sl_itf_vtbl[40], *sl_engine_vtbl[40];
extern void *SL_IID_tab[8];
extern void sl_init_iids(void);
extern uint32_t real_slCreateEngine(void **out);
#define SL_IIDs SL_IID_tab
static unsigned long sl_calls;
static SLFake *sl_new(void **vtbl, int kind) { SLFake *o = calloc(1, sizeof *o); o->vtbl = vtbl; o->kind = kind; return o; }
static uint32_t sl_ok(void) { sl_calls++; return 0; }
static uint32_t sl_GetInterface(SLFake *self, void *iid, void **out) {
    (void)self; sl_calls++;
    *out = sl_new(iid == SL_IIDs[2] ? sl_engine_vtbl : sl_itf_vtbl, 1);
    return 0;
}
static uint32_t sl_Create(SLFake *self, void **out) { (void)self; sl_calls++; *out = sl_new(sl_obj_vtbl, 0); return 0; }
static uint32_t my_slCreateEngine(void **out) { if (g_runner) return real_slCreateEngine(out); logf_("[audio] slCreateEngine (silent fake)\n"); *out = sl_new(sl_obj_vtbl, 0); return 0; }
static void sl_init(void) {
    for (int i = 0; i < 16; i++) sl_obj_vtbl[i] = sl_ok;
    sl_obj_vtbl[3] = sl_GetInterface;
    for (int i = 0; i < 40; i++) { sl_itf_vtbl[i] = sl_ok; sl_engine_vtbl[i] = sl_ok; }
    sl_engine_vtbl[2] = sl_Create;  // CreateAudioPlayer
    sl_engine_vtbl[7] = sl_Create;  // CreateOutputMix
    sl_init_iids();
}

// ---- lookup table ----------------------------------------------------------------------------------------
typedef struct { const char *name; void *ptr; } Shim;
#define F(n, f) { n, (void *)(f) }
static Shim shims[] = {
    F("__sF", bionic_sF), F("__stack_chk_guard", &my_stack_chk_guard),
    F("fprintf", my_fprintf), F("vfprintf", my_vfprintf), F("fputs", my_fputs), F("fputc", my_fputc),
    F("fwrite", my_fwrite), F("fflush", my_fflush), F("setbuf", my_setbuf), F("fileno", my_fileno),
    F("getc", my_getc), F("fgets", my_fgets),
    F("__errno", my_errno), F("__strlen_chk", my_strlen_chk), F("__strchr_chk", my_strchr_chk),
    F("__strrchr_chk", my_strrchr_chk), F("__strncpy_chk2", my_strncpy_chk2), F("__umask_chk", my_umask_chk),
    F("__FD_SET_chk", my_FD_SET_chk), F("__FD_CLR_chk", my_FD_CLR_chk), F("__FD_ISSET_chk", my_FD_ISSET_chk),
    F("android_set_abort_message", my_set_abort_message),
    F("__cxa_atexit", my_cxa_atexit), F("__cxa_finalize", my_cxa_finalize),
    F("__android_log_print", my_log_print), F("__android_log_vprint", my_log_vprint),
    F("__android_log_write", my_log_write), F("__android_log_assert", my_log_assert),
    F("sysconf", my_sysconf),
    F("sigaction", my_sigaction), F("signal", my_signal), F("sigprocmask", my_sigprocmask),
    F("sigfillset", my_sigfillset), F("sigdelset", my_sigdelset),
    F("setjmp", my_setjmp), F("sigsetjmp", my_setjmp), F("longjmp", my_longjmp), F("siglongjmp", my_longjmp),
    F("pthread_mutex_init", my_mutex_init), F("pthread_mutex_destroy", my_mutex_destroy),
    F("pthread_mutex_lock", my_mutex_lock), F("pthread_mutex_trylock", my_mutex_trylock),
    F("pthread_mutex_unlock", my_mutex_unlock), F("pthread_mutexattr_init", my_mutexattr_init),
    F("pthread_mutexattr_destroy", my_mutexattr_destroy), F("pthread_mutexattr_settype", my_mutexattr_settype),
    F("pthread_cond_wait", my_cond_wait), F("pthread_cond_timedwait", my_cond_timedwait), F("pthread_cond_init", my_cond_init),
    F("sem_init", my_sem_init), F("sem_destroy", my_sem_destroy), F("sem_post", my_sem_post), F("sem_wait", my_sem_wait),
    F("pthread_create", my_pthread_create), F("pthread_attr_init", my_attr_noop),
    F("pthread_attr_setschedparam", my_attr_noop2), F("pthread_attr_setschedpolicy", my_attr_noop2),
    F("dlopen", my_dlopen), F("dlsym", my_dlsym), F("dlclose", my_dlclose), F("dlerror", my_dlerror),
    F("dl_iterate_phdr", my_dl_iterate_phdr),
    F("getaddrinfo", my_getaddrinfo), F("freeaddrinfo", my_freeaddrinfo),
    F("AAssetManager_fromJava", my_AAssetManager_fromJava), F("AAssetManager_open", my_AAssetManager_open),
    F("AAsset_close", my_AAsset_close), F("AAsset_getLength", my_AAsset_getLength), F("AAsset_getLength64", my_AAsset_getLength),
    F("AAsset_read", my_AAsset_read), F("AAsset_seek", my_AAsset_seek),
    F("AAsset_openFileDescriptor", my_AAsset_openFileDescriptor), F("AAsset_openFileDescriptor64", my_AAsset_openFileDescriptor),
    F("AAssetManager_openDir", my_AAssetManager_openDir), F("AAssetDir_getNextFileName", my_AAssetDir_getNextFileName),
    F("AAssetDir_close", my_AAssetDir_close),
    F("glGetString", my_glGetString), F("glGetIntegerv", my_glGetIntegerv), F("glGetError", my_glGetError),
    F("glCreateShader", my_glCreateShader), F("glCreateProgram", my_glCreateProgram),
    F("glGetShaderiv", my_glGetShaderiv), F("glGetProgramiv", my_glGetProgramiv),
    F("glGenTextures", gen_ids), F("glGenBuffers", gen_ids), F("glGenFramebuffers", gen_ids), F("glGenRenderbuffers", gen_ids),
    F("glCheckFramebufferStatus", my_glCheckFramebufferStatus),
    F("glGetUniformLocation", my_glGetUniformLocation), F("glGetAttribLocation", my_glGetAttribLocation),
    F("glDrawArrays", my_glDrawArrays), F("glDrawElements", my_glDrawElements),
    F("glTexImage2D", my_glTexImage2D), F("glCompressedTexImage2D", my_glCompressedTexImage2D),
    F("slCreateEngine", my_slCreateEngine),
    F("SL_IID_ANDROIDSIMPLEBUFFERQUEUE", &SL_IIDs[0]), F("SL_IID_BUFFERQUEUE", &SL_IIDs[1]), F("SL_IID_ENGINE", &SL_IIDs[2]),
    F("SL_IID_METADATAEXTRACTION", &SL_IIDs[3]), F("SL_IID_PLAY", &SL_IIDs[4]), F("SL_IID_PREFETCHSTATUS", &SL_IIDs[5]),
    F("SL_IID_SEEK", &SL_IIDs[6]), F("SL_IID_VOLUME", &SL_IIDs[7]),
};

extern void *gltrace_lookup(const char *name);
void *shim_lookup(const char *name) {
    void *g = g_runner ? NULL : gltrace_lookup(name);
    if (g) return g;
    for (size_t i = 0; i < sizeof shims / sizeof *shims; i++) if (strcmp(shims[i].name, name) == 0) return shims[i].ptr;
    return NULL;
}

void shim_init(const char *apk, const char *work) {
    g_apk_path = apk;
    g_work_dir = work;
    snprintf(assets_dir_buf, sizeof assets_dir_buf, "%s/assets", work);
    g_assets_dir = assets_dir_buf;
    sl_init();
}
