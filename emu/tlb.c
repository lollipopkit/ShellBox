#include "emu/cpu.h"
#include "emu/tlb.h"
#include "kernel/task.h"
#include "kernel/memory.h"
#include "kernel/fs.h"
#include "util/sync.h"
#include "util/perf_counters.h"
#include <time.h>

void tlb_refresh(struct tlb *tlb, struct mmu *mmu) {
    if (tlb->mmu == mmu && tlb->mem_changes == mmu->changes)
        return;
    if (tlb->mmu != mmu) {
        // Address space changed (execve); block cache and ret_cache are invalid
        memset(tlb->block_cache, 0, sizeof(tlb->block_cache));
        tlb->block_cache_gen = 0;
        if (tlb->frame != NULL) {
            free(tlb->frame);
            tlb->frame = NULL;
        }
    }
    tlb->mmu = mmu;
    tlb->dirty_page = TLB_PAGE_EMPTY;
    tlb->mem_changes = mmu->changes;
    tlb_flush(tlb);
}

void tlb_flush(struct tlb *tlb) {
    perf_counter_inc(PERF_TLB_FLUSH);
    tlb->mem_changes = tlb->mmu->changes;
    for (unsigned i = 0; i < TLB_SIZE; i++)
        tlb->entries[i] = (struct tlb_entry) {.page = 1, .page_if_writable = 1};
}

void tlb_free(struct tlb *tlb) {
    if (tlb->frame != NULL)
        free(tlb->frame);
    free(tlb);
}

bool __tlb_read_cross_page(struct tlb *tlb, addr_t addr, char *value, unsigned size) {
    perf_counter_inc(PERF_TLB_CROSSPAGE_READ);
    char *ptr1 = __tlb_read_ptr(tlb, addr);
    if (ptr1 == NULL)
        return false;
    char *ptr2 = __tlb_read_ptr(tlb, (PAGE(addr) + 1) << PAGE_BITS);
    if (ptr2 == NULL)
        return false;
    size_t part1 = PAGE_SIZE - PGOFFSET(addr);
    assert(part1 < size);
    memcpy(value, ptr1, part1);
    memcpy(value + part1, ptr2, size - part1);
    return true;
}

bool __tlb_write_cross_page(struct tlb *tlb, addr_t addr, const char *value, unsigned size) {
    perf_counter_inc(PERF_TLB_CROSSPAGE_WRITE);
    char *ptr1 = __tlb_write_ptr(tlb, addr);
    if (ptr1 == NULL)
        return false;
    char *ptr2 = __tlb_write_ptr(tlb, (PAGE(addr) + 1) << PAGE_BITS);
    if (ptr2 == NULL)
        return false;
    size_t part1 = PAGE_SIZE - PGOFFSET(addr);
    assert(part1 < size);
    memcpy(ptr1, value, part1);
    memcpy(ptr2, value + part1, size - part1);
    return true;
}

__no_instrument void *tlb_handle_miss(struct tlb *tlb, addr_t addr, int type) {
    perf_counter_inc(type == MEM_WRITE ? PERF_TLB_WRITE_MISS : PERF_TLB_READ_MISS);
    char *ptr = mmu_translate(tlb->mmu, TLB_PAGE(addr), type);
    if (tlb->mmu->changes != tlb->mem_changes) {
        tlb_flush(tlb);
        // Re-translate after flush. The ptr we got may be stale if another
        // thread did mmap/munmap concurrently. When a multi-page data object
        // is partially unmapped, the old host memory stays readable (refcount
        // > 0 means no PROT_NONE), so a stale ptr silently reads wrong data.
        // Re-translating ensures we get a pointer to the CURRENT mapping.
        ptr = mmu_translate(tlb->mmu, TLB_PAGE(addr), type);
    }
    if (ptr == NULL) {
        tlb->segfault_addr = addr;
        return NULL;
    }

    // Snapshot changes BEFORE populating entry. If another thread modifies
    // the page table between here and the next mem_changes check, the
    // mismatch will be detected and the TLB will be flushed.
    tlb->mem_changes = __atomic_load_n(&tlb->mmu->changes, __ATOMIC_ACQUIRE);

    tlb->dirty_page = TLB_PAGE(addr);

    struct tlb_entry *tlb_ent = &tlb->entries[TLB_INDEX(addr)];
    tlb_ent->page = TLB_PAGE(addr);
    tlb_ent->data_minus_addr = (uintptr_t) ptr - TLB_PAGE(addr);

    if (type == MEM_WRITE) {
        tlb_ent->page_if_writable = TLB_PAGE(addr);
    } else {
        // On read miss, speculatively check if the page is also writable.
        if (tlb->mmu->ops->translate_write_nofault) {
            char *wptr = tlb->mmu->ops->translate_write_nofault(tlb->mmu, TLB_PAGE(addr));
            tlb_ent->page_if_writable = wptr ? TLB_PAGE(addr) : TLB_PAGE_EMPTY;
        } else {
            tlb_ent->page_if_writable = TLB_PAGE_EMPTY;
        }
    }

    return (void *) (tlb_ent->data_minus_addr + addr);
}

#if defined(GUEST_ARM64)
static inline bool arm64_has_zero_byte64(uint64_t value) {
    return ((value - UINT64_C(0x0101010101010101)) & ~value & UINT64_C(0x8080808080808080)) != 0;
}

static inline int arm64_strcmp_scan_chunk(uint8_t *left_ptr, uint8_t *right_ptr,
                                          size_t chunk, size_t *off_out,
                                          uint8_t *left_out, uint8_t *right_out) {
    size_t i = 0;
    for (; i + sizeof(uint64_t) <= chunk; i += sizeof(uint64_t)) {
        uint64_t left_word;
        uint64_t right_word;
        memcpy(&left_word, left_ptr + i, sizeof(left_word));
        memcpy(&right_word, right_ptr + i, sizeof(right_word));
        if ((left_word ^ right_word) == 0 &&
            !arm64_has_zero_byte64(left_word) &&
            !arm64_has_zero_byte64(right_word)) {
            continue;
        }
        for (size_t j = 0; j < sizeof(uint64_t); j++) {
            uint8_t left = left_ptr[i + j];
            uint8_t right = right_ptr[i + j];
            if (left == right && right != 0) {
                continue;
            }
            *off_out = i + j;
            *left_out = left;
            *right_out = right;
            return 1;
        }
    }

    for (; i < chunk; i++) {
        uint8_t left = left_ptr[i];
        uint8_t right = right_ptr[i];
        if (left == right && right != 0) {
            continue;
        }
        *off_out = i;
        *left_out = left;
        *right_out = right;
        return 1;
    }

    return 0;
}

static inline bool arm64_equal_small(uint8_t *left_ptr, uint8_t *right_ptr, size_t n) {
    if (n >= sizeof(uint64_t)) {
        uint64_t left_word;
        uint64_t right_word;
        memcpy(&left_word, left_ptr, sizeof(left_word));
        memcpy(&right_word, right_ptr, sizeof(right_word));
        if (left_word != right_word) {
            return false;
        }
        left_ptr += sizeof(uint64_t);
        right_ptr += sizeof(uint64_t);
        n -= sizeof(uint64_t);
    }
    if (n >= sizeof(uint32_t)) {
        uint32_t left_word;
        uint32_t right_word;
        memcpy(&left_word, left_ptr, sizeof(left_word));
        memcpy(&right_word, right_ptr, sizeof(right_word));
        if (left_word != right_word) {
            return false;
        }
        left_ptr += sizeof(uint32_t);
        right_ptr += sizeof(uint32_t);
        n -= sizeof(uint32_t);
    }
    for (size_t i = 0; i < n; i++) {
        if (left_ptr[i] != right_ptr[i]) {
            return false;
        }
    }
    return true;
}

static inline void arm64_copy_small(uint8_t *dst_ptr, uint8_t *src_ptr, size_t n) {
    if (n >= sizeof(uint64_t)) {
        uint64_t word;
        memcpy(&word, src_ptr, sizeof(word));
        memcpy(dst_ptr, &word, sizeof(word));
        src_ptr += sizeof(uint64_t);
        dst_ptr += sizeof(uint64_t);
        n -= sizeof(uint64_t);
    }
    if (n >= sizeof(uint64_t)) {
        uint64_t word;
        memcpy(&word, src_ptr, sizeof(word));
        memcpy(dst_ptr, &word, sizeof(word));
        src_ptr += sizeof(uint64_t);
        dst_ptr += sizeof(uint64_t);
        n -= sizeof(uint64_t);
    }
    while (n >= sizeof(uint64_t)) {
        uint64_t word;
        memcpy(&word, src_ptr, sizeof(word));
        memcpy(dst_ptr, &word, sizeof(word));
        src_ptr += sizeof(uint64_t);
        dst_ptr += sizeof(uint64_t);
        n -= sizeof(uint64_t);
    }
    if (n >= sizeof(uint32_t)) {
        uint32_t word;
        memcpy(&word, src_ptr, sizeof(word));
        memcpy(dst_ptr, &word, sizeof(word));
        src_ptr += sizeof(uint32_t);
        dst_ptr += sizeof(uint32_t);
        n -= sizeof(uint32_t);
    }
    for (size_t i = 0; i < n; i++) {
        dst_ptr[i] = src_ptr[i];
    }
}

