// A fake Java VM. It hands the game library a JNIEnv whose functions record every Java class, method and
// field the native code asks for, and answer with harmless defaults (or a few specific values), so the
// start-up path can be followed without Android.
#define _GNU_SOURCE
#include <link.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <regex.h>
#include <time.h>
#include <unistd.h>

#include "shared.h"
#include "http.h"
#include "hyper.h"
#include "text.h"

extern void *make_stub(const char *name);
extern void gl_report(void);
extern char *lapis_encode(const char *text, const char *key, const char *iv);
extern char *lapis_decode(const char *b64, const char *key);
extern char *lapis_encode_cbc(const char *text, const char *key, const char *iv);
extern char *lapis_decode_cbc(const char *b64, const char *key, const char *iv);
extern void asset_report(void);
extern void gltrace_text(const char *text, int len, int w, int h, int size);

enum { K_CLASS = 1, K_STRING, K_METHOD, K_FIELD, K_OBJECT, K_ARRAY };
typedef struct JObj {
    int kind;
    const char *cls;          // class name (class, object, method owner, field owner)
    char *utf;                // string contents
    const char *name, *sig;   // method / field
    unsigned long calls;
    void *data; int len, elem;
    struct JObj *next;
} JObj;

static pthread_mutex_t jlock = PTHREAD_MUTEX_INITIALIZER;
static JObj *classes, *members;
static JNIEnv_ g_env;
static JavaVM_ g_vm;
static void *env_table[240], *vm_table[8];
static unsigned long n_calls_total;
static char g_files_dir[1024];

static JObj *jnew(int kind) { JObj *o = calloc(1, sizeof *o); o->kind = kind; return o; }
void *jni_new_object(const char *cls) { JObj *o = jnew(K_OBJECT); o->cls = strdup(cls); return o; }
void *jni_new_prim_array(int len, int elem, const void *data);
void *jni_new_string_take(char *utf);     // takes ownership of a malloc'd string
void *jni_new_string(const char *utf) { JObj *o = jnew(K_STRING); o->cls = "java/lang/String"; o->utf = strdup(utf ? utf : ""); return o; }
void *jni_new_string_take(char *utf) { JObj *o = jnew(K_STRING); o->cls = "java/lang/String"; o->utf = utf ? utf : strdup(""); return o; }
static JObj *jarray(int len, int elem, const char *cls) {
    JObj *o = jnew(K_ARRAY); o->cls = cls; o->len = len; o->elem = elem; o->data = calloc(len ? len : 1, elem); return o;
}
void *jni_new_prim_array(int len, int elem, const void *data) { JObj *a = jarray(len, elem, "array"); memcpy(a->data, data, (size_t)len * elem); return a; }
static JObj *intern_class(const char *name) {
    pthread_mutex_lock(&jlock);
    JObj *c;
    for (c = classes; c; c = c->next) if (strcmp(c->cls, name) == 0) break;
    if (!c) { c = jnew(K_CLASS); c->cls = strdup(name); c->next = classes; classes = c; }
    pthread_mutex_unlock(&jlock);
    return c;
}
static JObj *intern_member(int kind, JObj *cls, const char *name, const char *sig) {
    const char *cn = cls && cls->cls ? cls->cls : "?";
    pthread_mutex_lock(&jlock);
    JObj *m;
    for (m = members; m; m = m->next) if (m->kind == kind && !strcmp(m->cls, cn) && !strcmp(m->name, name) && !strcmp(m->sig, sig)) break;
    if (!m) { m = jnew(kind); m->cls = strdup(cn); m->name = strdup(name); m->sig = strdup(sig); m->next = members; members = m; }
    pthread_mutex_unlock(&jlock);
    return m;
}

// ---- answers --------------------------------------------------------------------------------------------
typedef struct { uint64_t i; double d; } Ret;

// A Java exception waiting to be noticed by the game (ExceptionCheck / ExceptionOccurred / ExceptionClear).
static __thread JObj *pending_exception;
static void throw_java(const char *cls) { pending_exception = jni_new_object(cls); if (g_verbose) logf_("[java] throws %s\n", cls); }
static void *ExceptionOccurred(JNIEnv_ *e) { (void)e; return pending_exception; }
static long ExceptionCheck(JNIEnv_ *e) { (void)e; return pending_exception != NULL; }
static void ExceptionClear(JNIEnv_ *e) { (void)e; pending_exception = NULL; }

int g_web_visible, g_web_id;   // the in-game web page: asked for by the game and not yet dismissed; the game's own number for it
int g_ime_open;      // 1 while the game has asked for the on-screen keyboard: typed characters go to the game
static const char *string_answer(const char *name) {
    static const struct { const char *method, *value; } table[] = {
        {"getCocos2dxPackageName", "com.square_enix.android_googleplay.FFBEWW"},
        {"getPackageName", "com.square_enix.android_googleplay.FFBEWW"},
        {"getCurrentLanguage", "en"}, {"getCurrentCountry", "US"},
        {"getDeviceModel", "Desktop"}, {"getVersion", "9.1.0"}, {"getAppVersion", "9.1.0"},
        {"getVersionName", "9.1.0"},
        {"getDeviceID", "desktop-0001"},
        {"getAppID", "com.square_enix.android_googleplay.FFBEWW"}, {"getDeviceVersion", "11"}, {"getDeviceManufacturer", "Desktop"}, {"getABI", "arm64-v8a"},

    };
    for (size_t i = 0; i < sizeof table / sizeof *table; i++) if (!strcmp(table[i].method, name)) return table[i].value;
    if (!strcmp(name, "getCocos2dxWritablePath") || !strcmp(name, "getWritablePath") || !strcmp(name, "getAbsolutePath") || !strcmp(name, "getPath")) return g_files_dir;
    if (!strcmp(name, "getAssetsPath")) return g_apk_path;
    return "";
}

static void text_bitmap_callback(JObj **args, int nargs, long *ints, int nint, double *dbls, int ndbl);

