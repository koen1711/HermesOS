#ifndef PROFILER_H
#define PROFILER_H

#include <os/stdint.h>
#include <hardware/smp/smp.h>

#define PROFILER_MAGIC 0x4C5443464F525048ULL // "HPROFCTL"
#define PROFILER_VERSION 1

enum profiler_cpu_status {
    PROFILER_OFF = 0,
    PROFILER_ARMED = 1,
    PROFILER_NO_PMU = 2,
    PROFILER_NO_BUFFER = 3
};

#define PROFILER_MAX_DEPTH 32

#define PROFILER_SAMPLE_USER      (1 << 0)
#define PROFILER_SAMPLE_TRUNCATED (1 << 1)
#define PROFILER_SAMPLE_INTERRUPTS_ON (1 << 2)

struct profiler_sample {
    uint64_t tsc, rip;
    uint16_t depth;
    uint8_t cpu;
    uint8_t flags;
    uint32_t reserved;
    uint64_t rbp;
};

struct profiler_cpu_state {
    uint64_t status, armed_generation;
    uint64_t nmi_count, samples, dropped, truncated, user_samples;
    uint64_t write_offset, buffer_phys, buffer_size;
    uint64_t handler_tsc_total, start_tsc, stop_tsc;
    uint64_t spurious;
};

struct profiler_control {
    uint64_t magic, version;
    volatile uint64_t requested, generation, rate_hz, selftest_spin_ms;
    uint64_t ncpus, tsc_hz;
    struct profiler_cpu_state cpu[MAX_CPUS];
};

extern struct profiler_control profiler_control;

void profiler_initialize(void);
void profiler_tick(void);
void profiler_selftest_spin(uint64_t ms);

#endif //PROFILER_H