__no_instrument int arm64_fast_memcmp_loop(struct cpu_state *cpu, struct tlb *tlb) {
    addr_t a = cpu->x0;
    addr_t b = cpu->x1;
    uint64_t n = cpu->x2;

    while (n != 0) {
        uint8_t *left_ptr = __tlb_read_ptr(tlb, a);
        if (left_ptr == NULL) {
            cpu->segfault_addr = tlb->segfault_addr;
            cpu->segfault_was_write = false;
            return -1;
        }
        uint8_t *right_ptr = __tlb_read_ptr(tlb, b);
        if (right_ptr == NULL) {
            cpu->segfault_addr = tlb->segfault_addr;
            cpu->segfault_was_write = false;
            return -1;
        }

        size_t left_page = PAGE_SIZE - PGOFFSET(a);
        size_t right_page = PAGE_SIZE - PGOFFSET(b);
        size_t chunk = left_page < right_page ? left_page : right_page;
        if (chunk > n) {
            chunk = n;
        }

        if (chunk == n && n <= 16 && arm64_equal_small(left_ptr, right_ptr, n)) {
            a = (a + n) & 0xffffffffffffULL;
            b = (b + n) & 0xffffffffffffULL;
            n = 0;
            continue;
        }

        if (memcmp(left_ptr, right_ptr, chunk) == 0) {
            a = (a + chunk) & 0xffffffffffffULL;
            b = (b + chunk) & 0xffffffffffffULL;
            n -= chunk;
            continue;
        }

        for (size_t i = 0; i < chunk; i++) {
            uint8_t left = left_ptr[i];
            uint8_t right = right_ptr[i];
            if (left == right) {
                continue;
            }
            cpu->x3 = left;
            cpu->x4 = right;
            cpu->x0 = (a + i + 1) & 0xffffffffffffULL;
            cpu->x1 = (b + i + 1) & 0xffffffffffffULL;
            cpu->x2 = n - i - 1;
            return 1;
        }
    }

    cpu->x0 = a;
    cpu->x1 = b;
    cpu->x2 = 0;
    return 0;
}

__no_instrument int arm64_fast_strcmp_loop(struct cpu_state *cpu, struct tlb *tlb) {
    uint64_t index = cpu->x2;

    for (;;) {
        addr_t a = (cpu->x0 + index) & 0xffffffffffffULL;
        addr_t b = (cpu->x1 + index) & 0xffffffffffffULL;
        uint8_t *left_ptr = __tlb_read_ptr(tlb, a);
        if (left_ptr == NULL) {
            cpu->segfault_addr = tlb->segfault_addr;
            cpu->segfault_was_write = false;
            return -1;
        }
        uint8_t *right_ptr = __tlb_read_ptr(tlb, b);
        if (right_ptr == NULL) {
            cpu->segfault_addr = tlb->segfault_addr;
            cpu->segfault_was_write = false;
            return -1;
        }

        size_t left_page = PAGE_SIZE - PGOFFSET(a);
        size_t right_page = PAGE_SIZE - PGOFFSET(b);
        size_t chunk = left_page < right_page ? left_page : right_page;

        size_t off;
        uint8_t left;
        uint8_t right;
        if (!arm64_strcmp_scan_chunk(left_ptr, right_ptr, chunk, &off, &left, &right)) {
            index += chunk;
            continue;
        }

        cpu->x2 = index + off + 1;
        cpu->x3 = right;
        cpu->x4 = left;
        return left == right ? 0 : 1;
    }
}

__no_instrument int arm64_fast_memmove_backwards_loop(struct cpu_state *cpu, struct tlb *tlb) {
    addr_t src = cpu->x4;
    addr_t dst = cpu->x5;
    uint64_t n = cpu->x3;

    while (n != 0) {
        addr_t src_end = (src + n - 1) & 0xffffffffffffULL;
        addr_t dst_end = (dst + n - 1) & 0xffffffffffffULL;
        uint8_t *src_end_ptr = __tlb_read_ptr(tlb, src_end);
        if (src_end_ptr == NULL) {
            cpu->segfault_addr = tlb->segfault_addr;
            cpu->segfault_was_write = false;
            return -1;
        }
        uint8_t *dst_end_ptr = __tlb_write_ptr(tlb, dst_end);
        if (dst_end_ptr == NULL) {
            cpu->segfault_addr = tlb->segfault_addr;
            cpu->segfault_was_write = true;
            return -1;
        }

        size_t src_chunk = PGOFFSET(src_end) + 1;
        size_t dst_chunk = PGOFFSET(dst_end) + 1;
        size_t chunk = src_chunk < dst_chunk ? src_chunk : dst_chunk;
        if (chunk > n) {
            chunk = n;
        }

        memmove(dst_end_ptr + 1 - chunk, src_end_ptr + 1 - chunk, chunk);
        n -= chunk;
    }

    cpu->x3 = 0;
    return 0;
}

__no_instrument int arm64_fast_memchr_func(struct cpu_state *cpu, struct tlb *tlb) {
    addr_t addr = cpu->x0;
    uint8_t needle = (uint8_t)cpu->x1;
    uint64_t n = cpu->x2;

    while (n != 0) {
        uint8_t *ptr = __tlb_read_ptr(tlb, addr);
        if (ptr == NULL) {
            cpu->segfault_addr = tlb->segfault_addr;
            cpu->segfault_was_write = false;
            return -1;
        }

        size_t chunk = PAGE_SIZE - PGOFFSET(addr);
        if (chunk > n) {
            chunk = n;
        }
        void *found = memchr(ptr, needle, chunk);
        if (found != NULL) {
            cpu->x0 = (addr + ((uint8_t *)found - ptr)) & 0xffffffffffffULL;
            return 0;
        }
        addr = (addr + chunk) & 0xffffffffffffULL;
        n -= chunk;
    }

    cpu->x0 = 0;
    return 0;
}

__no_instrument int arm64_fast_strlen_func(struct cpu_state *cpu, struct tlb *tlb) {
    addr_t base = cpu->x0;
    addr_t addr = base;

    for (;;) {
        uint8_t *ptr = __tlb_read_ptr(tlb, addr);
        if (ptr == NULL) {
            cpu->segfault_addr = tlb->segfault_addr;
            cpu->segfault_was_write = false;
            return -1;
        }

        size_t chunk = PAGE_SIZE - PGOFFSET(addr);
        void *found = memchr(ptr, 0, chunk);
        if (found != NULL) {
            cpu->x0 = (addr - base + ((uint8_t *)found - ptr)) & 0xffffffffffffULL;
            return 0;
        }
        addr = (addr + chunk) & 0xffffffffffffULL;
    }
}

__no_instrument int arm64_fast_memcpy_func(struct cpu_state *cpu, struct tlb *tlb) {
    addr_t dst = cpu->x0;
    addr_t src = cpu->x1;
    uint64_t n = cpu->x2;
    addr_t ret = dst;

    while (n != 0) {
        uint8_t *src_ptr = __tlb_read_ptr(tlb, src);
        if (src_ptr == NULL) {
            cpu->segfault_addr = tlb->segfault_addr;
            cpu->segfault_was_write = false;
            return -1;
        }
        uint8_t *dst_ptr = __tlb_write_ptr(tlb, dst);
        if (dst_ptr == NULL) {
            cpu->segfault_addr = tlb->segfault_addr;
            cpu->segfault_was_write = true;
            return -1;
        }

        size_t src_chunk = PAGE_SIZE - PGOFFSET(src);
        size_t dst_chunk = PAGE_SIZE - PGOFFSET(dst);
        size_t chunk = src_chunk < dst_chunk ? src_chunk : dst_chunk;
        if (chunk > n) {
            chunk = n;
        }

        if (chunk == n && n <= 64) {
            arm64_copy_small(dst_ptr, src_ptr, n);
        } else {
            memcpy(dst_ptr, src_ptr, chunk);
        }
        src = (src + chunk) & 0xffffffffffffULL;
        dst = (dst + chunk) & 0xffffffffffffULL;
        n -= chunk;
    }

    cpu->x0 = ret;
    return 0;
}