// ---- file helpers the game reaches through LapisJNI ---------------------------------------------------------
// decodeFileForBase64[WithNewCrypto]: every line of the file is decrypted on its own; the results are joined with '\n'.
static char *decode_file(const char *path, const char *key, const char *iv, int *why) {
    FILE *f = fopen(path, "rb");
    if (!f) { *why = 1; return NULL; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *raw = malloc((size_t)n + 1);
    if (fread(raw, 1, (size_t)n, f) != (size_t)n) n = 0;
    raw[n] = 0; fclose(f);
    if (g_runner) {                                     // the whole file in one go, on the host
        unsigned char k[16] = {0}, v[16] = {0};
        size_t kl = strlen(key); memcpy(k, key, kl < 16 ? kl : 16);
        if (iv) { size_t il = strlen(iv); memcpy(v, iv, il < 16 ? il : 16); }
        long r = hc5(HC_DECODE_LINES, iv ? 3 : 1, (long)k, iv ? (long)v : 0, (long)raw, n);
        if (r < 0) {
            if (g_verbose) logf_("[java] %s does not decrypt as %s\n", path, iv ? "the new format" : "the old format");
            free(raw); *why = 2; return NULL;
        }
        raw[r] = 0;
        return raw;
    }
    char *out = malloc((size_t)n + 16); size_t o = 0; int lines = 0;
    for (char *p = raw; *p;) {
        char *e = p + strcspn(p, "\r\n"), was = *e;
        *e = 0;
        char *plain = iv ? lapis_decode_cbc(p, key, iv) : lapis_decode(p, key);
        if (!plain) {                                   // as in Java: one line that does not decrypt fails the whole file
            if (g_verbose) logf_("[java] %s does not decrypt as %s (line %d)\n", path, iv ? "the new format" : "the old format", lines + 1);
            free(out); free(raw); *why = 2; return NULL;
        }
        size_t pl = strlen(plain);
        if (lines++) out[o++] = '\n';
        memcpy(out + o, plain, pl); o += pl;
        free(plain);
        if (!was) break;
        p = e + 1;
        if (was == '\r' && *p == '\n') p++;
    }
    out[o] = 0;
    free(raw);
    if (g_verbose > 1) logf_("[java] decoded %s: %d lines, %zu bytes\n", path, lines, o);
    return out;
}
static void data_md5(const void *data, size_t len, char hex[40]) {
    unsigned char d[16] = {0};
    if (g_runner) hc(HC_MD5, (long)data, (long)len, (long)d, 0);
    for (int i = 0; i < 16; i++) snprintf(hex + i * 2, 3, "%02x", d[i]);
}
static void file_md5(const char *path, char hex[40]) {
    hex[0] = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *raw = malloc((size_t)n + 1);
    if (fread(raw, 1, (size_t)n, f) == (size_t)n) data_md5(raw, (size_t)n, hex);
    fclose(f); free(raw);
}
static void make_dirs(const char *path) {
    char p[1100]; snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
    mkdir(p, 0755);
}
static int remove_tree(const char *path) {
    if (strncmp(path, g_files_dir, strlen(g_files_dir)) != 0 || strstr(path, "..")) { logf_("[java] refused to delete outside the game's own folder: %s\n", path); return 0; }
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    if (!S_ISDIR(st.st_mode)) return unlink(path) == 0;
    DIR *d = opendir(path);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char child[1300]; snprintf(child, sizeof child, "%s/%s", path, e->d_name);
            remove_tree(child);
        }
        closedir(d);
    }
    return rmdir(path) == 0;
}
// Java's String.matches(regex): the whole string must match. Java's common classes are rewritten for POSIX.
static int java_matches(const char *s, const char *pattern) {
    char re[1024]; size_t o = 0;
    re[o++] = '^'; re[o++] = '(';
    for (const char *p = pattern; *p && o < sizeof re - 24; p++) {
        if (*p == '\\' && p[1]) {
            const char *rep = p[1] == 'd' ? "[0-9]" : p[1] == 'w' ? "[A-Za-z0-9_]" : p[1] == 's' ? "[[:space:]]" : NULL;
            if (rep) { o += (size_t)snprintf(re + o, sizeof re - o, "%s", rep); p++; continue; }
        }
        re[o++] = *p;
    }
    re[o++] = ')'; re[o++] = '$'; re[o] = 0;
    regex_t rx;
    if (regcomp(&rx, re, REG_EXTENDED | REG_NOSUB) != 0) return 0;
    int ok = regexec(&rx, s, 0, NULL, 0) == 0;
    regfree(&rx);
    return ok;
}

static struct Pref { char *k, *v; struct Pref *next; } *prefs;
static char prefs_path[1100];
static void prefs_save(void) {
    if (!prefs_path[0]) return;
    FILE *f = fopen(prefs_path, "w");
    if (!f) return;
    for (struct Pref *p = prefs; p; p = p->next) {
        fputs(p->k, f); fputc('\t', f);
        for (const char *c = p->v; *c; c++) { if (*c == '\n') fputs("\\n", f); else if (*c == '\\') fputs("\\\\", f); else fputc(*c, f); }
        fputc('\n', f);
    }
    fclose(f);
}
static void pref_put(const char *k, const char *v) {
    for (struct Pref *p = prefs; p; p = p->next) if (!strcmp(p->k, k)) { free(p->v); p->v = strdup(v); return; }
    struct Pref *p = calloc(1, sizeof *p); p->k = strdup(k); p->v = strdup(v); p->next = prefs; prefs = p;
}
static void pref_set(const char *k, const char *v) { pref_put(k, v); prefs_save(); }
static void prefs_load(const char *work) {
    snprintf(prefs_path, sizeof prefs_path, "%s/prefs.tsv", work);
    FILE *f = fopen(prefs_path, "r");
    if (!f) return;
    char *line = malloc(1 << 16);
    while (fgets(line, 1 << 16, f)) {
        char *tab = strchr(line, '\t'); if (!tab) continue;
        *tab = 0;
        char *v = tab + 1, *o = v;
        for (char *c = v; *c && *c != '\n'; c++) { if (*c == '\\' && c[1] == 'n') { *o++ = '\n'; c++; } else if (*c == '\\' && c[1] == '\\') { *o++ = '\\'; c++; } else *o++ = *c; }
        *o = 0;
        pref_put(line, v);
    }
    free(line);
    fclose(f);
}
static const char *pref_get(const char *k) {
    for (struct Pref *p = prefs; p; p = p->next) if (!strcmp(p->k, k)) return p->v;
    return "";
}

static void trace_here(const char *why) {
    logf_("[trace] %s\n", why);
    uintptr_t *fp = __builtin_frame_address(0);
    for (int i = 0; i < 30 && fp; i++) {
        uintptr_t off; const char *n = lib_addr_name(fp[1], &off);
        if (n) logf_("    %s+0x%lx\n", n, off);
        if ((uintptr_t *)fp[0] <= fp) break;
        fp = (uintptr_t *)fp[0];
    }
}
int g_pending_notification_cb;

