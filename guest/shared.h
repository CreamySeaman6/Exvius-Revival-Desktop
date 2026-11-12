#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#pragma once
#include <link.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>

extern uint8_t *g_base;
extern size_t g_image_size;
extern FILE *g_log;
extern int g_verbose;
void logf_(const char *fmt, ...);
void *lib_sym(const char *name);
const char *lib_addr_name(uintptr_t addr, uintptr_t *off);
void stub_report(void);

typedef struct JNIEnv__ { void **functions; } JNIEnv_;
typedef struct JavaVM__ { void **functions; } JavaVM_;
JavaVM_ *jni_vm(void);
JNIEnv_ *jni_env(void);
void *jni_new_object(const char *cls);
void *jni_new_string(const char *utf);
void jni_report(void);

void shim_init(const char *apk, const char *work);
void *shim_lookup(const char *name);
extern const char *g_apk_path, *g_work_dir, *g_assets_dir;
int my_dl_iterate_phdr(int (*cb)(struct dl_phdr_info *, size_t, void *), void *data);