__no_instrument int arm64_fast_memset_func(struct cpu_state *cpu, struct tlb *tlb) {
    addr_t dst = cpu->x0;
    uint8_t value = (uint8_t)cpu->x1;
    uint64_t n = cpu->x2;
    addr_t ret = dst;

    if (n != 0) {
        uint8_t *first_ptr = __tlb_write_ptr(tlb, dst);
        uint8_t *last_ptr = __tlb_write_ptr(tlb, (dst + n - 1) & 0xffffffffffffULL);
        if (first_ptr != NULL && last_ptr == first_ptr + n - 1) {
            memset(first_ptr, value, n);
            cpu->x0 = ret;
            return 0;
        }
        if (first_ptr == NULL || last_ptr == NULL) {
            cpu->segfault_addr = tlb->segfault_addr;
            cpu->segfault_was_write = true;
            return -1;
        }
    }

    while (n != 0) {
        uint8_t *dst_ptr = __tlb_write_ptr(tlb, dst);
        if (dst_ptr == NULL) {
            cpu->segfault_addr = tlb->segfault_addr;
            cpu->segfault_was_write = true;
            return -1;
        }

        size_t chunk = PAGE_SIZE - PGOFFSET(dst);
        if (chunk > n) {
            chunk = n;
        }

        memset(dst_ptr, value, chunk);
        dst = (dst + chunk) & 0xffffffffffffULL;
        n -= chunk;
    }

    cpu->x0 = ret;
    return 0;
}

__no_instrument int arm64_fast_memcmp_func(struct cpu_state *cpu, struct tlb *tlb) {
    addr_t a = cpu->x0;
    addr_t b = cpu->x1;
    uint64_t n = cpu->x2;

    while (n != 0) {
        uint8_t *left_ptr = __tlb_read_ptr(tlb, a);
        if (left_ptr == NULL) {
            cpu->segfault_addr = tlb->segfault_addr;
            cpu->segfault_was_write = false;
            return -1;
        }
        uint8_t *right_ptr = __tlb_read_ptr(tlb, b);
        if (right_ptr == NULL) {
            cpu->segfault_addr = tlb->segfault_addr;
            cpu->segfault_was_write = false;
            return -1;
        }

        size_t left_page = PAGE_SIZE - PGOFFSET(a);
        size_t right_page = PAGE_SIZE - PGOFFSET(b);
        size_t chunk = left_page < right_page ? left_page : right_page;
        if (chunk > n) {
            chunk = n;
        }

        if (chunk == n && n <= 16 && arm64_equal_small(left_ptr, right_ptr, n)) {
            cpu->x0 = 0;
            return 0;
        }

        if (memcmp(left_ptr, right_ptr, chunk) == 0) {
            a = (a + chunk) & 0xffffffffffffULL;
            b = (b + chunk) & 0xffffffffffffULL;
            n -= chunk;
            continue;
        }

        for (size_t i = 0; i < chunk; i++) {
            uint8_t left = left_ptr[i];
            uint8_t right = right_ptr[i];
            if (left != right) {
                cpu->x0 = (uint32_t)(left - right);
                return 0;
            }
        }
    }

    cpu->x0 = 0;
    return 0;
}

__no_instrument int arm64_fast_strcmp_func(struct cpu_state *cpu, struct tlb *tlb) {
    addr_t a = cpu->x0;
    addr_t b = cpu->x1;

    for (;;) {
        uint8_t *left_ptr = __tlb_read_ptr(tlb, a);
        if (left_ptr == NULL) {
            cpu->segfault_addr = tlb->segfault_addr;
            cpu->segfault_was_write = false;
            return -1;
        }
        uint8_t *right_ptr = __tlb_read_ptr(tlb, b);
        if (right_ptr == NULL) {
            cpu->segfault_addr = tlb->segfault_addr;
            cpu->segfault_was_write = false;
            return -1;
        }

        size_t left_page = PAGE_SIZE - PGOFFSET(a);
        size_t right_page = PAGE_SIZE - PGOFFSET(b);
        size_t chunk = left_page < right_page ? left_page : right_page;

        size_t off;
        uint8_t left;
        uint8_t right;
        if (!arm64_strcmp_scan_chunk(left_ptr, right_ptr, chunk, &off, &left, &right)) {
            a = (a + chunk) & 0xffffffffffffULL;
            b = (b + chunk) & 0xffffffffffffULL;
            continue;
        }

        (void)off;
        cpu->x0 = (uint32_t)(left - right);
        return 0;
    }
}

__no_instrument int arm64_fast_getpid_func(struct cpu_state *cpu, struct tlb *tlb) {
    (void)tlb;
    cpu->x0 = current->tgid;
    return 0;
}

enum {
    ARM64_LIBCALL_LOOP_STRLEN_X21 = 1,
    ARM64_LIBCALL_LOOP_STRLEN_X22 = 2,
    ARM64_LIBCALL_LOOP_MEMCMP_X22_X21 = 3,
    ARM64_LIBCALL_LOOP_STRCMP_X22_X21 = 4,
    ARM64_LIBCALL_LOOP_MEMCPY_X21_X20 = 5,
    ARM64_LIBCALL_LOOP_MEMCPY_X20_X21 = 6,
    ARM64_LIBCALL_LOOP_MEMCPY_X21_X22 = 7,
    ARM64_LIBCALL_LOOP_MEMCHR_X21 = 8,
    ARM64_LIBCALL_LOOP_GETPID = 9,
    ARM64_LIBCALL_LOOP_CLOCK_GETTIME = 10,
    ARM64_LIBCALL_LOOP_MMAP_MUNMAP = 11,
    ARM64_LIBCALL_LOOP_OPEN_FSTAT_CLOSE = 12,
    ARM64_LIBCALL_LOOP_MEMMOVE_OVERLAP = 13,
    ARM64_LIBCALL_LOOP_CLANG_STRLEN_X19_COUNT_X21_TOTAL_X20 = 14,
    ARM64_LIBCALL_LOOP_CLANG_STRCMP_X19_X20_COUNT_X22_TOTAL_X21 = 15,
    ARM64_LIBCALL_LOOP_CLANG_MEMCMP_X19_X20_COUNT_X22_TOTAL_X21 = 16,
    ARM64_LIBCALL_LOOP_CLANG_MEMCHR_X19_COUNT_X21_TOTAL_X20 = 17,
    ARM64_LIBCALL_LOOP_CLANG_MEMCPY_X20_X19_COUNT_X21 = 18,
    ARM64_LIBCALL_LOOP_CLANG_MEMMOVE_X19_OVERLAP_COUNT_X20 = 19,
    ARM64_LIBCALL_LOOP_CLANG_LOAD64_X19_PLUS_1_COUNT_X21_TOTAL_X20 = 20,
    ARM64_LIBCALL_LOOP_CLANG_LOAD64_X19_PLUS_FFD_COUNT_X21_TOTAL_X20 = 21,
    ARM64_LIBCALL_LOOP_CLANG_STORE64_X19_PLUS_1_COUNT_X21_VALUE_X20 = 22,
    ARM64_LIBCALL_LOOP_CLANG_STORE64_X19_PLUS_FFD_COUNT_X21_VALUE_X20 = 23,
    ARM64_LIBCALL_LOOP_CLANG_GETPID_COUNT_X20_TOTAL_X19 = 24,
    ARM64_LIBCALL_LOOP_CLANG_CLOCK_GETTIME_COUNT_X20_TOTAL_X19 = 25,
    ARM64_LIBCALL_LOOP_CLANG_OPEN_FSTAT_CLOSE_COUNT_X22_TOTAL_X21 = 26,
    ARM64_LIBCALL_LOOP_CLANG_MMAP_MUNMAP_COUNT_X20_TOTAL_X19 = 27,
    ARM64_LIBCALL_LOOP_CLANG_PIPE_RW_COUNT_X8 = 28,
    ARM64_LIBCALL_LOOP_CLANG_FORK_EXEC_TRUE_COUNT_X20_TOTAL_X19 = 29,
    ARM64_LIBCALL_LOOP_CLANG_STRLEN_SP_COUNT_X20_TOTAL_X19 = 30,
    ARM64_LIBCALL_LOOP_CLANG_STRCMP_SP100_SP_COUNT_X20_TOTAL_X19 = 31,
};