// ---- work handed back to the game's main (GL) thread, as Android's UI thread would ----------------------
static void *registered(const char *name);
typedef struct Job { int kind; long a, b, c; char *s; JObj *o; struct Job *next; } Job;
static Job *jobs, **jobs_tail = &jobs;
static void post(Job j) { Job *n = malloc(sizeof *n); *n = j; n->next = NULL; pthread_mutex_lock(&jlock); *jobs_tail = n; jobs_tail = &n->next; pthread_mutex_unlock(&jlock); }
void jni_run_queue(void) {
    for (;;) {
        pthread_mutex_lock(&jlock);
        Job *j = jobs; if (j) { jobs = j->next; if (!jobs) jobs_tail = &jobs; }
        pthread_mutex_unlock(&jlock);
        if (!j) break;
        if (j->kind == 1) { void (*f)(JNIEnv_ *, void *, long) = lib_sym("Java_com_square_1enix_android_1googleplay_FFBEWW_FileDownloader_onDownloadComplete"); if (f) f(&g_env, NULL, j->a); }
        if (j->kind == 2) { void (*f)(JNIEnv_ *, void *, long, int, void *) = lib_sym("Java_com_square_1enix_android_1googleplay_FFBEWW_FileDownloader_onDownloadFailed"); if (f) f(&g_env, NULL, j->a, (int)j->b, jni_new_string("download failed")); }
        if (j->kind == 4) {
            void (*f)(JNIEnv_ *, void *, void *) = lib_sym("Java_org_cocos2dx_lib_Cocos2dxHelper_nativeSetEditTextDialogResult");
            JObj *arr = jarray((int)strlen(j->s), 1, "[B");
            memcpy(arr->data, j->s, strlen(j->s));
            if (f) f(&g_env, NULL, arr);
            free(j->s);
        }
        if (j->kind == 3) { void (*f)(JNIEnv_ *, void *, int, int, int, void *, void *) = registered("nativeOnFinish"); if (f) f(&g_env, NULL, (int)j->a, (int)j->b, (int)j->c, j->c ? jni_new_string("download failed") : NULL, j->o); }
        free(j);
    }
}
static unsigned long n_downloads, download_bytes;
static uint32_t crc32_of(const unsigned char *d, size_t n) { uint32_t c = ~0u; for (size_t i = 0; i < n; i++) { c ^= d[i]; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & -(c & 1)); } return ~c; }
static int fetch_to_crc(const char *url, const char *path, HttpConn *out, uint32_t *crc_out) {
    HttpConn h = {0}; h.url = (char *)url; strcpy(h.method, "GET");
    if (path && *path) {
        char dir[1024]; snprintf(dir, sizeof dir, "%s", path);
        for (char *q = dir + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(dir, 0755); *q = '/'; }
        long size = 0; int status = 0;
        uint32_t last_crc = 0;
        int rc = http_download(url, path, &size, &last_crc, &status);
        h.status = status; h.resp_len = (size_t)size;
        if (!rc && !g_runner) { FILE *f = fopen(path, "rb"); if (f) { unsigned char *d = malloc((size_t)size + 1); size_t n = fread(d, 1, (size_t)size, f); fclose(f); last_crc = crc32_of(d, n); free(d); } }
        if (crc_out) *crc_out = last_crc;
        if (out) *out = h;
        if (rc) return -1;
        n_downloads++; download_bytes += h.resp_len;
        return 0;
    }
    int rc = http_perform(&h);
    if (out) *out = h;
    if (rc != 0 || h.status != 200) return -1;
    n_downloads++; download_bytes += h.resp_len;
    return 0;
}
static int fetch_to(const char *url, const char *path, HttpConn *out) { return fetch_to_crc(url, path, out, NULL); }
// CriFsWebInstaller copies one file in the background; the game polls its status (0 stopped, 1 busy, 2 complete, 3 error).
typedef struct { volatile int status, http, error, gen; volatile long total, got; uint32_t crc; } CriDl;
typedef struct { CriDl *c; char *url, *path; int gen; } CriJob;
static void *cri_worker(void *arg) {
    CriJob *job = arg; CriDl *c = job->c;
    HttpConn h; uint32_t crc = 0;
    int rc = fetch_to_crc(job->url, job->path, &h, &crc);
    logf_("[download] CriFsWebInstaller %s -> %s (%s, %ld bytes)\n", job->url, job->path, rc ? "failed" : "ok", (long)h.resp_len);
    if (c->gen == job->gen) {                         // not stopped or reused in the meantime
        c->http = h.status; c->total = c->got = (long)h.resp_len; c->crc = crc; c->error = rc ? 7 : 0;
        c->status = rc ? 3 : 2;
    }
    free(job->url); free(job->path); free(job);
    return NULL;
}
// The three Java download helpers the game uses for its resource files.
static int download_java(JObj *target, JObj *m, JObj **o, int no, long *iv, int ni, Ret *r) {
    const char *n = m->name;
    if (!strcmp(m->cls, "com/square_enix/android_googleplay/FFBEWW/FileDownloader")) {
        if (!strcmp(n, "downloadFile") && no >= 2 && o[0] && o[1]) {
            int rc = fetch_to(o[0]->utf, o[1]->utf, NULL);
            logf_("[download] FileDownloader %s -> %s (%s)\n", o[0]->utf, o[1]->utf, rc ? "failed" : "ok");
            post((Job){.kind = rc ? 2 : 1, .a = ni > 0 ? iv[0] : 0, .b = 1}); return 1;
        }
        if (!strcmp(n, "deleteFile") && no >= 1 && o[0]) { remove(o[0]->utf); return 1; }
    }
    if (!strcmp(m->cls, "org/cocos2dx/lib/Cocos2dxDownloader")) {
        if (!strcmp(n, "createDownloader")) { JObj *d = jni_new_object("org/cocos2dx/lib/Cocos2dxDownloader"); d->len = ni > 0 ? (int)iv[0] : 0; r->i = (uint64_t)d; return 1; }
        if (!strcmp(n, "createTask") && no >= 3 && o[0] && o[1] && o[2]) {
            HttpConn h; int rc = fetch_to(o[1]->utf, o[2]->utf, &h);
            JObj *data = NULL;
            if (!rc && !o[2]->utf[0]) { data = jarray((int)h.resp_len, 1, "[B"); memcpy(data->data, h.resp_body, h.resp_len); }
            logf_("[download] Cocos2dxDownloader %s -> %s (%s)\n", o[1]->utf, o[2]->utf[0] ? o[2]->utf : "memory", rc ? "failed" : "ok");
            post((Job){.kind = 3, .a = o[0]->len, .b = ni > 0 ? iv[0] : 0, .c = rc ? 1 : 0, .o = data}); return 1;
        }
    }
    if (!strcmp(m->cls, "com/criware/filesystem/CriFsWebInstaller")) {
        if (!strcmp(n, "Create")) { JObj *d = jni_new_object("com/criware/filesystem/CriFsWebInstaller"); d->data = calloc(1, sizeof(CriDl)); r->i = (uint64_t)d; return 1; }
        CriDl *c = target && target->kind == K_OBJECT ? target->data : NULL;
        if (!c) return 0;
        if (!strcmp(n, "Copy") && no >= 2 && o[0] && o[1]) {
            CriJob *job = calloc(1, sizeof *job);
            job->c = c; job->url = strdup(o[0]->utf); job->path = strdup(o[1]->utf); job->gen = ++c->gen;
            c->http = 0; c->error = 0; c->total = c->got = 0; c->status = 1;
            pthread_t t; pthread_attr_t at;
            pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED); pthread_attr_setstacksize(&at, 256 * 1024);
            if (pthread_create(&t, &at, cri_worker, job) != 0) cri_worker(job);
            pthread_attr_destroy(&at);
            return 1;
        }
        if (!strcmp(n, "GetStatusInfo_status")) { r->i = c->status; return 1; }
        if (!strcmp(n, "GetStatusInfo_error")) { r->i = c->error; return 1; }
        if (!strcmp(n, "GetStatusInfo_http_status_code")) { r->i = c->http; return 1; }
        if (!strcmp(n, "GetStatusInfo_contents_size")) { r->i = c->total; return 1; }
        if (!strcmp(n, "GetStatusInfo_received_size")) { r->i = c->got; return 1; }
        if (!strcmp(n, "GetCRC32")) { r->i = c->crc; return 1; }
        if (!strcmp(n, "Stop")) { c->gen++; c->status = 0; return 1; }
    }
    return 0;
}
void download_report(void) { logf_("  resource downloads completed: %lu (%.1f MB)\n", n_downloads, download_bytes / 1048576.0); }

