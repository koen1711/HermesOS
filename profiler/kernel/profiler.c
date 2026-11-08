#include "profiler.h"
#include "stackwalk.h"

#include <os/stdbool.h>
#include <os/stddef.h>
#include <drivers/scheduler/process.h>
#include <drivers/terminal/terminal.h>
#include <hardware/timer/tsc.h>
#include <hardware/interrupts/interrupts.h>
#include <hardware/memory/mmu.h>
#include <hardware/memory/pmm.h>
#include <hardware/smp/lapic.h>

#define PERF_EVT_SEL0 0xC0010200
#define PERF_CTR0     0xC0010201

#define EVENT_CYCLES_NOT_HALTED 0x76
#define EVTSEL_USR (1ULL << 16)
#define EVTSEL_OS  (1ULL << 17)
#define EVTSEL_INT (1ULL << 20)
#define EVTSEL_EN  (1ULL << 22)

#define COUNTER_MASK     ((1ULL << 48) - 1)
#define COUNTER_SIGN_BIT (1ULL << 47)

#define LVT_NMI    0x400
#define RFLAGS_IF  0x200
#define LVT_MASKED 0x10000

#define CPUID_EXT_FEATURES   0x80000001
#define CPUID_ECX_PERFCTR_CORE (1U << 23)

#define IST_SLOT_OFFSET 16
#define DEFAULT_RATE_HZ 1000
#define MAX_RATE_HZ 10000
#define STALE_WINDOW_DIVISOR 1000

#define IDT_VECTOR_NMI 2
#define IDT_KERNEL_CODE 0x08
#define IDT_INTERRUPT_GATE 0x8E
#define IDT_NMI_IST 1

#define USER_RPL 3

#define NMI_UNCLAIMED_ON_IST 0
#define NMI_CLAIMED 1

#define MAX_SELFTEST_SPIN_MS 10000
#define BUFFER_SIZE ((uint64_t)HERMES_PROFILER_BUFFER_MIB * 1024 * 1024)
#define BOOT_STACK_SIZE 0x4000 // STACK_SIZE in start.asm

_Static_assert(sizeof(idt_entry) == 16, "the NMI gate is written as two 64-bit halves");
_Static_assert(offsetof(interrupt_context, rbp) == CTX_RBP_OFFSET, "stackwalk's rbp offset");
_Static_assert(offsetof(interrupt_context, rip) == CTX_RIP_OFFSET, "stackwalk's rip offset");
_Static_assert(offsetof(interrupt_context, cs) == CTX_CS_OFFSET, "stackwalk's cs offset");
_Static_assert(sizeof(interrupt_context) == CTX_SIZE, "stackwalk's context size");
_Static_assert(sizeof(struct profiler_sample) == 32, "the host parses this record");
_Static_assert(sizeof(struct profiler_cpu_state) == 112, "the host parses this struct");
_Static_assert(sizeof(struct profiler_control) == 64 + MAX_CPUS * sizeof(struct profiler_cpu_state),
               "the host parses this struct");

struct profiler_percpu {
    struct cpu* cpu;
    struct profiler_cpu_state* state;
    uint8_t* buffer;
    uint64_t reload;
    uint64_t last_event_tsc;
};

extern void profiler_nmi_entry();
extern char _kernel_physical_start[];
extern char _boot_end[];
extern char tmp_stack[];

struct profiler_control profiler_control;

static struct profiler_percpu percpu[MAX_CPUS];
static uint64_t stale_window_tsc;
static volatile bool ready;

static inline uint64_t rdmsr(const uint32_t msr)
{
    uint32_t lo, hi;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return (uint64_t)hi << 32 | lo;
}

static inline void wrmsr(const uint32_t msr, const uint64_t value)
{
    asm volatile("wrmsr" : : "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32)));
}

static inline uint64_t rdtsc()
{
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t)hi << 32 | lo;
}

static inline void cpuid(const uint32_t leaf, uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d)
{
    asm volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}

static bool pmu_available()
{
    uint32_t a, b, c, d;
    cpuid(0, &a, &b, &c, &d);
    const bool amd = b == 0x68747541 && d == 0x69746E65 && c == 0x444D4163; // "AuthenticAMD"
    if (!amd || !lapic_present())
        return false;
    cpuid(0x80000000, &a, &b, &c, &d);
    if (a < CPUID_EXT_FEATURES)
        return false;
    cpuid(CPUID_EXT_FEATURES, &a, &b, &c, &d);
    return c & CPUID_ECX_PERFCTR_CORE;
}

static void counter_stop()
{
    wrmsr(PERF_EVT_SEL0, 0);
    lapic_set_perf_lvt(LVT_MASKED);
}

static void disarm(struct profiler_percpu* self)
{
    struct profiler_cpu_state* state = self->state;

    counter_stop();
    const uint64_t now = rdtsc();
    state->stop_tsc = now;
    self->last_event_tsc = now;
    __atomic_store_n(&state->status, PROFILER_OFF, __ATOMIC_RELEASE);

    lapic_set_perf_lvt(LVT_MASKED);
}