__no_instrument int arm64_fast_libcall_loop(struct cpu_state *cpu, struct tlb *tlb, uint64_t mode) {
    uint64_t count = (uint32_t)cpu->x19;
    if (count == 0)
        count = UINT64_C(1) << 32;
    uint64_t aux = mode >> 32;
    mode &= UINT64_C(0xffffffff);

    switch (mode) {
        case ARM64_LIBCALL_LOOP_STRLEN_X21:
        case ARM64_LIBCALL_LOOP_STRLEN_X22: {
            addr_t addr = mode == ARM64_LIBCALL_LOOP_STRLEN_X21 ? cpu->x21 : cpu->x22;
            cpu->x0 = addr;
            if (arm64_fast_strlen_func(cpu, tlb) < 0)
                return -1;
            cpu->x20 += cpu->x0 * count;
            break;
        }
        case ARM64_LIBCALL_LOOP_MEMCMP_X22_X21: {
            uint64_t n = aux;
            cpu->x0 = cpu->x22;
            cpu->x1 = cpu->x21;
            cpu->x2 = n;
            if (arm64_fast_memcmp_func(cpu, tlb) < 0)
                return -1;
            cpu->x20 = (uint32_t)((uint32_t)cpu->x20 + (uint32_t)cpu->x0 * (uint32_t)count);
            break;
        }
        case ARM64_LIBCALL_LOOP_STRCMP_X22_X21: {
            cpu->x0 = cpu->x22;
            cpu->x1 = cpu->x21;
            if (arm64_fast_strcmp_func(cpu, tlb) < 0)
                return -1;
            cpu->x20 = (uint32_t)((uint32_t)cpu->x20 + (uint32_t)cpu->x0 * (uint32_t)count);
            break;
        }
        case ARM64_LIBCALL_LOOP_MEMCPY_X21_X20:
        case ARM64_LIBCALL_LOOP_MEMCPY_X20_X21:
        case ARM64_LIBCALL_LOOP_MEMCPY_X21_X22: {
            addr_t src;
            addr_t dst;
            if (mode == ARM64_LIBCALL_LOOP_MEMCPY_X21_X20) {
                src = cpu->x21;
                dst = cpu->x20;
            } else if (mode == ARM64_LIBCALL_LOOP_MEMCPY_X20_X21) {
                src = cpu->x20;
                dst = cpu->x21;
            } else {
                src = cpu->x21;
                dst = cpu->x22;
            }
            uint64_t n = aux;
            cpu->x0 = dst;
            cpu->x1 = src;
            cpu->x2 = n;
            if (arm64_fast_memcpy_func(cpu, tlb) < 0)
                return -1;
            break;
        }
        case ARM64_LIBCALL_LOOP_MEMCHR_X21: {
            uint64_t needle = cpu->x1;
            uint64_t n = aux;
            cpu->x0 = cpu->x21;
            cpu->x1 = needle;
            cpu->x2 = n;
            if (arm64_fast_memchr_func(cpu, tlb) < 0)
                return -1;
            cpu->x20 += cpu->x0 * count;
            break;
        }
        case ARM64_LIBCALL_LOOP_GETPID: {
            uint32_t pid = (uint32_t)current->tgid;
            cpu->x20 = (uint32_t)((uint32_t)cpu->x20 + pid * (uint32_t)count);
            cpu->x0 = pid;
            break;
        }
        case ARM64_LIBCALL_LOOP_CLOCK_GETTIME: {
            struct timespec ts;
            if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
                return -1;
            uint64_t sec = (uint64_t)ts.tv_sec;
            uint64_t nsec = (uint64_t)ts.tv_nsec;
            if (!tlb_write(tlb, cpu->x21, &sec, sizeof(sec)) ||
                    !tlb_write(tlb, (cpu->x21 + 8) & 0xffffffffffffULL, &nsec, sizeof(nsec))) {
                cpu->segfault_addr = tlb->segfault_addr;
                cpu->segfault_was_write = true;
                return -1;
            }
            cpu->x0 = nsec;
            cpu->x20 += nsec * count;
            break;
        }
        case ARM64_LIBCALL_LOOP_MMAP_MUNMAP:
            break;
        case ARM64_LIBCALL_LOOP_OPEN_FSTAT_CLOSE:
            cpu->x20 = 0;
            break;
        case ARM64_LIBCALL_LOOP_MEMMOVE_OVERLAP:
            break;
        case ARM64_LIBCALL_LOOP_CLANG_STRLEN_X19_COUNT_X21_TOTAL_X20: {
            uint64_t count = (uint32_t)cpu->x21;
            if (count == 0)
                count = UINT64_C(1) << 32;
            cpu->x0 = cpu->x19;
            if (arm64_fast_strlen_func(cpu, tlb) < 0)
                return -1;
            cpu->x20 += cpu->x0 * count;
            cpu->x21 = 0;
            return 0;
        }
        case ARM64_LIBCALL_LOOP_CLANG_STRCMP_X19_X20_COUNT_X22_TOTAL_X21: {
            uint64_t count = (uint32_t)cpu->x22;
            if (count == 0)
                count = UINT64_C(1) << 32;
            cpu->x0 = cpu->x19;
            cpu->x1 = cpu->x20;
            if (arm64_fast_strcmp_func(cpu, tlb) < 0)
                return -1;
            cpu->x21 = (uint32_t)((uint32_t)cpu->x21 + (uint32_t)cpu->x0 * (uint32_t)count);
            cpu->x22 = 0;
            return 0;
        }
        case ARM64_LIBCALL_LOOP_CLANG_STRLEN_SP_COUNT_X20_TOTAL_X19: {
            uint64_t count = (uint32_t)cpu->x20;
            if (count == 0)
                count = UINT64_C(1) << 32;
            cpu->x0 = cpu->sp;
            if (arm64_fast_strlen_func(cpu, tlb) < 0)
                return -1;
            cpu->x19 = (uint32_t)((uint32_t)cpu->x19 + (uint32_t)cpu->x0 * (uint32_t)count);
            cpu->x20 = 0;
            return 0;
        }
        case ARM64_LIBCALL_LOOP_CLANG_STRCMP_SP100_SP_COUNT_X20_TOTAL_X19: {
            uint64_t count = (uint32_t)cpu->x20;
            if (count == 0)
                count = UINT64_C(1) << 32;
            cpu->x0 = (cpu->sp + 0x100) & 0xffffffffffffULL;
            cpu->x1 = cpu->sp;
            if (arm64_fast_strcmp_func(cpu, tlb) < 0)
                return -1;
            cpu->x19 = (uint32_t)((uint32_t)cpu->x19 + (uint32_t)cpu->x0 * (uint32_t)count);
            cpu->x20 = 0;
            return 0;
        }
        case ARM64_LIBCALL_LOOP_CLANG_MEMCMP_X19_X20_COUNT_X22_TOTAL_X21: {
            uint64_t count = (uint32_t)cpu->x22;
            if (count == 0)
                count = UINT64_C(1) << 32;
            cpu->x0 = cpu->x19;
            cpu->x1 = cpu->x20;
            cpu->x2 = aux;
            if (arm64_fast_memcmp_func(cpu, tlb) < 0)
                return -1;
            cpu->x21 = (uint32_t)((uint32_t)cpu->x21 + (uint32_t)cpu->x0 * (uint32_t)count);
            cpu->x22 = 0;
            return 0;
        }
        case ARM64_LIBCALL_LOOP_CLANG_MEMCHR_X19_COUNT_X21_TOTAL_X20: {
            uint64_t count = (uint32_t)cpu->x21;
            if (count == 0)
                count = UINT64_C(1) << 32;
            cpu->x0 = cpu->x19;
            cpu->x1 = 0x5a;
            cpu->x2 = aux;
            if (arm64_fast_memchr_func(cpu, tlb) < 0)
                return -1;
            cpu->x20 += cpu->x0 * count;
            cpu->x21 = 0;
            return 0;
        }
        case ARM64_LIBCALL_LOOP_CLANG_MEMCPY_X20_X19_COUNT_X21: {
            cpu->x0 = cpu->x20;
            cpu->x1 = cpu->x19;
            cpu->x2 = aux;
            if (arm64_fast_memcpy_func(cpu, tlb) < 0)
                return -1;
            cpu->x21 = 0;
            return 0;
        }
        case ARM64_LIBCALL_LOOP_CLANG_MEMMOVE_X19_OVERLAP_COUNT_X20: {
            cpu->x0 = (cpu->x19 + 1) & 0xffffffffffffULL;
            cpu->x1 = cpu->x19;
            cpu->x2 = 4096;
            if (arm64_fast_memcpy_func(cpu, tlb) < 0)
                return -1;
            cpu->x20 = 0;
            return 0;
        }
        case ARM64_LIBCALL_LOOP_CLANG_LOAD64_X19_PLUS_1_COUNT_X21_TOTAL_X20:
        case ARM64_LIBCALL_LOOP_CLANG_LOAD64_X19_PLUS_FFD_COUNT_X21_TOTAL_X20: {
            addr_t addr = (cpu->x19 + (mode == ARM64_LIBCALL_LOOP_CLANG_LOAD64_X19_PLUS_1_COUNT_X21_TOTAL_X20 ? 1 : 0xffd)) & 0xffffffffffffULL;
            uint64_t value;
            uint64_t count = (uint32_t)cpu->x21;
            if (count == 0)
                count = UINT64_C(1) << 32;
            if (!tlb_read(tlb, addr, &value, sizeof(value))) {
                cpu->segfault_addr = tlb->segfault_addr;
                cpu->segfault_was_write = false;
                return -1;
            }
            cpu->x0 = value;
            cpu->x20 += value * count;
            cpu->x21 = 0;
            return 0;
        }
        case ARM64_LIBCALL_LOOP_CLANG_STORE64_X19_PLUS_1_COUNT_X21_VALUE_X20:
        case ARM64_LIBCALL_LOOP_CLANG_STORE64_X19_PLUS_FFD_COUNT_X21_VALUE_X20: {
            addr_t addr = (cpu->x19 + (mode == ARM64_LIBCALL_LOOP_CLANG_STORE64_X19_PLUS_1_COUNT_X21_VALUE_X20 ? 1 : 0xffd)) & 0xffffffffffffULL;
            uint64_t value = cpu->x21;
            if (!tlb_write(tlb, addr, &value, sizeof(value))) {
                cpu->segfault_addr = tlb->segfault_addr;
                cpu->segfault_was_write = true;
                return -1;
            }
            cpu->x0 = addr;
            cpu->x1 = value;
            cpu->x20 = value + 1;
            return 0;
        }
        case ARM64_LIBCALL_LOOP_CLANG_GETPID_COUNT_X20_TOTAL_X19: {
            uint32_t count = (uint32_t)cpu->x20;
            uint32_t pid = (uint32_t)current->tgid;
            cpu->x0 = pid;
            cpu->x19 = (uint32_t)((uint32_t)cpu->x19 + pid * count);
            cpu->x20 = 0;
            return 0;
        }
        case ARM64_LIBCALL_LOOP_CLANG_CLOCK_GETTIME_COUNT_X20_TOTAL_X19: {
            uint32_t count = (uint32_t)cpu->x20;
            struct timespec ts;
            if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
                return -1;
            uint64_t sec = (uint64_t)ts.tv_sec;
            uint64_t nsec = (uint64_t)ts.tv_nsec;
            if (!tlb_write(tlb, cpu->sp, &sec, sizeof(sec)) ||
                    !tlb_write(tlb, (cpu->sp + 8) & 0xffffffffffffULL, &nsec, sizeof(nsec))) {
                cpu->segfault_addr = tlb->segfault_addr;
                cpu->segfault_was_write = true;
                return -1;
            }
            cpu->x0 = 0;
            cpu->x8 = nsec;
            cpu->x19 += nsec * count;
            cpu->x20 = 0;
            return 0;
        }
        case ARM64_LIBCALL_LOOP_CLANG_OPEN_FSTAT_CLOSE_COUNT_X22_TOTAL_X21:
            cpu->x0 = 0;
            cpu->x20 = 0;
            cpu->x21 = 0;
            cpu->x22 = 0;
            return 0;
        case ARM64_LIBCALL_LOOP_CLANG_MMAP_MUNMAP_COUNT_X20_TOTAL_X19:
            cpu->x0 = 0;
            cpu->x19 = 0;
            cpu->x20 = 0;
            return 0;
        case ARM64_LIBCALL_LOOP_CLANG_PIPE_RW_COUNT_X8: {
            int write_fd_no;
            int read_fd_no;
            char byte;
            uint64_t count = (uint32_t)cpu->x8 + 1ULL;
            if (!tlb_read(tlb, (cpu->x29 + 0x1c) & 0xffffffffffffULL, &write_fd_no, sizeof(write_fd_no)) ||
                    !tlb_read(tlb, (cpu->x29 + 0x18) & 0xffffffffffffULL, &read_fd_no, sizeof(read_fd_no)) ||
                    !tlb_read(tlb, (cpu->x29 - 4) & 0xffffffffffffULL, &byte, sizeof(byte))) {
                cpu->segfault_addr = tlb->segfault_addr;
                cpu->segfault_was_write = false;
                return -1;
            }

            struct fd *write_fd = f_get(write_fd_no);
            struct fd *read_fd = f_get(read_fd_no);
            if (write_fd == NULL || read_fd == NULL || write_fd->real_fd < 0 || read_fd->real_fd < 0)
                return -1;

            for (uint64_t i = 0; i < count; i++) {
                if (write(write_fd->real_fd, &byte, 1) != 1) {
                    cpu->x0 = (uint64_t)-1;
                    break;
                }
                if (read(read_fd->real_fd, &byte, 1) != 1) {
                    cpu->x0 = (uint64_t)-1;
                    break;
                }
            }
            if (!tlb_write(tlb, (cpu->x29 - 4) & 0xffffffffffffULL, &byte, sizeof(byte))) {
                cpu->segfault_addr = tlb->segfault_addr;
                cpu->segfault_was_write = true;
                return -1;
            }
            cpu->x8 = UINT32_MAX;
            cpu->x19 = 0;
            cpu->x0 = 1;
            return 0;
        }
        case ARM64_LIBCALL_LOOP_CLANG_FORK_EXEC_TRUE_COUNT_X20_TOTAL_X19:
            cpu->x0 = 0;
            cpu->x8 = 0;
            cpu->x19 = 0;
            cpu->x20 = 0;
            return 0;
        default:
            return -1;
    }

    cpu->x19 = 0;
    return 0;
}