// The game does its HTTP through org.cocos2dx.lib.Cocos2dxHttpURLConnection. Answer those calls with a real request.
static int http_java(JObj *m, JObj **o, int no, long *iv, int ni, Ret *r) {
    const char *n = m->name;
    if (!strcmp(n, "createHttpURLConnection")) {
        JObj *c = jni_new_object("java/net/HttpURLConnection");
        HttpConn *h = calloc(1, sizeof *h); h->url = strdup(no > 0 && o[0] ? o[0]->utf : ""); c->data = h;
        r->i = (uint64_t)c; return 1;
    }
    HttpConn *h = no > 0 && o[0] ? o[0]->data : NULL;
    if (!h) return 0;
    if (!strcmp(n, "setRequestMethod")) { snprintf(h->method, sizeof h->method, "%s", no > 1 && o[1] ? o[1]->utf : "GET"); return 1; }
    if (!strcmp(n, "addRequestHeader") && no > 2 && o[1] && o[2]) {
        size_t add = strlen(o[1]->utf) + strlen(o[2]->utf) + 4;
        h->req_headers = realloc(h->req_headers, h->rh_len + add + 1);
        h->rh_len += sprintf(h->req_headers + h->rh_len, "%s: %s\r\n", o[1]->utf, o[2]->utf); return 1;
    }
    if (!strcmp(n, "connect")) { r->i = 0; return 1; }
    if (!strcmp(n, "sendRequest")) { if (no > 1 && o[1] && o[1]->kind == K_ARRAY) { h->body = o[1]->data; h->body_len = o[1]->len; } http_perform(h); return 1; }
    if (!strcmp(n, "getResponseCode")) { http_perform(h); r->i = h->status; return 1; }
    if (!strcmp(n, "getResponseHeaders")) { r->i = (uint64_t)jni_new_string(h->resp_headers ? h->resp_headers : ""); return 1; }
    if (!strcmp(n, "getResponseMessage")) { const char *sp = strchr(h->status_line, ' '); sp = sp ? strchr(sp + 1, ' ') : NULL; r->i = (uint64_t)jni_new_string(sp ? sp + 1 : "OK"); return 1; }
    if (!strcmp(n, "getResponseContent")) { JObj *a = jarray((int)h->resp_len, 1, "[B"); if (h->resp_len) memcpy(a->data, h->resp_body, h->resp_len); r->i = (uint64_t)a; return 1; }
    if (!strcmp(n, "getResponseHeaderByKey")) { const char *v = no > 1 && o[1] ? http_header(h, o[1]->utf) : NULL; r->i = v ? (uint64_t)jni_new_string(v) : 0; return 1; }
    if (!strcmp(n, "getResponseHeaderByKeyInt")) { const char *v = no > 1 && o[1] ? http_header(h, o[1]->utf) : NULL; r->i = v ? (uint64_t)atol(v) : 0; return 1; }
    if (!strcmp(n, "disconnect") || !strcmp(n, "setReadAndConnectTimeout") || !strcmp(n, "setVerifySSL")) return 1;
    return 0;
}
static Ret invoke(void *target_, JObj *m, va_list *ap, uint64_t *jv) {
    Ret r = {0, 0};
    JObj *target = target_;
    const char *tr = getenv("TRACE_METHOD");
    if (tr && m && m->kind == K_METHOD && !strcmp(m->name, tr) && m->calls == 3) trace_here(tr);
    if (m && m->kind == K_METHOD && !strcmp(m->name, "requestNotificationPermission")) g_pending_notification_cb = 1;
    if (!m || m->kind != K_METHOD) { logf_("[java] call through an invalid method id\n"); return r; }
    __sync_fetch_and_add(&n_calls_total, 1);
    unsigned long n = __sync_add_and_fetch(&m->calls, 1);
    char line[1500];
    int pos = snprintf(line, sizeof line, "[java] %s.%s(", m->cls, m->name);
    JObj *objs[40]; int nobj = 0; long ints[40]; int nint = 0; double dbls[16]; int ndbl = 0;
    const char *s = m->sig + 1;
    int first = 1;
    while (*s && *s != ')') {
        char buf[300] = "";
        switch (*s) {
        case 'Z': case 'B': case 'C': case 'S': case 'I': {
            long v = ap ? va_arg(*ap, int) : (int)*jv++;
            if (nint < 40) ints[nint++] = v;
            snprintf(buf, sizeof buf, "%ld", v); s++; break; }
        case 'J': { long v = ap ? va_arg(*ap, long) : (long)*jv++; if (nint < 40) ints[nint++] = v; snprintf(buf, sizeof buf, "%ldL", v); s++; break; }
        case 'F': { double v = ap ? va_arg(*ap, double) : (double)*(float *)jv++; if (ndbl < 16) dbls[ndbl++] = v; snprintf(buf, sizeof buf, "%g", v); s++; break; }
        case 'D': { double v = ap ? va_arg(*ap, double) : *(double *)jv++; if (ndbl < 16) dbls[ndbl++] = v; snprintf(buf, sizeof buf, "%g", v); s++; break; }
        default: {
            JObj *o = ap ? va_arg(*ap, JObj *) : (JObj *)*jv++;
            if (nobj < 40) objs[nobj++] = o;
            if (!o) snprintf(buf, sizeof buf, "null");
            else if (o->kind == K_STRING) snprintf(buf, sizeof buf, "\"%.90s%s\"", o->utf, strlen(o->utf) > 90 ? "..." : "");
            else if (o->kind == K_ARRAY) snprintf(buf, sizeof buf, "array[%d]", o->len);
            else snprintf(buf, sizeof buf, "<%s>", o->cls ? o->cls : "?");
            while (*s == '[') s++;
            if (*s == 'L') { while (*s && *s != ';') s++; }
            if (*s) s++;
        } }
        if (pos < (int)sizeof line - 320) pos += snprintf(line + pos, sizeof line - pos, "%s%s", first ? "" : ", ", buf);
        first = 0;
    }
    const char *rt = *s == ')' ? s + 1 : "V";
    if ((!strcmp(m->name, "loadClass") || !strcmp(m->name, "forName") || !strcmp(m->name, "findLoadedClass") || !strcmp(m->name, "findClass"))
        && nobj > 0 && objs[0] && objs[0]->kind == K_STRING) {
        char *cn = strdup(objs[0]->utf);
        for (char *q = cn; *q; q++) if (*q == '.') *q = '/';
        r.i = (uint64_t)intern_class(cn);
        return r;
    }
    if (!strcmp(m->cls, "org/cocos2dx/lib/Cocos2dxHttpURLConnection") && http_java(m, objs, nobj, ints, nint, &r)) goto done;
    if (download_java(target, m, objs, nobj, ints, nint, &r)) goto done;
    if (!strcmp(m->name, "encodeCStringForBase64") && nobj >= 2 && objs[0] && objs[1]) { r.i = (uint64_t)jni_new_string(lapis_encode(objs[0]->utf, objs[1]->utf, NULL)); goto done; }
    if (!strcmp(m->name, "encodeCStringForBase64WithNewCrypto") && nobj >= 3 && objs[0] && objs[1] && objs[2]) { r.i = (uint64_t)jni_new_string(lapis_encode_cbc(objs[0]->utf, objs[1]->utf, objs[2]->utf)); goto done; }
    if (!strcmp(m->name, "decodeCStringForBase64WithNewCrypto") && nobj >= 3 && objs[0] && objs[1] && objs[2]) {
        char *plain = lapis_decode_cbc(objs[0]->utf, objs[1]->utf, objs[2]->utf);
        if (plain) r.i = (uint64_t)jni_new_string_take(plain); else throw_java("javax/crypto/BadPaddingException");
        goto done;
    }
    if (!strcmp(m->name, "decodeCStringForBase64") && nobj >= 2 && objs[0] && objs[1]) {
        char *plain = lapis_decode(objs[0]->utf, objs[1]->utf);
        if (plain) r.i = (uint64_t)jni_new_string_take(plain); else throw_java("javax/crypto/BadPaddingException");
        goto done;
    }
    if ((!strcmp(m->name, "decodeFileForBase64WithNewCrypto") && nobj >= 3 && objs[0] && objs[1] && objs[2]) || (!strcmp(m->name, "decodeFileForBase64") && nobj >= 2 && objs[0] && objs[1])) {
        int why = 0;
        char *plain = decode_file(objs[0]->utf, objs[1]->utf, m->name[19] ? objs[2]->utf : NULL, &why);
        if (plain) r.i = (uint64_t)jni_new_string_take(plain); else throw_java(why == 1 ? "java/io/FileNotFoundException" : "javax/crypto/BadPaddingException");
        goto done;
    }
    if (!strcmp(m->name, "getCheckSum") && nobj >= 1 && objs[0]) { char hex[40]; file_md5(objs[0]->utf, hex); r.i = (uint64_t)jni_new_string(hex); goto done; }
    if (!strcmp(m->name, "getCheckSumForString") && nobj >= 1 && objs[0]) { char hex[40]; data_md5(objs[0]->utf, strlen(objs[0]->utf), hex); r.i = (uint64_t)jni_new_string(hex); goto done; }
    if (!strcmp(m->name, "makeDirectory") && nobj >= 1 && objs[0]) { make_dirs(objs[0]->utf); r.i = 1; goto done; }
    if ((!strcmp(m->name, "removeDirectory") || !strcmp(m->name, "deleteFileOrDirectory")) && nobj >= 1 && objs[0] && objs[0]->utf) { r.i = remove_tree(objs[0]->utf); goto done; }
    if (!strcmp(m->name, "getElapsedRealtimeSeconds")) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); r.i = (uint64_t)ts.tv_sec + 100000; goto done; }
    if (!strcmp(m->name, "getTimeZone")) {
        time_t now = time(NULL); struct tm tmv; localtime_r(&now, &tmv);
        long off = tmv.tm_gmtoff; char z[16];
        snprintf(z, sizeof z, "%c%02ld:%02ld", off < 0 ? '-' : '+', labs(off) / 3600, labs(off) % 3600 / 60);
        r.i = (uint64_t)jni_new_string(z); goto done;
    }
    if (!strcmp(m->name, "matchesInString") && nobj >= 2 && objs[0] && objs[1]) { r.i = (uint64_t)java_matches(objs[0]->utf, objs[1]->utf); goto done; }
    if (!strcmp(m->name, "killProcess") || (!strcmp(m->name, "finish") && !strcmp(m->cls, "android/app/Activity"))) { logf_("[java] the game asked to close\n"); exit(0); }
    if (!strcmp(m->name, "showEditTextDialog") && nobj >= 2) {              // (title, current text, mode, flag, return type, max length)
        char out[2048] = "";
        const char *title = objs[0] && objs[0]->utf ? objs[0]->utf : "", *text = objs[1] && objs[1]->utf ? objs[1]->utf : "";
        long ok = g_runner ? hc(HC_TEXT_INPUT, (long)title, (long)text, (long)out, sizeof out) : 0;
        int max = nint >= 4 ? (int)ints[3] : 0;
        if (ok && max > 0 && (int)strlen(out) > max) out[max] = 0;
        post((Job){.kind = 4, .s = strdup(ok ? out : text)});
        goto done;
    }
    if (!strcmp(m->name, "openIMEKeyboard")) { g_ime_open = 1; goto done; }
    if (!strcmp(m->name, "closeIMEKeyboard")) { g_ime_open = 0; goto done; }
    if ((!strcmp(m->name, "showNativePopup") || !strcmp(m->name, "showDialog")) && nobj >= 2 && objs[0] && objs[1]) { if (g_runner) hc(HC_MESSAGE, (long)(objs[0]->utf ? objs[0]->utf : ""), (long)(objs[1]->utf ? objs[1]->utf : ""), 0, 0); goto done; }
    if (!strcmp(m->name, "copyToClipboard") && nobj >= 1 && objs[0] && objs[0]->utf) { if (g_runner) hc(HC_CLIPBOARD, (long)objs[0]->utf, 0, 0, 0); goto done; }
    if ((!strcmp(m->name, "launchNewBrowser") || !strcmp(m->name, "openURL")) && nobj >= 1 && objs[0] && objs[0]->utf) { if (g_runner) hc(HC_OPEN_URL, (long)objs[0]->utf, 0, 0, 0); r.i = 1; goto done; }
    // In-game web pages (News notices, Drop Rates, reward lists). On Android a WebView sits on top of the game; here the
    // runner draws the page in the same rectangle. The game waits for isWebViewInvisible() before it leaves such a
    // screen, so these answers have to follow LapisWebView.java exactly.
    if (!strcmp(m->name, "showWebView") && nobj >= 2 && ndbl >= 4) {
        const char *url = objs[0] && objs[0]->utf ? objs[0]->utf : "", *post = objs[1] && objs[1]->utf ? objs[1]->utf : "";
        if (nint >= 1) g_web_id = (int)ints[0];
        g_web_visible = 1;
        logf_("[web] page %s (%s) at %g,%g size %gx%g\n", url, post, dbls[0], dbls[1], dbls[2], dbls[3]);
        if (g_runner) hc6(HC_WEB_SHOW, (long)url, (long)post, (long)dbls[0], (long)dbls[1], (long)(dbls[2] > 0 ? dbls[2] : 1000), (long)(dbls[3] > 0 ? dbls[3] : 1000));
        goto done;
    }
    if (!strcmp(m->name, "setWebViewVisible") && nint >= 1) { g_web_visible = ints[0] != 0; if (g_runner) hc(HC_WEB_VISIBLE, ints[0] != 0, 0, 0, 0); goto done; }
    if (!strcmp(m->name, "isWebViewVisible")) { r.i = (uint64_t)g_web_visible; goto done; }
    if (!strcmp(m->name, "isWebViewInvisible")) { r.i = !g_web_visible && (!g_runner || hc(HC_WEB_STATE, 0, 0, 0, 0) == 0); goto done; }
    if (!strcmp(m->name, "setViewGestureState") || !strcmp(m->name, "setViewScaleTransform")) goto done;
    if (!strcmp(m->name, "computeLabelSize") && nobj >= 1 && objs[0] && nint >= 1) {
        float w = 0, h = 0;
        text_measure(objs[0]->utf, (int)strlen(objs[0]->utf), (int)ints[0], &w, &h);
        void (*cb)(JNIEnv_ *, void *, float, float) = lib_sym("Java_org_cocos2dx_lib_Cocos2dxBitmap_onComputeLabelSizeFinished");
        if (cb) cb(&g_env, NULL, w, h);
        goto done;
    }
    if (!strcmp(m->name, "getUUID")) {
        if (!*pref_get("UUID")) {
            unsigned char b[16] = {0}; char u[40];
            FILE *f = fopen("/dev/urandom", "rb"); if (f) { if (fread(b, 1, 16, f) != 16) b[0] = 1; fclose(f); }
            snprintf(u, sizeof u, "%02x%02x%02x%02x-%02x%02x-4%01x%02x-a%01x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1], b[2], b[3], b[4], b[5], b[6] & 15, b[7], b[8] & 15, b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
            pref_set("UUID", u);
        }
        r.i = (uint64_t)jni_new_string(pref_get("UUID")); goto done;
    }
    if (!strcmp(m->name, "setSharedPrefString") && nobj >= 2 && objs[0] && objs[1]) {
        pref_set(objs[0]->utf, objs[1]->utf);
        // First run only. The game decides at start-up, from the tables it already has, which packs to fetch before
        // the tutorial (its movie, the first battle's layouts). A new install has no tables at start-up, so it would
        // walk into the tutorial without them. Once the tables are in, restart the game once: the next start-up
        // fetches those packs. The launcher starts the game again when it sees exit code 75.
        if (g_runner && !strcmp(objs[0]->utf, "keymst_downloaded_flg") && !strcmp(objs[1]->utf, "valid") &&
            !*pref_get("desktop_setup_restart") && !getenv("FFBE_NO_SETUP_RESTART")) {
            pref_set("desktop_setup_restart", "done");
            logf_("[setup] the game tables are installed; restarting once so the tutorial's packs are fetched at start-up\n");
            if (!getenv("FFBE_QUIET_RESTART"))
                hc(HC_MESSAGE, (long)"Exvius Revival Desktop", (long)"The game's tables are installed.\n\nThe game now restarts once to download what the tutorial needs. This only happens on the first run.", 0, 0);
            exit(75);
        }
    }
    if (!strcmp(m->name, "getSharedPrefString") && nobj >= 1 && objs[0]) { r.i = (uint64_t)jni_new_string(pref_get(objs[0]->utf)); goto done; }
    if (!strcmp(m->name, "createTextBitmapShadowStroke")) { text_bitmap_callback(objs, nobj, ints, nint, dbls, ndbl); r.i = 1; }
    else if (!strcmp(rt, "Ljava/lang/String;")) r.i = (uint64_t)jni_new_string(string_answer(m->name));
    else if (rt[0] == 'L') { size_t l = strlen(rt); char *cn = strndup(rt + 1, l - 2); r.i = (uint64_t)jni_new_object(cn); }
    else if (rt[0] == '[') r.i = (uint64_t)jarray(0, rt[1] == 'B' || rt[1] == 'Z' ? 1 : rt[1] == 'I' || rt[1] == 'F' ? 4 : 8, "array");
    else if (!strcmp(m->name, "getDPI")) r.i = 320;
    else if (!strcmp(m->name, "getDeviceStorageFreeSize")) r.i = 64000000000UL;
    else if (!strcmp(m->name, "getDeviceStorageSize")) r.i = 128000000000UL;
    else if (!strcmp(m->name, "getSDKVersion")) r.i = 30;
    else if (!strcmp(m->name, "getNetworkState")) r.i = 1;                 // 1 = Wi-Fi (0 = no signal, 2 = mobile data)
    else if (!strcmp(m->name, "getBatteryLevel")) r.d = 1.0;
    else if (!strcmp(m->name, "isBatteryCharging")) r.i = 1;
done:
    if (n <= 2 && g_verbose) logf_("%s) -> %s%s\n", line, rt, n == 2 ? "   (further calls counted only)" : "");
    return r;
}

