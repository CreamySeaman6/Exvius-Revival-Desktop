#pragma once
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "hc.h"
#include "cpu.h"

#define GUEST_BASE 0x10000ULL
#define ARENA_SIZE (4ULL << 30)
#define PAGE 4096ULL
#define MMAP_LO 0x40000000ULL
#define GUEST_ROOT "/ffbe"

extern uint8_t *arena;
extern char g_root[1024];      // host folder that the guest sees as /ffbe
extern int g_quit, g_exit_code;

void rlog(const char *fmt, ...);
void fatal(const char *fmt, ...);
void *g2h(uint64_t addr);
static inline uint64_t h2g(const void *p) { return p ? (uint64_t)((const uint8_t *)p - arena) + GUEST_BASE : 0; }
const char *gstr(uint64_t addr);
uint64_t guest_alloc(size_t len);              // page-granular allocation inside the guest's address space
uint64_t now_ns(void);                         // monotonic
uint64_t wall_ns(void);                        // since 1970
void symbolize(uint64_t addr, char *out, size_t cap);
void register_image(uint64_t base, uint64_t size, const char *host_path);

// platform.c
uint8_t *os_reserve(size_t size);
void os_commit(void *p, size_t len);
void os_guard(void *p, size_t len);              // make a reserved range fault on access
void os_release(void *p, size_t len);          // pages read back as zero afterwards
int os_open(const char *path, int linux_flags, int mode);
int os_stat(const char *path, void *linux_stat);    // fills a Linux aarch64 struct stat; 0 or -errno
int os_fstat(int hfd, void *linux_stat);
int os_mkdir(const char *path);
int os_unlink(const char *path);
int os_rmdir(const char *path);
int os_rename(const char *a, const char *b);
int os_truncate(int hfd, int64_t len);
int64_t os_lseek(int hfd, int64_t off, int whence);
int64_t os_read(int hfd, void *buf, size_t n);
int64_t os_write(int hfd, const void *buf, size_t n);
int64_t os_pread(int hfd, void *buf, size_t n, int64_t off);     // at a position, leaving the file's own position alone
int64_t os_pwrite(int hfd, const void *buf, size_t n, int64_t off);
void os_files_init(void);
void os_tune_process(void);                    // priority and power settings suited to a game
void os_close(int hfd);
void *os_opendir(const char *path);
int os_readdir(void *d, char *name, size_t cap, int *is_dir);
void os_closedir(void *d);
void os_random(void *buf, size_t n);
int os_utc_offset_seconds(void);
int os_text_input(const char *title, const char *initial, char *out, size_t cap);
void os_open_url(const char *url);
FILE *os_fopen(const char *path, const char *mode);   // UTF-8 path on every system
void os_args(int *argc, char ***argv);

// sched / syscalls (main.c)
int guest_path(uint64_t gaddr, char *out, size_t cap);     // guest path -> host path; 0 or -errno
void block_current_on_job(int job);
void block_current_sleep(uint64_t until_ns);
void overlay_file_written(const char *host_path);   // the game now has its own copy of this file (a finished download)

// host_gl.c
void video_init(int w, int h, const char *title);
void video_present(void);
int video_poll(struct HcEvent *ev);
void video_size(int *w, int *h);
uint64_t gl_call(int index, const uint64_t *x, uint64_t sp);
void video_shutdown(void);
int video_paced(void);                        // 1 while frames are timed by the display (vertical sync) instead of the game's own clock

// host_audio.c
int audio_open(int rate, int channels, int bits);
int audio_write(int id, const void *data, size_t bytes);
int audio_queued_ms(int id);
void audio_close(int id);
void audio_ctl(int id, int what, int value);
int audio_low_ms(void);
int audio_gaps(void);                         // times a playing sound ran dry and then carried on, since the last call
void audio_load_settings(void);                // volume and mute from the settings file; call once at start-up

// host_http.c
int http_start(struct HcHttp *req);            // returns job id
int http_done(int job);
void http_finish(int job, struct HcHttp *req); // copies status/lengths into req
void http_read(int job, void *headers, void *body);
void http_free(int job);
extern char g_server[128];

// host_aes.c
void aes_blocks(int flags, const uint8_t *key, const uint8_t *iv, uint8_t *buf, size_t len);
void md5_bytes(const uint8_t *data, size_t len, uint8_t out[16]);
int64_t decode_lines(int flags, const uint8_t *key, const uint8_t *iv, uint8_t *text, size_t len);