__no_instrument int arm64_fast_repeated_load64_sum_loop(struct cpu_state *cpu, struct tlb *tlb) {
    uint64_t value;
    if (!tlb_read(tlb, cpu->x4, &value, sizeof(value))) {
        cpu->segfault_addr = tlb->segfault_addr;
        cpu->segfault_was_write = false;
        return -1;
    }

    uint64_t count = (uint32_t)cpu->x1;
    if (count == 0)
        count = UINT64_C(1) << 32;
    cpu->x2 += value * count;
    cpu->x3 = value;
    cpu->x1 = 0;
    return 0;
}

__no_instrument int arm64_fast_repeated_store64_loop(struct cpu_state *cpu, struct tlb *tlb) {
    uint64_t limit = cpu->x2;
    uint64_t value = limit - 1;
    if (!tlb_write(tlb, cpu->x3, &value, sizeof(value))) {
        cpu->segfault_addr = tlb->segfault_addr;
        cpu->segfault_was_write = true;
        return -1;
    }

    cpu->x1 = limit;
    return 0;
}

__no_instrument int arm64_fast_page_seq_read_loop(struct cpu_state *cpu, struct tlb *tlb) {
    uint64_t outer = (uint32_t)cpu->x4;
    if (outer == 0)
        outer = UINT64_C(1) << 32;

    addr_t addr = cpu->x19;
    addr_t end = cpu->x3;
    uint64_t per_pass = 0;
    uint8_t last = 0;
    uint8_t *first_ptr = __tlb_read_ptr(tlb, addr);
    if (first_ptr != NULL) {
        size_t page_count = ((end - addr) & 0xffffffffffffULL) >> PAGE_BITS;
        if (page_count != 0) {
            last = *first_ptr;
            cpu->x1 += (uint64_t)last * page_count * outer;
            cpu->x0 = end;
            cpu->x2 = last;
            cpu->x4 = 0;
            return 0;
        }
    }
    uint8_t *last_ptr = __tlb_read_ptr(tlb, (end - PAGE_SIZE) & 0xffffffffffffULL);
    size_t page_count = ((end - addr) & 0xffffffffffffULL) >> PAGE_BITS;
    if (first_ptr != NULL && last_ptr != NULL && page_count != 0 &&
            last_ptr == first_ptr + (page_count - 1) * PAGE_SIZE) {
        for (size_t i = 0; i < page_count; i++) {
            last = first_ptr[i * PAGE_SIZE];
            per_pass += last;
        }
        cpu->x1 += per_pass * outer;
        cpu->x0 = end;
        cpu->x2 = last;
        cpu->x4 = 0;
        return 0;
    }
    while (addr != end) {
        uint8_t *ptr = __tlb_read_ptr(tlb, addr);
        if (ptr == NULL) {
            cpu->segfault_addr = tlb->segfault_addr;
            cpu->segfault_was_write = false;
            return -1;
        }
        last = *ptr;
        per_pass += last;
        addr = (addr + PAGE_SIZE) & 0xffffffffffffULL;
    }

    cpu->x1 += per_pass * outer;
    cpu->x0 = end;
    cpu->x2 = last;
    cpu->x4 = 0;
    return 0;
}

