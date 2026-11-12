// The CPUs the guest program runs on. Two implementations: cpu_dyn.cpp (dynarmic, a recompiler; the default,
// several cores) and cpu_uc.c (Unicorn, slower, one core; kept for comparison). Each core is driven by one host
// thread, which first calls cpu_bind(); every other call below then acts on that thread's core. The scheduler in
// main.c saves and restores register sets (CpuCtx) to move guest threads on and off cores.
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct CpuCtx CpuCtx;
enum { CPU_SVC = 1, CPU_SLICE = 2, CPU_FAULT = 3 };
int cpu_init(uint8_t *arena_host, uint64_t guest_base, uint64_t size, int code_cache_mb, int cores);   // returns the number of cores set up
void cpu_bind(int core);              // the calling host thread drives this core from now on
int cpu_run(void);                    // runs until the guest makes a system call, a stop was requested, or it faults
void cpu_request_stop(int core);      // may be called from another thread
const char *cpu_fault_text(void);
uint64_t cpu_x(int i);
void cpu_set_x(int i, uint64_t v);
uint64_t cpu_sp(void);
uint64_t cpu_pc(void);
uint64_t cpu_tls(void);
void cpu_set_sp(uint64_t v);
void cpu_set_pc(uint64_t v);
void cpu_set_tls(uint64_t v);
float cpu_s(int i);                   // low 32 bits of vector register i, as a float argument
CpuCtx *cpu_ctx_new(void);
void cpu_ctx_free(CpuCtx *c);
void cpu_save(CpuCtx *c);
void cpu_restore(CpuCtx *c);          // also drops any half-finished exclusive load/store pair
void cpu_ctx_set_x0(CpuCtx *c, uint64_t v);
uint64_t cpu_ctx_reg(CpuCtx *c, int which);   // 29 = fp, 30 = lr, 32 = pc
int guest_page_ok(uint64_t addr);     // provided by main.c: is this guest address inside mapped guest memory
#ifdef __cplusplus
}
#endif