static void arm(struct profiler_percpu* self, const uint64_t generation)
{
    struct profiler_cpu_state* state = self->state;

    if (state->status == PROFILER_ARMED)
        disarm(self);

    if (!self->buffer) {
        state->armed_generation = generation;
        state->status = PROFILER_NO_BUFFER;
        return;
    }

    if (!pmu_available()) {
        state->armed_generation = generation;
        state->status = PROFILER_NO_PMU;
        return;
    }

    state->nmi_count = 0;
    state->samples = 0;
    state->dropped = 0;
    state->truncated = 0;
    state->user_samples = 0;
    state->write_offset = 0;
    state->handler_tsc_total = 0;
    state->stop_tsc = 0;
    state->spurious = 0;

    uint64_t requested_hz = profiler_control.rate_hz;
    uint64_t hz = requested_hz;
    if (hz == 0)
        hz = DEFAULT_RATE_HZ;
    else if (hz > MAX_RATE_HZ)
        hz = MAX_RATE_HZ;
    if (hz != requested_hz)
        __atomic_compare_exchange_n(&profiler_control.rate_hz, &requested_hz, hz, false, __ATOMIC_RELAXED,
                                    __ATOMIC_RELAXED);
    self->reload = -(profiler_control.tsc_hz / hz) & COUNTER_MASK;

    state->start_tsc = rdtsc();

    lapic_set_perf_lvt(LVT_NMI);
    wrmsr(PERF_CTR0, self->reload);
    wrmsr(PERF_EVT_SEL0, EVENT_CYCLES_NOT_HALTED | EVTSEL_USR | EVTSEL_OS | EVTSEL_INT | EVTSEL_EN);

    state->armed_generation = generation;
    __atomic_store_n(&state->status, PROFILER_ARMED, __ATOMIC_RELEASE);
}

static uint64_t move_frame_to_kernel_stack(const struct profiler_percpu* self, const interrupt_context* frame)
{
    uint64_t* to = (uint64_t*)((self->cpu->kernel_rsp & ~(uint64_t)0xF) - 5 * sizeof(uint64_t));
    to[0] = frame->rip;
    to[1] = frame->cs;
    to[2] = frame->rflags;
    to[3] = frame->rsp;
    to[4] = frame->ss;
    return (uint64_t)to;
}

static void record_sample(const struct profiler_percpu* self, const interrupt_context* frame, const uint64_t tsc)
{
    struct profiler_cpu_state* state = self->state;
    const bool user = (frame->cs & 3) == USER_RPL;
    uint64_t frames[PROFILER_MAX_DEPTH];
    size_t depth = 0;
    bool truncated = false;

    if (!user) {
        // CPU 0's idle context runs on the boot stack, which kernel_rsp doesn't describe
        const uint64_t boot_low = (uint64_t)tmp_stack;
        const bool on_boot_stack = frame->rsp >= boot_low && frame->rsp < boot_low + BOOT_STACK_SIZE;
        const uint64_t stack_high = on_boot_stack ? boot_low + BOOT_STACK_SIZE : self->cpu->kernel_rsp;
        const uint64_t stack_low = on_boot_stack ? boot_low : stack_high - KERNEL_STACK_SIZE;
        if (frame->rsp >= stack_low && frame->rsp < stack_high) {
            const struct stackwalk_bounds bounds = {
                .stack_low = stack_low,
                .stack_high = stack_high,
                .text_low = (uint64_t)_kernel_physical_start,
                .text_high = (uint64_t)_boot_end,
                .trap_return = (uint64_t)trap_return,
            };
            depth = stackwalk(frame->rbp, &bounds, frames, PROFILER_MAX_DEPTH, &truncated);
        }
    }

    const uint64_t offset = state->write_offset;
    const uint64_t size = sizeof(struct profiler_sample) + depth * sizeof(uint64_t);
    if (state->dropped || size > state->buffer_size - offset) {
        state->dropped++;
        return;
    }

    struct profiler_sample* sample = (struct profiler_sample*)(self->buffer + offset);
    sample->tsc = tsc;
    sample->rip = frame->rip;
    sample->depth = depth;
    sample->cpu = self->cpu->index;
    sample->flags = (user ? PROFILER_SAMPLE_USER : 0) | (truncated ? PROFILER_SAMPLE_TRUNCATED : 0)
                    | (frame->rflags & RFLAGS_IF ? PROFILER_SAMPLE_INTERRUPTS_ON : 0);
    sample->reserved = 0;
    sample->rbp = frame->rbp;
    uint64_t* out = (uint64_t*)(sample + 1);
    for (size_t i = 0; i < depth; i++)
        out[i] = frames[i];

    state->samples++;
    if (user)
        state->user_samples++;
    if (truncated)
        state->truncated++;
    __atomic_store_n(&state->write_offset, offset + size, __ATOMIC_RELEASE);
}

