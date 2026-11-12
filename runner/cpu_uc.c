// Unicorn backend (QEMU's translator as a library). Works, but memory-heavy and vector code runs many times
// slower than under the dynarmic backend.
#include <stdlib.h>
#include <string.h>
#include <unicorn/unicorn.h>
#include "cpu.h"

static uc_engine *uc;
static volatile int stop_req;
static int svc_hit, fault_hit;
static char fault_text[128];
static uint64_t clrex_stub;
struct CpuCtx { uc_context *c; };

static void on_block(uc_engine *u, uint64_t a, uint32_t s, void *ud) { (void)a; (void)s; (void)ud; if (stop_req) uc_emu_stop(u); }
static void on_intr(uc_engine *u, uint32_t intno, void *ud) {
    (void)ud;
    if (intno == 2) svc_hit = 1; else { fault_hit = 1; snprintf(fault_text, sizeof fault_text, "CPU exception %u", intno); }
    uc_emu_stop(u);
}

void cpu_bind(int core) { (void)core; }
static void cpu_init_one(uint8_t *arena, uint64_t base, uint64_t size, int cache_mb);
int cpu_init(uint8_t *arena, uint64_t base, uint64_t size, int cache_mb, int cores) { (void)cores; cpu_init_one(arena, base, size, cache_mb); return 1; }
static void cpu_init_one(uint8_t *arena, uint64_t base, uint64_t size, int cache_mb) {
    uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc);
    uc_ctl_set_cpu_model(uc, UC_CPU_ARM64_MAX);
    uc_ctl_tlb_mode(uc, UC_TLB_VIRTUAL);
    uc_ctl_set_tcg_buffer_size(uc, (uint32_t)cache_mb << 20);
    uc_mem_map_ptr(uc, base, size, UC_PROT_ALL, arena);
    uc_hook h1, h2;
    uc_hook_add(uc, &h1, UC_HOOK_INTR, (void *)on_intr, NULL, 1, 0);
    uc_hook_add(uc, &h2, UC_HOOK_BLOCK, (void *)on_block, NULL, 1, 0);
    uint64_t cpacr = 0x300000;
    uc_reg_write(uc, UC_ARM64_REG_CPACR_EL1, &cpacr);
    clrex_stub = base + size - 4096;                       // last page of guest memory: "clrex ; b ."
    uint32_t code[2] = {0xD503305F, 0x14000000};
    memcpy(arena + size - 4096, code, sizeof code);
}
int cpu_run(void) {
    svc_hit = fault_hit = 0; stop_req = 0;
    uint64_t pc; uc_reg_read(uc, UC_ARM64_REG_PC, &pc);
    uc_err e = uc_emu_start(uc, pc, 0xFFFFFFFFFFFFF000ULL, 0, 0);
    if (e != UC_ERR_OK) { snprintf(fault_text, sizeof fault_text, "%s", uc_strerror(e)); return CPU_FAULT; }
    return fault_hit ? CPU_FAULT : svc_hit ? CPU_SVC : CPU_SLICE;
}
void cpu_request_stop(int core) { (void)core; stop_req = 1; }
const char *cpu_fault_text(void) { return fault_text; }
static int xid(int i) { return i == 29 ? UC_ARM64_REG_X29 : i == 30 ? UC_ARM64_REG_X30 : UC_ARM64_REG_X0 + i; }
uint64_t cpu_x(int i) { uint64_t v = 0; uc_reg_read(uc, xid(i), &v); return v; }
void cpu_set_x(int i, uint64_t v) { uc_reg_write(uc, xid(i), &v); }
uint64_t cpu_sp(void) { uint64_t v = 0; uc_reg_read(uc, UC_ARM64_REG_SP, &v); return v; }
uint64_t cpu_pc(void) { uint64_t v = 0; uc_reg_read(uc, UC_ARM64_REG_PC, &v); return v; }
uint64_t cpu_tls(void) { uint64_t v = 0; uc_reg_read(uc, UC_ARM64_REG_TPIDR_EL0, &v); return v; }
void cpu_set_sp(uint64_t v) { uc_reg_write(uc, UC_ARM64_REG_SP, &v); }
void cpu_set_pc(uint64_t v) { uc_reg_write(uc, UC_ARM64_REG_PC, &v); }
void cpu_set_tls(uint64_t v) { uc_reg_write(uc, UC_ARM64_REG_TPIDR_EL0, &v); }
float cpu_s(int i) { uint64_t v = 0; uc_reg_read(uc, UC_ARM64_REG_S0 + i, &v); uint32_t lo = (uint32_t)v; float f; memcpy(&f, &lo, 4); return f; }
CpuCtx *cpu_ctx_new(void) { CpuCtx *c = calloc(1, sizeof *c); uc_context_alloc(uc, &c->c); return c; }
void cpu_ctx_free(CpuCtx *c) { uc_context_free(c->c); free(c); }
void cpu_save(CpuCtx *c) { uc_context_save(uc, c->c); }
void cpu_restore(CpuCtx *c) {
    uc_context_restore(uc, c->c);
    uint64_t pc = cpu_pc();
    stop_req = 0;
    uc_emu_start(uc, clrex_stub, clrex_stub + 4, 0, 0);
    cpu_set_pc(pc);
}
void cpu_ctx_set_x0(CpuCtx *c, uint64_t v) { uc_context_reg_write(c->c, UC_ARM64_REG_X0, &v); }
uint64_t cpu_ctx_reg(CpuCtx *c, int which) { uint64_t v = 0; uc_context_reg_read(c->c, which == 32 ? UC_ARM64_REG_PC : xid(which), &v); return v; }
