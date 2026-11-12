// dynarmic backend: recompiles the ARM64 code to x86-64 and lets it use guest memory directly.
// One recompiler per core; they share guest memory and one exclusive-access monitor.
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include <dynarmic/interface/A64/a64.h>
#include <dynarmic/interface/A64/config.h>
#include <dynarmic/interface/exclusive_monitor.h>

#include "cpu.h"

using namespace Dynarmic;

static uint8_t *g_arena;
static uint64_t g_base, g_size;
static std::unique_ptr<ExclusiveMonitor> monitor;
static std::mutex wide_lock;                    // 128-bit exclusive stores, which have no single host instruction here

struct CpuCtx { std::array<uint64_t, 31> x; std::array<A64::Vector, 32> v; uint64_t sp, pc, tls; uint32_t fpcr, fpsr, pstate; };

static inline uint8_t *host(uint64_t a, size_t n) {
    if (a < g_base || a + n > g_base + g_size || !guest_page_ok(a) || !guest_page_ok(a + n - 1)) return nullptr;
    return g_arena + (a - g_base);
}
struct Core;
struct Env final : A64::UserCallbacks {
    Core *core = nullptr;
    void fault(const char *what, uint64_t addr);
    template <typename T> T rd(uint64_t a) { T v{}; if (uint8_t *p = host(a, sizeof(T))) std::memcpy(&v, p, sizeof(T)); else fault("invalid memory read", a); return v; }
    template <typename T> void wr(uint64_t a, const T &v) { if (uint8_t *p = host(a, sizeof(T))) std::memcpy(p, &v, sizeof(T)); else fault("invalid memory write", a); }
    // Exclusive stores must be one atomic step: other cores may be writing the same word.
    template <typename T> bool cas(uint64_t a, T v, T expected) {
        uint8_t *p = host(a, sizeof(T));
        if (!p) { fault("invalid memory write", a); return false; }
        return __atomic_compare_exchange_n((T *)p, &expected, v, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }
    std::uint8_t MemoryRead8(A64::VAddr a) override { return rd<uint8_t>(a); }
    std::uint16_t MemoryRead16(A64::VAddr a) override { return rd<uint16_t>(a); }
    std::uint32_t MemoryRead32(A64::VAddr a) override { return rd<uint32_t>(a); }
    std::uint64_t MemoryRead64(A64::VAddr a) override { return rd<uint64_t>(a); }
    A64::Vector MemoryRead128(A64::VAddr a) override { return rd<A64::Vector>(a); }
    void MemoryWrite8(A64::VAddr a, std::uint8_t v) override { wr(a, v); }
    void MemoryWrite16(A64::VAddr a, std::uint16_t v) override { wr(a, v); }
    void MemoryWrite32(A64::VAddr a, std::uint32_t v) override { wr(a, v); }
    void MemoryWrite64(A64::VAddr a, std::uint64_t v) override { wr(a, v); }
    void MemoryWrite128(A64::VAddr a, A64::Vector v) override { wr(a, v); }
    bool MemoryWriteExclusive8(A64::VAddr a, std::uint8_t v, std::uint8_t e) override { return cas(a, v, e); }
    bool MemoryWriteExclusive16(A64::VAddr a, std::uint16_t v, std::uint16_t e) override { return cas(a, v, e); }
    bool MemoryWriteExclusive32(A64::VAddr a, std::uint32_t v, std::uint32_t e) override { return cas(a, v, e); }
    bool MemoryWriteExclusive64(A64::VAddr a, std::uint64_t v, std::uint64_t e) override { return cas(a, v, e); }
    bool MemoryWriteExclusive128(A64::VAddr a, A64::Vector v, A64::Vector e) override {
        uint8_t *p = host(a, sizeof v);
        if (!p) { fault("invalid memory write", a); return false; }
        std::lock_guard<std::mutex> g(wide_lock);
        if (std::memcmp(p, &e, sizeof e) != 0) return false;
        std::memcpy(p, &v, sizeof v);
        return true;
    }
    void InterpreterFallback(A64::VAddr pc, size_t) override { fault("instruction the recompiler does not support", pc); }
    void CallSVC(std::uint32_t) override;
    void ExceptionRaised(A64::VAddr pc, A64::Exception e) override {
        char what[64]; std::snprintf(what, sizeof what, "CPU exception %d", (int)e);
        fault(what, pc);
    }
    void AddTicks(std::uint64_t) override {}
    std::uint64_t GetTicksRemaining() override { return 1ULL << 40; }
    std::uint64_t GetCNTPCT() override { return 0; }
};
struct Core {
    Env env;
    std::unique_ptr<A64::Jit> jit;
    uint64_t tpidr = 0, tpidrro = 0;
    bool svc_hit = false, fault_hit = false;
    char fault_text[160] = "";
};
void Env::fault(const char *what, uint64_t addr) {
    if (!core->fault_hit) std::snprintf(core->fault_text, sizeof core->fault_text, "%s at 0x%llx", what, (unsigned long long)addr);
    core->fault_hit = true;
    core->jit->HaltExecution(HaltReason::UserDefined2);
}
void Env::CallSVC(std::uint32_t) { core->svc_hit = true; core->jit->HaltExecution(HaltReason::UserDefined1); }

static std::vector<std::unique_ptr<Core>> cores;
static thread_local Core *me;

int cpu_init(uint8_t *arena, uint64_t base, uint64_t size, int cache_mb, int n) {
    g_arena = arena; g_base = base; g_size = size;
    if (n < 1) n = 1;
    monitor = std::make_unique<ExclusiveMonitor>((size_t)n);
    for (int i = 0; i < n; i++) {
        auto c = std::make_unique<Core>();
        c->env.core = c.get();
        A64::UserConfig cfg;
        cfg.callbacks = &c->env;
        cfg.processor_id = (size_t)i;
        cfg.global_monitor = monitor.get();
        cfg.tpidr_el0 = &c->tpidr;
        cfg.tpidrro_el0 = &c->tpidrro;
        cfg.dczid_el0 = 0x10;                       // tell the C library not to use "dc zva"
        cfg.cntfrq_el0 = 1000000000;
        cfg.fastmem_pointer = (uintptr_t)(arena - base);
        cfg.fastmem_address_space_bits = 36;
        cfg.silently_mirror_fastmem = false;
        cfg.fastmem_exclusive_access = true;
        cfg.enable_cycle_counting = false;
        // Atomic operations (reference counts, locks: the game does a great many) are done as one compare-and-swap on
        // the value. Without this every one of them also takes a lock shared by all cores, twice. The stricter way is
        // kept behind FFBE_STRICT_ATOMICS=1 in case a problem is ever traced to it.
        if (!getenv("FFBE_STRICT_ATOMICS")) {
            cfg.unsafe_optimizations = true;
            cfg.optimizations |= OptimizationFlag::Unsafe_IgnoreGlobalMonitor;
        }
        cfg.wall_clock_cntpct = true;
        // Core 0 runs the game's main thread, which touches the most code; the others get half as much room.
        cfg.code_cache_size = (size_t)(i == 0 ? cache_mb : cache_mb / 2) << 20;
        c->jit = std::make_unique<A64::Jit>(cfg);
        cores.push_back(std::move(c));
    }
    me = cores[0].get();
    return n;
}
void cpu_bind(int core) { me = cores[(size_t)core].get(); }
int cpu_run(void) {
    me->svc_hit = me->fault_hit = false;
    me->jit->Run();
    return me->fault_hit ? CPU_FAULT : me->svc_hit ? CPU_SVC : CPU_SLICE;
}
void cpu_request_stop(int core) { if ((size_t)core < cores.size()) cores[(size_t)core]->jit->HaltExecution(HaltReason::UserDefined3); }
const char *cpu_fault_text(void) { return me->fault_text; }
uint64_t cpu_x(int i) { return me->jit->GetRegister((size_t)i); }
void cpu_set_x(int i, uint64_t v) { me->jit->SetRegister((size_t)i, v); }
uint64_t cpu_sp(void) { return me->jit->GetSP(); }
uint64_t cpu_pc(void) { return me->jit->GetPC(); }
uint64_t cpu_tls(void) { return me->tpidr; }
void cpu_set_sp(uint64_t v) { me->jit->SetSP(v); }
void cpu_set_pc(uint64_t v) { me->jit->SetPC(v); }
void cpu_set_tls(uint64_t v) { me->tpidr = v; }
float cpu_s(int i) { A64::Vector v = me->jit->GetVector((size_t)i); uint32_t lo = (uint32_t)v[0]; float f; std::memcpy(&f, &lo, 4); return f; }
CpuCtx *cpu_ctx_new(void) { return new CpuCtx{}; }
void cpu_ctx_free(CpuCtx *c) { delete c; }
void cpu_save(CpuCtx *c) {
    A64::Jit *jit = me->jit.get();
    c->x = jit->GetRegisters(); c->v = jit->GetVectors(); c->sp = jit->GetSP(); c->pc = jit->GetPC(); c->tls = me->tpidr;
    c->fpcr = jit->GetFpcr(); c->fpsr = jit->GetFpsr(); c->pstate = jit->GetPstate();
}
void cpu_restore(CpuCtx *c) {
    A64::Jit *jit = me->jit.get();
    jit->SetRegisters(c->x); jit->SetVectors(c->v); jit->SetSP(c->sp); jit->SetPC(c->pc); me->tpidr = c->tls;
    jit->SetFpcr(c->fpcr); jit->SetFpsr(c->fpsr); jit->SetPstate(c->pstate);
    jit->ClearExclusiveState();
}
void cpu_ctx_set_x0(CpuCtx *c, uint64_t v) { c->x[0] = v; }
uint64_t cpu_ctx_reg(CpuCtx *c, int which) { return which == 32 ? c->pc : c->x[(size_t)which]; }