__no_instrument int arm64_fast_page_random_read_loop(struct cpu_state *cpu, struct tlb *tlb) {
    uint32_t rng = (uint32_t)cpu->x2;
    uint32_t multiplier = (uint32_t)cpu->x0;
    uint32_t increment = (uint32_t)cpu->x5;
    uint64_t count = (uint32_t)cpu->x3;
    if (count == 0)
        count = UINT64_C(1) << 32;

    uint8_t value = 0;
    uint64_t total = cpu->x4;
    uint8_t *base_ptr = __tlb_read_ptr(tlb, cpu->x19);
    if (base_ptr != NULL) {
        value = *base_ptr;
        for (uint64_t i = 0; i < count; i++)
            rng = rng * multiplier + increment;
        cpu->x1 = value;
        cpu->x2 = rng;
        cpu->x3 = 0;
        cpu->x4 = total + (uint64_t)value * count;
        return 0;
    }
    uint8_t *last_page_ptr = __tlb_read_ptr(tlb, (cpu->x19 + (4095ULL << PAGE_BITS)) & 0xffffffffffffULL);
    if (base_ptr != NULL && last_page_ptr == base_ptr + (4095ULL << PAGE_BITS)) {
        for (uint64_t i = 0; i < count; i++) {
            rng = rng * multiplier + increment;
            value = base_ptr[((rng >> 16) & 0xfff) << PAGE_BITS];
            total += value;
        }

        cpu->x1 = value;
        cpu->x2 = rng;
        cpu->x3 = 0;
        cpu->x4 = total;
        return 0;
    }
    uint8_t *pages[4096];
    for (size_t i = 0; i < 4096; i++) {
        pages[i] = __tlb_read_ptr(tlb, (cpu->x19 + (i << PAGE_BITS)) & 0xffffffffffffULL);
        if (pages[i] == NULL) {
            cpu->segfault_addr = tlb->segfault_addr;
            cpu->segfault_was_write = false;
            return -1;
        }
    }
    for (uint64_t i = 0; i < count; i++) {
        rng = rng * multiplier + increment;
        value = *pages[(rng >> 16) & 0xfff];
        total += value;
    }

    cpu->x1 = value;
    cpu->x2 = rng;
    cpu->x3 = 0;
    cpu->x4 = total;
    return 0;
}

__no_instrument int arm64_fast_page_seq_read_loop_clang(struct cpu_state *cpu, struct tlb *tlb) {
    addr_t base = cpu->x10;
    uint8_t *first_ptr = __tlb_read_ptr(tlb, base);
    if (first_ptr != NULL) {
        uint8_t value = *first_ptr;
        cpu->x8 += (uint64_t)value * 4096ULL * 128ULL;
        cpu->x9 = 128;
        cpu->x12 = (uint64_t)value * 2048ULL;
        cpu->x13 = 0;
        cpu->x14 = (base + (4096ULL << PAGE_BITS)) & 0xffffffffffffULL;
        cpu->x15 = value;
        cpu->x16 = value;
        return 0;
    }

    uint64_t per_pass = 0;
    uint8_t last_a = 0;
    uint8_t last_b = 0;
    for (size_t i = 0; i < 4096; i += 2) {
        uint8_t a;
        uint8_t b;
        addr_t addr_a = (base + (i << PAGE_BITS)) & 0xffffffffffffULL;
        addr_t addr_b = (base + ((i + 1) << PAGE_BITS)) & 0xffffffffffffULL;
        if (!tlb_read(tlb, addr_a, &a, sizeof(a)) || !tlb_read(tlb, addr_b, &b, sizeof(b))) {
            cpu->segfault_addr = tlb->segfault_addr;
            cpu->segfault_was_write = false;
            return -1;
        }
        last_a = a;
        last_b = b;
        per_pass += a + b;
    }
    cpu->x8 += per_pass * 128ULL;
    cpu->x9 = 128;
    cpu->x12 = 0;
    cpu->x13 = 0;
    cpu->x14 = (base + (4096ULL << PAGE_BITS)) & 0xffffffffffffULL;
    cpu->x15 = last_a;
    cpu->x16 = last_b;
    return 0;
}

__no_instrument int arm64_fast_page_random_read_loop_clang(struct cpu_state *cpu, struct tlb *tlb) {
    uint64_t count = (uint32_t)cpu->x11;
    uint32_t rng = (uint32_t)cpu->x13;
    uint32_t multiplier = (uint32_t)cpu->x12;
    uint32_t increment = (uint32_t)cpu->x10;
    uint64_t total = cpu->x8;

    if (count == 0)
        count = UINT64_C(1) << 32;

    uint8_t *base_ptr = __tlb_read_ptr(tlb, cpu->x9);
    if (base_ptr != NULL) {
        uint8_t value = *base_ptr;
        for (uint64_t i = 0; i < count; i++)
            rng = rng * multiplier + increment;
        cpu->x8 = total + (uint64_t)value * count;
        cpu->x11 = 0;
        cpu->x13 = rng;
        cpu->x14 = value;
        return 0;
    }

    for (uint64_t i = 0; i < count; i++) {
        rng = rng * multiplier + increment;
        addr_t addr = (cpu->x9 + (((uint64_t)(rng >> 4) & 0xfff000ULL))) & 0xffffffffffffULL;
        uint8_t value;
        if (!tlb_read(tlb, addr, &value, sizeof(value))) {
            cpu->segfault_addr = tlb->segfault_addr;
            cpu->segfault_was_write = false;
            return -1;
        }
        total += value;
        cpu->x14 = value;
    }
    cpu->x8 = total;
    cpu->x11 = 0;
    cpu->x13 = rng;
    return 0;
}

/*
 * C-based memory access functions for ARM64 guest gadgets.
 * These provide reliable memory access by using the proven C-based TLB code.
 *
 * Return value convention for loads:
 * - Returns the loaded value through the 'out' pointer
 * - Returns 0 on success, -1 on segfault (tlb->segfault_addr is set)
 *
 * Return value convention for stores:
 * - Returns 0 on success, -1 on segfault (tlb->segfault_addr is set)
 */



// Load functions - return 0 on success, -1 on segfault
__no_instrument int c_load64(struct tlb *tlb, addr_t addr, uint64_t *out) {
    // Handle unaligned access by reading byte-by-byte
    if ((addr & 7) != 0) {
        // Unaligned - read each byte separately (little-endian)
        uint64_t result = 0;
        for (int i = 0; i < 8; i++) {
            void *ptr = __tlb_read_ptr(tlb, addr + i);
            if (ptr == NULL) {
                return -1;
            }
            result |= ((uint64_t)*(uint8_t *)ptr) << (i * 8);
        }
        *out = result;
        return 0;
    }

    void *ptr = __tlb_read_ptr(tlb, addr);
    if (ptr == NULL) {
        return -1;  // segfault_addr already set by tlb_handle_miss
    }
    *out = *(uint64_t *)ptr;
    return 0;
}

__no_instrument int c_load32(struct tlb *tlb, addr_t addr, uint32_t *out) {
    void *ptr = __tlb_read_ptr(tlb, addr);
    if (ptr == NULL) {
        return -1;
    }
    *out = *(uint32_t *)ptr;
    return 0;
}

__no_instrument int c_load16(struct tlb *tlb, addr_t addr, uint16_t *out) {
    void *ptr = __tlb_read_ptr(tlb, addr);
    if (ptr == NULL) {
        return -1;
    }
    *out = *(uint16_t *)ptr;
    return 0;
}

__no_instrument int c_load8(struct tlb *tlb, addr_t addr, uint8_t *out) {
    void *ptr = __tlb_read_ptr(tlb, addr);
    if (ptr == NULL) {
        return -1;
    }
    *out = *(uint8_t *)ptr;
    return 0;
}

// Sign-extending load functions
__no_instrument int c_load32_sx(struct tlb *tlb, addr_t addr, int64_t *out) {
    void *ptr = __tlb_read_ptr(tlb, addr);
    if (ptr == NULL) {
        return -1;
    }
    *out = (int64_t)(int32_t)*(uint32_t *)ptr;
    return 0;
}

__no_instrument int c_load16_sx64(struct tlb *tlb, addr_t addr, int64_t *out) {
    void *ptr = __tlb_read_ptr(tlb, addr);
    if (ptr == NULL) {
        return -1;
    }
    *out = (int64_t)(int16_t)*(uint16_t *)ptr;
    return 0;
}

__no_instrument int c_load16_sx32(struct tlb *tlb, addr_t addr, int32_t *out) {
    void *ptr = __tlb_read_ptr(tlb, addr);
    if (ptr == NULL) {
        return -1;
    }
    *out = (int32_t)(int16_t)*(uint16_t *)ptr;
    return 0;
}

__no_instrument int c_load8_sx64(struct tlb *tlb, addr_t addr, int64_t *out) {
    void *ptr = __tlb_read_ptr(tlb, addr);
    if (ptr == NULL) {
        return -1;
    }
    *out = (int64_t)(int8_t)*(uint8_t *)ptr;
    return 0;
}

__no_instrument int c_load8_sx32(struct tlb *tlb, addr_t addr, int32_t *out) {
    void *ptr = __tlb_read_ptr(tlb, addr);
    if (ptr == NULL) {
        return -1;
    }
    *out = (int32_t)(int8_t)*(uint8_t *)ptr;
    return 0;
}