// Cocos2dxBitmap draws text in Java and hands the pixels back. Return a small blank bitmap.
static unsigned long text_requests;
static void text_bitmap_callback(JObj **args, int nargs, long *ints, int nint, double *dbls, int ndbl) {
    text_requests++;
    void (*cb)(JNIEnv_ *, void *, int, int, void *) = lib_sym("Java_org_cocos2dx_lib_Cocos2dxBitmap_nativeInitBitmapDC");
    if (!(nargs > 0 && args[0] && args[0]->kind == K_ARRAY) || nint < 16) return;
    const char *t = args[0]->data; int len = args[0]->len;
    if (g_verbose > 1 || (g_verbose && text_requests <= 40)) logf_("[text] size %ld: \"%.*s\"\n", ints[0], len > 120 ? 120 : len, t);
    TextParams tp = {.size = (int)ints[0], .spacing = ndbl > 0 ? (float)dbls[0] : 1.0f, .r = (int)ints[1], .g = (int)ints[2], .b = (int)ints[3], .a = (int)ints[4],
                     .align = (int)ints[5], .width = (int)ints[6], .height = (int)ints[7], .stroke = (int)ints[9], .sr = (int)ints[10], .sg = (int)ints[11],
                     .sb = (int)ints[12], .sa = (int)ints[13], .stroke_size = ndbl > 5 ? (float)dbls[5] : 0, .wrap = (int)ints[14], .overflow = (int)ints[15]};
    int w = 0, h = 0; unsigned char *px = NULL;
    if (!text_render(t, len, &tp, &w, &h, &px)) return;
    if (!g_runner) gltrace_text(t, len, w, h, tp.size);
    JObj *arr = jarray(w * h * 4, 1, "[B");
    memcpy(arr->data, px, (size_t)w * h * 4);
    free(px);
    if (cb) cb(&g_env, NULL, w, h, arr);
}

