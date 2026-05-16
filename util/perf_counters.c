#include "util/perf_counters.h"

#ifdef PERF_COUNTERS
#include <stdatomic.h>

static _Atomic uint64_t counters[PERF_COUNTER_COUNT];

static const char *const counter_names[PERF_COUNTER_COUNT] = {
    [PERF_TLB_READ_MISS] = "tlb_read_miss",
    [PERF_TLB_WRITE_MISS] = "tlb_write_miss",
    [PERF_TLB_FLUSH] = "tlb_flush",
    [PERF_TLB_CROSSPAGE_READ] = "tlb_crosspage_read",
    [PERF_TLB_CROSSPAGE_WRITE] = "tlb_crosspage_write",
    [PERF_BLOCK_CACHE_HIT] = "block_cache_hit",
    [PERF_BLOCK_CACHE_MISS] = "block_cache_miss",
    [PERF_BLOCK_LOOKUP_HIT] = "block_lookup_hit",
    [PERF_BLOCK_COMPILE] = "block_compile",
    [PERF_BLOCK_CHAIN_ATTEMPT] = "block_chain_attempt",
    [PERF_BLOCK_CHAIN_PATCHED] = "block_chain_patched",
    [PERF_RET_CACHE_HIT] = "ret_cache_hit",
    [PERF_RET_CACHE_MISS] = "ret_cache_miss",
    [PERF_SYSCALL_TOTAL] = "syscall_total",
    [PERF_SYSCALL_FAST] = "syscall_fast",
    [PERF_SYSCALL_SLOW] = "syscall_slow",
    [PERF_JIT_CRASH_RETRY] = "jit_crash_retry",
};

void perf_counter_add(enum perf_counter_id id, uint64_t delta) {
    if ((unsigned) id >= PERF_COUNTER_COUNT)
        return;
    atomic_fetch_add_explicit(&counters[id], delta, memory_order_relaxed);
}

uint64_t perf_counter_get(enum perf_counter_id id) {
    if ((unsigned) id >= PERF_COUNTER_COUNT)
        return 0;
    return atomic_load_explicit(&counters[id], memory_order_relaxed);
}

void perf_counter_reset_all(void) {
    for (unsigned i = 0; i < PERF_COUNTER_COUNT; i++)
        atomic_store_explicit(&counters[i], 0, memory_order_relaxed);
}

const char *perf_counter_name(enum perf_counter_id id) {
    if ((unsigned) id >= PERF_COUNTER_COUNT || counter_names[id] == NULL)
        return "";
    return counter_names[id];
}
#endif