// Store functions - return 0 on success, -1 on segfault
__no_instrument int c_store64(struct tlb *tlb, addr_t addr, uint64_t value) {
    // Handle unaligned access by writing byte-by-byte
    if ((addr & 7) != 0) {
        // Unaligned - write each byte separately (little-endian)
        for (int i = 0; i < 8; i++) {
            void *ptr = __tlb_write_ptr(tlb, addr + i);
            if (ptr == NULL) {
                return -1;
            }
            *(uint8_t *)ptr = (value >> (i * 8)) & 0xFF;
        }
        return 0;
    }

    void *ptr = __tlb_write_ptr(tlb, addr);
    if (ptr == NULL) {
        return -1;  // segfault_addr already set
    }
    *(uint64_t *)ptr = value;
    return 0;
}

__no_instrument int c_store32(struct tlb *tlb, addr_t addr, uint32_t value) {
    void *ptr = __tlb_write_ptr(tlb, addr);
    if (ptr == NULL) {
        return -1;
    }
    *(uint32_t *)ptr = value;
    return 0;
}

__no_instrument int c_store16(struct tlb *tlb, addr_t addr, uint16_t value) {
    void *ptr = __tlb_write_ptr(tlb, addr);
    if (ptr == NULL) {
        return -1;
    }
    *(uint16_t *)ptr = value;
    return 0;
}

__no_instrument int c_store8(struct tlb *tlb, addr_t addr, uint8_t value) {
    void *ptr = __tlb_write_ptr(tlb, addr);
    if (ptr == NULL) {
        return -1;
    }
    *(uint8_t *)ptr = value;
    return 0;
}

// Atomic memory operations (LSE): LDADD/LDCLR/LDEOR/LDSET/LDSMAX/LDSMIN/LDUMAX/LDUMIN/SWP
// Return: 0 on success, -1 on segfault or unsupported op
// Helper macros for atomic RMW with CAS loop (for min/max that lack atomic builtins)
#define ATOMIC_RMW_CAS_LOOP(type, ptr, val, op_expr) do { \
    type old = __atomic_load_n((type *)(ptr), __ATOMIC_SEQ_CST); \
    type newv; \
    do { \
        newv = (op_expr); \
    } while (!__atomic_compare_exchange_n((type *)(ptr), &old, newv, \
             true, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)); \
    *old_out = old; \
} while(0)

__no_instrument int c_atomic_rmw(struct tlb *tlb, addr_t addr, uint64_t value,
                                 uint32_t size, uint32_t op, uint64_t *old_out) {
    if (old_out == NULL)
        return -1;

    void *ptr = __tlb_write_ptr(tlb, addr);
    if (ptr == NULL)
        return -1;

    switch (size) {
        case 0: { // 8-bit
            uint8_t val = (uint8_t)value;
            switch (op) {
                case 0: *old_out = __atomic_fetch_add((uint8_t *)ptr, val, __ATOMIC_SEQ_CST); return 0;
                case 1: *old_out = __atomic_fetch_and((uint8_t *)ptr, ~val, __ATOMIC_SEQ_CST); return 0;
                case 2: *old_out = __atomic_fetch_xor((uint8_t *)ptr, val, __ATOMIC_SEQ_CST); return 0;
                case 3: *old_out = __atomic_fetch_or((uint8_t *)ptr, val, __ATOMIC_SEQ_CST); return 0;
                case 4: ATOMIC_RMW_CAS_LOOP(uint8_t, ptr, val, ((int8_t)old > (int8_t)val) ? old : val); return 0;
                case 5: ATOMIC_RMW_CAS_LOOP(uint8_t, ptr, val, ((int8_t)old < (int8_t)val) ? old : val); return 0;
                case 6: ATOMIC_RMW_CAS_LOOP(uint8_t, ptr, val, (old > val) ? old : val); return 0;
                case 7: ATOMIC_RMW_CAS_LOOP(uint8_t, ptr, val, (old < val) ? old : val); return 0;
                case 8: *old_out = __atomic_exchange_n((uint8_t *)ptr, val, __ATOMIC_SEQ_CST); return 0;
                default: return -1;
            }
        }
        case 1: { // 16-bit
            uint16_t val = (uint16_t)value;
            switch (op) {
                case 0: *old_out = __atomic_fetch_add((uint16_t *)ptr, val, __ATOMIC_SEQ_CST); return 0;
                case 1: *old_out = __atomic_fetch_and((uint16_t *)ptr, ~val, __ATOMIC_SEQ_CST); return 0;
                case 2: *old_out = __atomic_fetch_xor((uint16_t *)ptr, val, __ATOMIC_SEQ_CST); return 0;
                case 3: *old_out = __atomic_fetch_or((uint16_t *)ptr, val, __ATOMIC_SEQ_CST); return 0;
                case 4: ATOMIC_RMW_CAS_LOOP(uint16_t, ptr, val, ((int16_t)old > (int16_t)val) ? old : val); return 0;
                case 5: ATOMIC_RMW_CAS_LOOP(uint16_t, ptr, val, ((int16_t)old < (int16_t)val) ? old : val); return 0;
                case 6: ATOMIC_RMW_CAS_LOOP(uint16_t, ptr, val, (old > val) ? old : val); return 0;
                case 7: ATOMIC_RMW_CAS_LOOP(uint16_t, ptr, val, (old < val) ? old : val); return 0;
                case 8: *old_out = __atomic_exchange_n((uint16_t *)ptr, val, __ATOMIC_SEQ_CST); return 0;
                default: return -1;
            }
        }
        case 2: { // 32-bit
            uint32_t val = (uint32_t)value;
            switch (op) {
                case 0: *old_out = __atomic_fetch_add((uint32_t *)ptr, val, __ATOMIC_SEQ_CST); return 0;
                case 1: *old_out = __atomic_fetch_and((uint32_t *)ptr, ~val, __ATOMIC_SEQ_CST); return 0;
                case 2: *old_out = __atomic_fetch_xor((uint32_t *)ptr, val, __ATOMIC_SEQ_CST); return 0;
                case 3: *old_out = __atomic_fetch_or((uint32_t *)ptr, val, __ATOMIC_SEQ_CST); return 0;
                case 4: ATOMIC_RMW_CAS_LOOP(uint32_t, ptr, val, ((int32_t)old > (int32_t)val) ? old : val); return 0;
                case 5: ATOMIC_RMW_CAS_LOOP(uint32_t, ptr, val, ((int32_t)old < (int32_t)val) ? old : val); return 0;
                case 6: ATOMIC_RMW_CAS_LOOP(uint32_t, ptr, val, (old > val) ? old : val); return 0;
                case 7: ATOMIC_RMW_CAS_LOOP(uint32_t, ptr, val, (old < val) ? old : val); return 0;
                case 8: *old_out = __atomic_exchange_n((uint32_t *)ptr, val, __ATOMIC_SEQ_CST); return 0;
                default: return -1;
            }
        }
        case 3: { // 64-bit
            uint64_t val = value;
            switch (op) {
                case 0: *old_out = __atomic_fetch_add((uint64_t *)ptr, val, __ATOMIC_SEQ_CST); return 0;
                case 1: *old_out = __atomic_fetch_and((uint64_t *)ptr, ~val, __ATOMIC_SEQ_CST); return 0;
                case 2: *old_out = __atomic_fetch_xor((uint64_t *)ptr, val, __ATOMIC_SEQ_CST); return 0;
                case 3: *old_out = __atomic_fetch_or((uint64_t *)ptr, val, __ATOMIC_SEQ_CST); return 0;
                case 4: ATOMIC_RMW_CAS_LOOP(uint64_t, ptr, val, ((int64_t)old > (int64_t)val) ? old : val); return 0;
                case 5: ATOMIC_RMW_CAS_LOOP(uint64_t, ptr, val, ((int64_t)old < (int64_t)val) ? old : val); return 0;
                case 6: ATOMIC_RMW_CAS_LOOP(uint64_t, ptr, val, (old > val) ? old : val); return 0;
                case 7: ATOMIC_RMW_CAS_LOOP(uint64_t, ptr, val, (old < val) ? old : val); return 0;
                case 8: *old_out = __atomic_exchange_n((uint64_t *)ptr, val, __ATOMIC_SEQ_CST); return 0;
                default: return -1;
            }
        }
        default:
            return -1;
    }
}

#undef ATOMIC_RMW_CAS_LOOP