uint64_t profiler_nmi_handler(interrupt_context* frame, struct profiler_percpu** slot)
{
    const uint64_t start = rdtsc();
    struct profiler_percpu* self = *slot;
    struct profiler_cpu_state* state = self->state;
    const bool armed = __atomic_load_n(&state->status, __ATOMIC_ACQUIRE) == PROFILER_ARMED;

    if (armed && !(rdmsr(PERF_CTR0) & COUNTER_SIGN_BIT)) {
        state->nmi_count++;
        record_sample(self, frame, start);
        wrmsr(PERF_CTR0, self->reload);
        lapic_set_perf_lvt(LVT_NMI);
        const uint64_t end = rdtsc();
        state->handler_tsc_total += end - start;
        self->last_event_tsc = end;
        return NMI_CLAIMED;
    }

    if (self->last_event_tsc && start - self->last_event_tsc < stale_window_tsc) {
        state->spurious++;
        if (armed)
            lapic_set_perf_lvt(LVT_NMI);
        return NMI_CLAIMED;
    }

    if ((frame->cs & 3) == USER_RPL)
        return move_frame_to_kernel_stack(self, frame);
    return NMI_UNCLAIMED_ON_IST;
}

__attribute__((noinline)) void profiler_selftest_spin(const uint64_t ms)
{
    // A frameless leaf would hide profiler_tick from the walker, which starts at the interrupted rbp
    asm volatile("" : : "r"(__builtin_frame_address(0)));
    const uint64_t start = __builtin_ia32_rdtsc();
    const uint64_t ticks = ms * (profiler_control.tsc_hz / 1000);
    while (__builtin_ia32_rdtsc() - start < ticks)
        ;
}

void profiler_tick(void)
{
    if (!ready)
        return;

    struct profiler_percpu* self = &percpu[cpu_this()->index];
    struct profiler_cpu_state* state = self->state;

    if (profiler_control.requested) {
        const uint64_t generation = profiler_control.generation;
        if (generation != state->armed_generation)
            arm(self, generation);
    } else if (state->status == PROFILER_ARMED) {
        disarm(self);
    }

    if (self == &percpu[0]) {
        const uint64_t spin_ms = profiler_control.selftest_spin_ms;
        if (spin_ms) {
            profiler_selftest_spin(spin_ms < MAX_SELFTEST_SPIN_MS ? spin_ms : MAX_SELFTEST_SPIN_MS);
            profiler_control.selftest_spin_ms = 0;
        }
    }
}

static uint64_t measure_tsc_hz()
{
    const uint64_t start_us = tsc_micros();
    const uint64_t start = tsc_read();
    while (tsc_micros() - start_us < 10000)
        asm volatile("pause");
    return (tsc_read() - start) * 100;
}

void profiler_initialize(void)
{
    const int ncpus = cpu_count();

    profiler_control.magic = PROFILER_MAGIC;
    profiler_control.version = PROFILER_VERSION;
    profiler_control.ncpus = ncpus;
    profiler_control.tsc_hz = measure_tsc_hz();
    stale_window_tsc = profiler_control.tsc_hz / STALE_WINDOW_DIVISOR;

    uint64_t slots[MAX_CPUS];
    for (int i = 0; i < ncpus; i++) {
        uint8_t* stack = alloc_kernel_pages(KERNEL_STACK_PAGES);
        if (!stack) {
            terminal_printf("profiler: no memory for the NMI stack of cpu %d\n", i);
            for (int j = 0; j < ncpus; j++)
                profiler_control.cpu[j].status = PROFILER_NO_BUFFER;
            return;
        }
        percpu[i].cpu = cpu_get(i);
        percpu[i].state = &profiler_control.cpu[i];
        const uint64_t slot = (uint64_t)stack + KERNEL_STACK_SIZE - IST_SLOT_OFFSET;
        *(struct profiler_percpu**)slot = &percpu[i];
        slots[i] = slot;
    }

    for (int i = 0; i < ncpus; i++) {
        struct profiler_cpu_state* state = percpu[i].state;
        void* buffer = alloc_physical_pages(BUFFER_SIZE / PAGE_SIZE);
        if (!buffer) {
            terminal_printf("profiler: no memory for the sample buffer of cpu %d\n", i);
            state->status = PROFILER_NO_BUFFER;
            continue;
        }
        percpu[i].buffer = phys_to_virt((uint64_t)buffer);
        state->buffer_phys = (uint64_t)buffer;
        state->buffer_size = BUFFER_SIZE;
    }

    for (int i = 0; i < ncpus; i++)
        cpu_get(i)->tss->ist1 = slots[i];

    // The gate can be live while it's written: only the store of the lower half may change what an NMI sees
    const uint64_t entry = (uintptr_t)profiler_nmi_entry;
    uint64_t* gate = (uint64_t*)&idt[IDT_VECTOR_NMI];
    __atomic_store_n(&gate[1], entry >> 32, __ATOMIC_RELEASE);
    __atomic_store_n(&gate[0],
                     (entry & 0xFFFF) | (uint64_t)IDT_KERNEL_CODE << 16 | (uint64_t)IDT_NMI_IST << 32
                         | (uint64_t)IDT_INTERRUPT_GATE << 40 | (entry >> 16 & 0xFFFF) << 48,
                     __ATOMIC_RELEASE);

    __atomic_store_n(&ready, true, __ATOMIC_RELEASE);
}