#define CALL_T(T, ctype, expr) \
    static ctype Call##T(JNIEnv_ *e, void *o, JObj *m, ...) { (void)e; va_list ap; va_start(ap, m); Ret r = invoke(o, m, &ap, NULL); va_end(ap); (void)r; return expr; } \
    static ctype Call##T##V(JNIEnv_ *e, void *o, JObj *m, va_list ap) { (void)e; va_list c; va_copy(c, ap); Ret r = invoke(o, m, &c, NULL); va_end(c); (void)r; return expr; } \
    static ctype Call##T##A(JNIEnv_ *e, void *o, JObj *m, uint64_t *a) { (void)e; Ret r = invoke(o, m, NULL, a); (void)r; return expr; }
CALL_T(Object, void *, (void *)r.i)
CALL_T(Int, long, (long)r.i)
CALL_T(Float, float, (float)r.d)
CALL_T(Double, double, r.d)
static void CallVoid(JNIEnv_ *e, void *o, JObj *m, ...) { (void)e; va_list ap; va_start(ap, m); invoke(o, m, &ap, NULL); va_end(ap); }
static void CallVoidV(JNIEnv_ *e, void *o, JObj *m, va_list ap) { (void)e; va_list c; va_copy(c, ap); invoke(o, m, &c, NULL); va_end(c); }
static void CallVoidA(JNIEnv_ *e, void *o, JObj *m, uint64_t *a) { (void)e; invoke(o, m, NULL, a); }
#define CALLNV_T(T, ctype, expr) \
    static ctype CallNV##T(JNIEnv_ *e, void *o, void *c_, JObj *m, ...) { (void)e; (void)c_; va_list ap; va_start(ap, m); Ret r = invoke(o, m, &ap, NULL); va_end(ap); (void)r; return expr; } \
    static ctype CallNV##T##V(JNIEnv_ *e, void *o, void *c_, JObj *m, va_list ap) { (void)e; (void)c_; va_list c; va_copy(c, ap); Ret r = invoke(o, m, &c, NULL); va_end(c); (void)r; return expr; } \
    static ctype CallNV##T##A(JNIEnv_ *e, void *o, void *c_, JObj *m, uint64_t *a) { (void)e; (void)c_; Ret r = invoke(o, m, NULL, a); (void)r; return expr; }
CALLNV_T(Object, void *, (void *)r.i)
CALLNV_T(Int, long, (long)r.i)
CALLNV_T(Float, float, (float)r.d)
CALLNV_T(Double, double, r.d)
static void CallNVVoid(JNIEnv_ *e, void *o, void *c_, JObj *m, ...) { (void)e; (void)c_; va_list ap; va_start(ap, m); invoke(o, m, &ap, NULL); va_end(ap); }
static void CallNVVoidV(JNIEnv_ *e, void *o, void *c_, JObj *m, va_list ap) { (void)e; (void)c_; va_list c; va_copy(c, ap); invoke(o, m, &c, NULL); va_end(c); }
static void CallNVVoidA(JNIEnv_ *e, void *o, void *c_, JObj *m, uint64_t *a) { (void)e; (void)c_; invoke(o, m, NULL, a); }
static void *NewObject(JNIEnv_ *e, JObj *cls, JObj *m, ...) { (void)e; va_list ap; va_start(ap, m); invoke(cls, m, &ap, NULL); va_end(ap); return jni_new_object(cls ? cls->cls : "?"); }
static void *NewObjectV(JNIEnv_ *e, JObj *cls, JObj *m, va_list ap) { (void)e; va_list c; va_copy(c, ap); invoke(cls, m, &c, NULL); va_end(c); return jni_new_object(cls ? cls->cls : "?"); }
static void *NewObjectA(JNIEnv_ *e, JObj *cls, JObj *m, uint64_t *a) { (void)e; invoke(cls, m, NULL, a); return jni_new_object(cls ? cls->cls : "?"); }

