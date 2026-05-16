#ifndef UTIL_PERF_COUNTERS_H
#define UTIL_PERF_COUNTERS_H

#include <stdint.h>
#include <stddef.h>

enum perf_counter_id {
    PERF_TLB_READ_MISS,
    PERF_TLB_WRITE_MISS,
    PERF_TLB_FLUSH,
    PERF_TLB_CROSSPAGE_READ,
    PERF_TLB_CROSSPAGE_WRITE,
    PERF_BLOCK_CACHE_HIT,
    PERF_BLOCK_CACHE_MISS,
    PERF_BLOCK_LOOKUP_HIT,
    PERF_BLOCK_COMPILE,
    PERF_BLOCK_CHAIN_ATTEMPT,
    PERF_BLOCK_CHAIN_PATCHED,
    PERF_RET_CACHE_HIT,
    PERF_RET_CACHE_MISS,
    PERF_SYSCALL_TOTAL,
    PERF_SYSCALL_FAST,
    PERF_SYSCALL_SLOW,
    PERF_JIT_CRASH_RETRY,
    PERF_COUNTER_COUNT,
};

#ifdef PERF_COUNTERS
void perf_counter_add(enum perf_counter_id id, uint64_t delta);
uint64_t perf_counter_get(enum perf_counter_id id);
void perf_counter_reset_all(void);
const char *perf_counter_name(enum perf_counter_id id);
#else
static inline void perf_counter_add(enum perf_counter_id id, uint64_t delta) {
    (void) id;
    (void) delta;
}
static inline uint64_t perf_counter_get(enum perf_counter_id id) {
    (void) id;
    return 0;
}
static inline void perf_counter_reset_all(void) {}
static inline const char *perf_counter_name(enum perf_counter_id id) {
    (void) id;
    return "";
}
#endif

static inline void perf_counter_inc(enum perf_counter_id id) {
    perf_counter_add(id, 1);
}

#endif