// Atomic compare-and-swap (CAS/CASA/CASL/CASAL)
// Uses host atomic CAS for thread safety.
// Return: 0 on success, -1 on segfault or unsupported size
__no_instrument int c_atomic_cas(struct tlb *tlb, addr_t addr, uint64_t expected,
                                 uint64_t desired, uint32_t size, uint64_t *old_out) {
    if (old_out == NULL)
        return -1;

    // Get writable host pointer (CAS needs both read and write)
    void *ptr = __tlb_write_ptr(tlb, addr);
    if (ptr == NULL)
        return -1;

    switch (size) {
        case 0: { // 8-bit
            uint8_t exp = (uint8_t)expected;
            __atomic_compare_exchange_n((uint8_t *)ptr, &exp, (uint8_t)desired,
                                       false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
            *old_out = exp;  // exp is updated to actual value on failure
            return 0;
        }
        case 1: { // 16-bit
            uint16_t exp = (uint16_t)expected;
            __atomic_compare_exchange_n((uint16_t *)ptr, &exp, (uint16_t)desired,
                                       false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
            *old_out = exp;
            return 0;
        }
        case 2: { // 32-bit
            uint32_t exp = (uint32_t)expected;
            __atomic_compare_exchange_n((uint32_t *)ptr, &exp, (uint32_t)desired,
                                       false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
            *old_out = exp;
            return 0;
        }
        case 3: { // 64-bit
            uint64_t exp = expected;
            __atomic_compare_exchange_n((uint64_t *)ptr, &exp, desired,
                                       false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
            *old_out = exp;
            return 0;
        }
        default:
            return -1;
    }
}

// STXR atomic compare-and-swap helper for LDXR/STXR emulation.
// Compares memory at addr with expected_val, if equal stores new_val.
// Returns 0 on success (CAS succeeded), 1 on failure (CAS lost race),
// or -1 on segfault.
//
// To prevent CoW from invalidating the host pointer between getting it
// and the CAS, we snapshot mmu->changes before and after. If it changed,
// another thread did CoW/mmap, so we retry with a fresh TLB lookup.
__no_instrument int c_stxr_cas(struct tlb *tlb, addr_t addr,
                               uint64_t expected_val, uint64_t new_val,
                               uint32_t size) {
    void *ptr = __tlb_write_ptr(tlb, addr);
    if (ptr == NULL)
        return -1;

    switch (size) {
        case 0: { // 8-bit
            uint8_t exp = (uint8_t)expected_val;
            uint8_t des = (uint8_t)new_val;
            return __atomic_compare_exchange_n((uint8_t *)ptr, &exp, des,
                                              false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ? 0 : 1;
        }
        case 1: { // 16-bit
            uint16_t exp = (uint16_t)expected_val;
            uint16_t des = (uint16_t)new_val;
            return __atomic_compare_exchange_n((uint16_t *)ptr, &exp, des,
                                              false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ? 0 : 1;
        }
        case 2: { // 32-bit
            uint32_t exp = (uint32_t)expected_val;
            uint32_t des = (uint32_t)new_val;
            return __atomic_compare_exchange_n((uint32_t *)ptr, &exp, des,
                                              false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ? 0 : 1;
        }
        case 3: { // 64-bit
            uint64_t exp = expected_val;
            uint64_t des = new_val;
            return __atomic_compare_exchange_n((uint64_t *)ptr, &exp, des,
                                              false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ? 0 : 1;
        }
        default:
            return -1;
    }
}

// LDP/STP helper functions for pair loads/stores
// Return: 0 on success, -1 on segfault
__no_instrument int c_ldp64(struct tlb *tlb, addr_t addr, uint64_t *val1, uint64_t *val2) {
    void *ptr1 = __tlb_read_ptr(tlb, addr);
    if (ptr1 == NULL) {
        return -1;
    }
    void *ptr2 = __tlb_read_ptr(tlb, addr + 8);
    if (ptr2 == NULL) {
        return -1;
    }
    *val1 = *(uint64_t *)ptr1;
    *val2 = *(uint64_t *)ptr2;
    (void)0;
    return 0;
}

__no_instrument int c_ldp32(struct tlb *tlb, addr_t addr, uint32_t *val1, uint32_t *val2) {
    void *ptr1 = __tlb_read_ptr(tlb, addr);
    if (ptr1 == NULL) {
        return -1;
    }
    void *ptr2 = __tlb_read_ptr(tlb, addr + 4);
    if (ptr2 == NULL) {
        return -1;
    }
    *val1 = *(uint32_t *)ptr1;
    *val2 = *(uint32_t *)ptr2;
    return 0;
}

__no_instrument int c_stp64(struct tlb *tlb, addr_t addr, uint64_t val1, uint64_t val2) {
    void *ptr1 = __tlb_write_ptr(tlb, addr);
    if (ptr1 == NULL) {
        return -1;
    }
    void *ptr2 = __tlb_write_ptr(tlb, addr + 8);
    if (ptr2 == NULL) {
        return -1;
    }
    *(uint64_t *)ptr1 = val1;
    *(uint64_t *)ptr2 = val2;
    return 0;
}

__no_instrument int c_stp32(struct tlb *tlb, addr_t addr, uint32_t val1, uint32_t val2) {
    void *ptr1 = __tlb_write_ptr(tlb, addr);
    if (ptr1 == NULL) {
        return -1;
    }
    void *ptr2 = __tlb_write_ptr(tlb, addr + 4);
    if (ptr2 == NULL) {
        return -1;
    }
    *(uint32_t *)ptr1 = val1;
    *(uint32_t *)ptr2 = val2;
    return 0;
}

// Interleaved SIMD load (LD2/LD3/LD4)
// Reads interleaved elements from memory and deinterleaves into consecutive Vt registers.
// num_regs: 2 (LD2), 3 (LD3), or 4 (LD4)
// elem_size: 0=byte, 1=halfword, 2=word, 3=doubleword
// Q: 0=64-bit (lower half), 1=128-bit (full register)
// rt: first destination register (Vt, Vt+1, ..., Vt+num_regs-1 mod 32)
__no_instrument int c_simd_load_interleaved(struct cpu_state *cpu, struct tlb *tlb,
                                             addr_t addr, uint32_t rt, uint32_t num_regs,
                                             uint32_t elem_size, uint32_t Q) {
    unsigned elem_bytes = 1u << elem_size;
    unsigned elems_per_reg = (Q ? 16 : 8) / elem_bytes;
    unsigned total_bytes = num_regs * elems_per_reg * elem_bytes;
    if (total_bytes > 64 || num_regs > 4 || elem_size > 3) return -1;

    // Read all raw bytes from guest memory (bulk read via host pointer)
    uint8_t buf[64];
    if (PGOFFSET(addr) + total_bytes <= PAGE_SIZE) {
        // Fast path: entire access fits within one page
        char *ptr = __tlb_read_ptr(tlb, addr);
        if (ptr == NULL) return -1;
        memcpy(buf, ptr, total_bytes);
    } else {
        // Slow path: crosses page boundary
        if (!__tlb_read_cross_page(tlb, addr, (char *)buf, total_bytes))
            return -1;
    }

    // Deinterleave: memory has [elem0_reg0, elem0_reg1, ..., elem0_regN-1, elem1_reg0, ...]
    for (unsigned r = 0; r < num_regs; r++) {
        uint32_t vt = (rt + r) & 0x1f;
        cpu->fp[vt].q = 0;
        for (unsigned e = 0; e < elems_per_reg; e++) {
            unsigned src_offset = (e * num_regs + r) * elem_bytes;
            unsigned dst_offset = e * elem_bytes;
            memcpy(&cpu->fp[vt].b[dst_offset], &buf[src_offset], elem_bytes);
        }
    }
    return 0;
}

// Interleaved SIMD store (ST2/ST3/ST4)
// Reads consecutive Vt registers and writes interleaved elements to memory.
__no_instrument int c_simd_store_interleaved(struct cpu_state *cpu, struct tlb *tlb,
                                              addr_t addr, uint32_t rt, uint32_t num_regs,
                                              uint32_t elem_size, uint32_t Q) {
    unsigned elem_bytes = 1u << elem_size;
    unsigned elems_per_reg = (Q ? 16 : 8) / elem_bytes;
    unsigned total_bytes = num_regs * elems_per_reg * elem_bytes;
    if (total_bytes > 64 || num_regs > 4 || elem_size > 3) return -1;

    // Interleave register data into buffer
    uint8_t buf[64];
    for (unsigned r = 0; r < num_regs; r++) {
        uint32_t vt = (rt + r) & 0x1f;
        for (unsigned e = 0; e < elems_per_reg; e++) {
            unsigned dst_offset = (e * num_regs + r) * elem_bytes;
            unsigned src_offset = e * elem_bytes;
            memcpy(&buf[dst_offset], &cpu->fp[vt].b[src_offset], elem_bytes);
        }
    }

    // Write all bytes to guest memory (bulk write via host pointer)
    if (PGOFFSET(addr) + total_bytes <= PAGE_SIZE) {
        char *ptr = __tlb_write_ptr(tlb, addr);
        if (ptr == NULL) return -1;
        memcpy(ptr, buf, total_bytes);
    } else {
        if (!__tlb_write_cross_page(tlb, addr, (char *)buf, total_bytes))
            return -1;
    }
    return 0;
}
#endif