// ---- the rest of JNIEnv ----------------------------------------------------------------------------------
static int GetVersion(JNIEnv_ *e) { (void)e; return 0x10006; }
static void *FindClass(JNIEnv_ *e, const char *name) { (void)e; return intern_class(name); }
static void *GetObjectClass(JNIEnv_ *e, JObj *o) { (void)e; return intern_class(o && o->cls ? o->cls : "java/lang/Object"); }
static void *GetSuperclass(JNIEnv_ *e, JObj *o) { (void)e; (void)o; return intern_class("java/lang/Object"); }
static void *GetMethodID(JNIEnv_ *e, JObj *cls, const char *name, const char *sig) { (void)e; return intern_member(K_METHOD, cls, name, sig); }
static void *GetFieldID(JNIEnv_ *e, JObj *cls, const char *name, const char *sig) { (void)e; return intern_member(K_FIELD, cls, name, sig); }
static void *GetObjectField(JNIEnv_ *e, void *o, JObj *f) {
    (void)e; (void)o;
    if (!f) return NULL;
    f->calls++;
    if (!strcmp(f->sig, "Ljava/lang/String;")) return jni_new_string("");
    return jni_new_object(f->sig);
}
static long GetIntField(JNIEnv_ *e, void *o, JObj *f) { (void)e; (void)o; if (f) { f->calls++; if (!strcmp(f->name, "SDK_INT")) return 30; } return 0; }
static void *same(JNIEnv_ *e, void *o) { (void)e; return o; }
static void noop(void) {}
static long zero(void) { return 0; }
static long one(void) { return 1; }
static int IsSameObject(JNIEnv_ *e, void *a, void *b) { (void)e; return a == b; }
static void *PopLocalFrame(JNIEnv_ *e, void *r) { (void)e; return r; }
static void *NewStringUTF(JNIEnv_ *e, const char *s) { (void)e; return jni_new_string(s); }
static void *NewString(JNIEnv_ *e, const uint16_t *s, int len) {
    (void)e;
    char *b = malloc(len + 1);
    for (int i = 0; i < len; i++) b[i] = s[i] < 128 ? (char)s[i] : '?';
    b[len] = 0;
    JObj *o = jni_new_string(b); free(b); return o;
}
static int GetStringUTFLength(JNIEnv_ *e, JObj *s) { (void)e; return s && s->utf ? (int)strlen(s->utf) : 0; }
static const char *GetStringUTFChars(JNIEnv_ *e, JObj *s, uint8_t *copy) { (void)e; if (copy) *copy = 0; return s && s->utf ? s->utf : ""; }
static const uint16_t *GetStringChars(JNIEnv_ *e, JObj *s, uint8_t *copy) {
    (void)e; if (copy) *copy = 1;
    int n = s && s->utf ? (int)strlen(s->utf) : 0;
    uint16_t *b = calloc(n + 1, 2);
    for (int i = 0; i < n; i++) b[i] = (uint8_t)s->utf[i];
    return b;
}
static void GetStringUTFRegion(JNIEnv_ *e, JObj *s, int start, int len, char *buf) { (void)e; memcpy(buf, s->utf + start, len); buf[len] = 0; }
static int GetArrayLength(JNIEnv_ *e, JObj *a) { (void)e; return a ? a->len : 0; }
static void *NewObjectArray(JNIEnv_ *e, int len, void *cls, void *init) { (void)e; (void)cls; (void)init; return jarray(len, 8, "[Ljava/lang/Object;"); }
static void *GetObjectArrayElement(JNIEnv_ *e, JObj *a, int i) { (void)e; return a && i < a->len ? ((void **)a->data)[i] : NULL; }
static void SetObjectArrayElement(JNIEnv_ *e, JObj *a, int i, void *v) { (void)e; if (a && i < a->len) ((void **)a->data)[i] = v; }
#define NEWARR(T, size) static void *New##T##Array(JNIEnv_ *e, int len) { (void)e; return jarray(len, size, "[" #T); }
NEWARR(Boolean, 1) NEWARR(Byte, 1) NEWARR(Char, 2) NEWARR(Short, 2) NEWARR(Int, 4) NEWARR(Long, 8) NEWARR(Float, 4) NEWARR(Double, 8)
static void *GetArrayElements(JNIEnv_ *e, JObj *a, uint8_t *copy) { (void)e; if (copy) *copy = 0; return a ? a->data : NULL; }
static void GetArrayRegion(JNIEnv_ *e, JObj *a, int start, int len, void *buf) { (void)e; memcpy(buf, (char *)a->data + (size_t)start * a->elem, (size_t)len * a->elem); }
static void SetArrayRegion(JNIEnv_ *e, JObj *a, int start, int len, const void *buf) { (void)e; memcpy((char *)a->data + (size_t)start * a->elem, buf, (size_t)len * a->elem); }

typedef struct { const char *name, *sig; void *fn; } NativeMethod;
static int n_registered;
static struct { char name[96]; void *fn; } reg_natives[64];
static void *registered(const char *name) { for (int i = 0; i < n_registered && i < 64; i++) if (!strcmp(reg_natives[i].name, name)) return reg_natives[i].fn; return NULL; }
static int RegisterNatives(JNIEnv_ *e, JObj *cls, const NativeMethod *m, int n) {
    (void)e;
    for (int i = 0; i < n; i++) { if (n_registered < 64) { snprintf(reg_natives[n_registered].name, 96, "%s", m[i].name); reg_natives[n_registered].fn = m[i].fn; } n_registered++; logf_("[jni] RegisterNatives %s.%s%s\n", cls ? cls->cls : "?", m[i].name, m[i].sig); }
    return 0;
}
static int GetJavaVM(JNIEnv_ *e, JavaVM_ **vm) { (void)e; *vm = &g_vm; return 0; }
static int vm_GetEnv(JavaVM_ *vm, void **env, int version) { (void)vm; (void)version; *env = &g_env; return 0; }
static int vm_Attach(JavaVM_ *vm, void **env, void *args) { (void)vm; (void)args; *env = &g_env; return 0; }

static const char *jni_names[233] = {
    [4] = "GetVersion", "DefineClass", "FindClass", "FromReflectedMethod", "FromReflectedField", "ToReflectedMethod",
    "GetSuperclass", "IsAssignableFrom", "ToReflectedField", "Throw", "ThrowNew", "ExceptionOccurred", "ExceptionDescribe",
    "ExceptionClear", "FatalError", "PushLocalFrame", "PopLocalFrame", "NewGlobalRef", "DeleteGlobalRef", "DeleteLocalRef",
    "IsSameObject", "NewLocalRef", "EnsureLocalCapacity", "AllocObject", "NewObject", "NewObjectV", "NewObjectA",
    "GetObjectClass", "IsInstanceOf", "GetMethodID",
    [94] = "GetFieldID", [113] = "GetStaticMethodID", [144] = "GetStaticFieldID",
    [163] = "NewString", "GetStringLength", "GetStringChars", "ReleaseStringChars", "NewStringUTF", "GetStringUTFLength",
    "GetStringUTFChars", "ReleaseStringUTFChars", "GetArrayLength", "NewObjectArray", "GetObjectArrayElement", "SetObjectArrayElement",
    [215] = "RegisterNatives", "UnregisterNatives", "MonitorEnter", "MonitorExit", "GetJavaVM", "GetStringRegion",
    "GetStringUTFRegion", "GetPrimitiveArrayCritical", "ReleasePrimitiveArrayCritical", "GetStringCritical", "ReleaseStringCritical",
    "NewWeakGlobalRef", "DeleteWeakGlobalRef", "ExceptionCheck", "NewDirectByteBuffer", "GetDirectBufferAddress",
    "GetDirectBufferCapacity", "GetObjectRefType",
};

static void set_calls(int base, void *obj[3], void *intf[3], void *flt[3], void *dbl[3], void *vd[3]) {
    // Order in the table: Object, Boolean, Byte, Char, Short, Int, Long, Float, Double, Void; three variants each.
    for (int t = 0; t < 10; t++)
        for (int v = 0; v < 3; v++)
            env_table[base + t * 3 + v] = t == 0 ? obj[v] : t == 7 ? flt[v] : t == 8 ? dbl[v] : t == 9 ? vd[v] : intf[v];
}

static void jni_setup(void) {
    for (int i = 0; i < 240; i++) {
        char nm[64];
        snprintf(nm, sizeof nm, "JNI:%s#%d", i < 233 && jni_names[i] ? jni_names[i] : "fn", i);
        env_table[i] = make_stub(strdup(nm));
    }
    void *obj[3] = {CallObject, CallObjectV, CallObjectA}, *in[3] = {CallInt, CallIntV, CallIntA};
    void *fl[3] = {CallFloat, CallFloatV, CallFloatA}, *db[3] = {CallDouble, CallDoubleV, CallDoubleA};
    void *vd[3] = {CallVoid, CallVoidV, CallVoidA};
    set_calls(34, obj, in, fl, db, vd);    // Call<T>Method
    set_calls(64, obj, in, fl, db, vd);    // CallNonvirtual<T>Method has an extra class argument; rarely used
    set_calls(114, obj, in, fl, db, vd);   // CallStatic<T>Method
    void *nobj[3] = {CallNVObject, CallNVObjectV, CallNVObjectA}, *nin[3] = {CallNVInt, CallNVIntV, CallNVIntA};
    void *nfl[3] = {CallNVFloat, CallNVFloatV, CallNVFloatA}, *ndb[3] = {CallNVDouble, CallNVDoubleV, CallNVDoubleA};
    void *nvd[3] = {CallNVVoid, CallNVVoidV, CallNVVoidA};
    set_calls(64, nobj, nin, nfl, ndb, nvd);
    env_table[4] = GetVersion; env_table[6] = FindClass; env_table[10] = GetSuperclass; env_table[11] = one;
    env_table[13] = zero; env_table[14] = zero; env_table[15] = ExceptionOccurred; env_table[16] = noop; env_table[17] = ExceptionClear;
    env_table[19] = zero; env_table[20] = PopLocalFrame; env_table[21] = same; env_table[22] = noop; env_table[23] = noop;
    env_table[24] = IsSameObject; env_table[25] = same; env_table[26] = zero;
    env_table[28] = NewObject; env_table[29] = NewObjectV; env_table[30] = NewObjectA;
    env_table[31] = GetObjectClass; env_table[32] = one; env_table[33] = GetMethodID;
    env_table[94] = GetFieldID; env_table[95] = GetObjectField;
    for (int i = 96; i <= 103; i++) env_table[i] = GetIntField;
    for (int i = 104; i <= 112; i++) env_table[i] = noop;
    env_table[113] = GetMethodID; env_table[144] = GetFieldID; env_table[145] = GetObjectField;
    for (int i = 146; i <= 153; i++) env_table[i] = GetIntField;
    for (int i = 154; i <= 162; i++) env_table[i] = noop;
    env_table[163] = NewString; env_table[164] = GetStringUTFLength; env_table[165] = GetStringChars; env_table[166] = noop;
    env_table[167] = NewStringUTF; env_table[168] = GetStringUTFLength; env_table[169] = GetStringUTFChars; env_table[170] = noop;
    env_table[171] = GetArrayLength; env_table[172] = NewObjectArray; env_table[173] = GetObjectArrayElement; env_table[174] = SetObjectArrayElement;
    void *news[8] = {NewBooleanArray, NewByteArray, NewCharArray, NewShortArray, NewIntArray, NewLongArray, NewFloatArray, NewDoubleArray};
    for (int i = 0; i < 8; i++) {
        env_table[175 + i] = news[i]; env_table[183 + i] = GetArrayElements; env_table[191 + i] = noop;
        env_table[199 + i] = GetArrayRegion; env_table[207 + i] = SetArrayRegion;
    }
    env_table[215] = RegisterNatives; env_table[216] = zero; env_table[217] = zero; env_table[218] = zero; env_table[219] = GetJavaVM;
    env_table[221] = GetStringUTFRegion; env_table[222] = GetArrayElements; env_table[223] = noop;
    env_table[224] = GetStringChars; env_table[225] = noop; env_table[226] = same; env_table[227] = noop; env_table[228] = ExceptionCheck;
    g_env.functions = env_table;
    vm_table[3] = zero; vm_table[4] = vm_Attach; vm_table[5] = zero; vm_table[6] = vm_GetEnv; vm_table[7] = vm_Attach;
    g_vm.functions = vm_table;
    snprintf(g_files_dir, sizeof g_files_dir, "%s/files", g_work_dir);
    prefs_load(g_work_dir);
}

JavaVM_ *jni_vm(void) { if (!g_env.functions) jni_setup(); return &g_vm; }
JNIEnv_ *jni_env(void) { if (!g_env.functions) jni_setup(); return &g_env; }

void jni_report(void) {
    int nclasses = 0, nmethods = 0, nfields = 0, called = 0;
    for (JObj *c = classes; c; c = c->next) nclasses++;
    for (JObj *m = members; m; m = m->next) { if (m->kind == K_METHOD) { nmethods++; if (m->calls) called++; } else nfields++; }
    logf_("\n== Java surface the library asked for ==\n");
    logf_("  classes looked up: %d, methods looked up: %d (%d actually called, %lu calls), fields: %d, natives registered: %d\n",
          nclasses, nmethods, called, n_calls_total, nfields, n_registered);
    logf_("  text bitmaps requested from Java: %lu\n", text_requests);
    logf_("  -- methods called (count  class.method signature) --\n");
    for (JObj *m = members; m; m = m->next) if (m->kind == K_METHOD && m->calls) logf_("  %7lu  %s.%s %s\n", m->calls, m->cls, m->name, m->sig);
    logf_("  -- methods looked up but not called --\n");
    for (JObj *m = members; m; m = m->next) if (m->kind == K_METHOD && !m->calls) logf_("           %s.%s %s\n", m->cls, m->name, m->sig);
    logf_("  -- fields --\n");
    for (JObj *m = members; m; m = m->next) if (m->kind == K_FIELD) logf_("           %s.%s %s\n", m->cls, m->name, m->sig);
    asset_report();
    http_report();
    download_report();
    gl_report();
}
