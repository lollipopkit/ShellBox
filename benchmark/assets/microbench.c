#define _GNU_SOURCE
// microbench.c - ARM64-focused emulator microbenchmarks.
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef MAP_ANON
#define MAP_ANON MAP_ANONYMOUS
#endif

static volatile uint64_t sink_u64;
static volatile int sink_i;
static size_t (*volatile bench_strlen_fn)(const char *) = strlen;
static int (*volatile bench_strcmp_fn)(const char *, const char *) = strcmp;
static int (*volatile bench_memcmp_fn)(const void *, const void *, size_t) = memcmp;
static void *(*volatile bench_memchr_fn)(const void *, int, size_t) = memchr;
static void *(*volatile bench_memcpy_fn)(void *, const void *, size_t) = memcpy;
static void *(*volatile bench_memmove_fn)(void *, const void *, size_t) = memmove;

static long long now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long) ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void emit(const char *cat, const char *name, long long ns) {
    printf("%s|%s|%lld\n", cat, name, ns);
}

static uint64_t explicit_load64(const char *p) {
    uint64_t value;
#if defined(__aarch64__)
    __asm__ __volatile__("ldr %0, [%1]" : "=r"(value) : "r"(p) : "memory");
#else
    memcpy(&value, p, sizeof(value));
#endif
    return value;
}

static void explicit_store64(char *p, uint64_t value) {
#if defined(__aarch64__)
    __asm__ __volatile__("str %0, [%1]" :: "r"(value), "r"(p) : "memory");
#else
    memcpy(p, &value, sizeof(value));
#endif
}

static void bench_one(const char *cat, const char *name, void (*fn)(void)) {
    long long t0 = now_ns();
    fn();
    long long t1 = now_ns();
    emit(cat, name, t1 - t0);
}

static void *xmalloc(size_t size) {
    void *p = malloc(size);
    if (!p) {
        perror("malloc");
        exit(2);
    }
    return p;
}

static void check_true(int ok, const char *what) {
    if (!ok) {
        fprintf(stderr, "microbench correctness failed: %s\n", what);
        exit(3);
    }
}

static void verify_correctness(void) {
    char a[64];
    char b[64];
    memset(a, 'A', sizeof(a));
    memset(b, 'A', sizeof(b));
    a[sizeof(a) - 1] = '\0';
    b[sizeof(b) - 1] = '\0';
    check_true(bench_memcmp_fn(a, b, sizeof(a)) == 0, "memcmp equal");
    b[sizeof(b) - 2] = 'B';
    check_true(bench_memcmp_fn(a, b, sizeof(a)) < 0, "memcmp last diff");

    char s1[64];
    char s2[64];
    memset(s1, 'x', sizeof(s1));
    memset(s2, 'x', sizeof(s2));
    s1[sizeof(s1) - 1] = '\0';
    s2[sizeof(s2) - 1] = '\0';
    check_true(bench_strcmp_fn(s1, s2) == 0, "strcmp equal");
    s2[sizeof(s2) - 2] = 'y';
    check_true(bench_strcmp_fn(s1, s2) < 0, "strcmp last diff");
    s2[0] = 'w';
    check_true(bench_strcmp_fn(s1, s2) > 0, "strcmp first diff");

    char move[32];
    for (int i = 0; i < (int)sizeof(move); i++) move[i] = (char)i;
    bench_memmove_fn(move + 1, move, 16);
    for (int i = 0; i < 16; i++) {
        check_true(move[i + 1] == (char)i, "memmove overlap forward");
    }
    for (int i = 0; i < (int)sizeof(move); i++) move[i] = (char)i;
    bench_memmove_fn(move, move + 1, 16);
    for (int i = 0; i < 16; i++) {
        check_true(move[i] == (char)(i + 1), "memmove overlap backward");
    }

    char *pages = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    check_true(pages != MAP_FAILED, "crosspage mmap");
    char *cross = pages + 4093;
    for (int i = 0; i < 8; i++) cross[i] = (char)(0x11 + i);
    check_true(explicit_load64(cross) == UINT64_C(0x1817161514131211), "crosspage load64");
    explicit_store64(cross, UINT64_C(0x8877665544332211));
    const unsigned char expected[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    check_true(memcmp(cross, expected, sizeof(expected)) == 0, "crosspage store64");
    munmap(pages, 8192);
}

static char *make_string(size_t len, size_t extra, size_t offset) {
    char *base = xmalloc(len + extra + offset + 64);
    memset(base, 'A', len + extra + offset + 64);
    char *s = base + offset;
    s[len] = '\0';
    return s;
}

#define STRLEN_BENCH(name, len, iters, offset) \
static void name(void) { \
    char *s = make_string((len), 32, (offset)); \
    uint64_t total = 0; \
    for (int i = 0; i < (iters); i++) total += bench_strlen_fn(s); \
    sink_u64 = total; \
    free(s - (offset)); \
}

#define STRCMP_BENCH(name, len, iters, diff_at) \
static void name(void) { \
    char *a = make_string((len), 32, 0); \
    char *b = make_string((len), 32, 0); \
    if ((diff_at) >= 0 && (size_t)(diff_at) < (len)) b[(diff_at)] = 'B'; \
    int total = 0; \
    for (int i = 0; i < (iters); i++) total += bench_strcmp_fn(a, b); \
    sink_i = total; \
    free(a); free(b); \
}

#define MEMCMP_BENCH(name, len, iters, diff_at) \
static void name(void) { \
    char *a = make_string((len), 32, 0); \
    char *b = make_string((len), 32, 0); \
    if ((diff_at) >= 0 && (size_t)(diff_at) < (len)) b[(diff_at)] = 'B'; \
    int total = 0; \
    for (int i = 0; i < (iters); i++) total += bench_memcmp_fn(a, b, (len)); \
    sink_i = total; \
    free(a); free(b); \
}

#define MEMCHR_BENCH(name, len, iters, pos) \
static void name(void) { \
    char *a = make_string((len), 32, 0); \
    a[(pos)] = 'Z'; \
    uint64_t total = 0; \
    for (int i = 0; i < (iters); i++) total += (uintptr_t) bench_memchr_fn(a, 'Z', (len)); \
    sink_u64 = total; \
    free(a); \
}

#define MEMCPY_BENCH(name, len, iters, off_src, off_dst) \
static void name(void) { \
    char *src_base = xmalloc((len) + (off_src) + 64); \
    char *dst_base = xmalloc((len) + (off_dst) + 64); \
    char *src = src_base + (off_src); \
    char *dst = dst_base + (off_dst); \
    memset(src, 0x5a, (len)); \
    for (int i = 0; i < (iters); i++) bench_memcpy_fn(dst, src, (len)); \
    sink_i = dst[(len) - 1]; \
    free(src_base); free(dst_base); \
}

#define MEMMOVE_BENCH(name, len, iters, delta) \
static void name(void) { \
    char *buf = xmalloc((len) + (delta) + 64); \
    memset(buf, 0x2a, (len) + (delta) + 64); \
    for (int i = 0; i < (iters); i++) bench_memmove_fn(buf + (delta), buf, (len)); \
    sink_i = buf[(len) + (delta) - 1]; \
    free(buf); \
}

STRLEN_BENCH(bench_strlen_8, 8, 8000000, 0)
STRLEN_BENCH(bench_strlen_255, 255, 2000000, 0)
STRLEN_BENCH(bench_strlen_4k, 4096, 250000, 0)
STRLEN_BENCH(bench_strlen_255_unaligned, 255, 2000000, 1)

STRCMP_BENCH(bench_strcmp_255_eq, 255, 2000000, -1)
STRCMP_BENCH(bench_strcmp_255_firstdiff, 255, 2000000, 0)
STRCMP_BENCH(bench_strcmp_255_lastdiff, 255, 2000000, 254)

MEMCMP_BENCH(bench_memcmp_255_eq, 255, 2000000, -1)
MEMCMP_BENCH(bench_memcmp_8_eq, 8, 8000000, -1)
MEMCMP_BENCH(bench_memcmp_4k_eq, 4096, 250000, -1)
MEMCHR_BENCH(bench_memchr_255_last, 255, 2000000, 254)
MEMCHR_BENCH(bench_memchr_4k_last, 4096, 250000, 4095)

MEMCPY_BENCH(bench_memcpy_8, 8, 8000000, 0, 0)
MEMCPY_BENCH(bench_memcpy_64, 64, 4000000, 0, 0)
MEMCPY_BENCH(bench_memcpy_4k, 4096, 250000, 0, 0)
MEMCPY_BENCH(bench_memcpy_4k_unaligned, 4096, 250000, 1, 3)
MEMMOVE_BENCH(bench_memmove_4k_overlap, 4096, 250000, 1)

static char *page_bench_buf;
static const size_t page_bench_pages = 4096;
static const size_t page_bench_page_size = 4096;

static void init_page_bench_buf(void) {
    page_bench_buf = xmalloc(page_bench_pages * page_bench_page_size);
    memset(page_bench_buf, 1, page_bench_pages * page_bench_page_size);
}

static void bench_page_seq_read(void) {
    uint64_t total = 0;
    for (int r = 0; r < 128; r++)
        for (size_t p = 0; p < page_bench_pages; p++)
            total += page_bench_buf[p * page_bench_page_size];
    sink_u64 = total;
}

static void bench_page_random_read(void) {
    uint32_t rng = 12345;
    uint64_t total = 0;
    for (int i = 0; i < 2000000; i++) {
        rng = rng * 1103515245u + 12345u;
        total += page_bench_buf[((rng >> 16) & (page_bench_pages - 1)) * page_bench_page_size];
    }
    sink_u64 = total;
}

static void bench_crosspage_load64(void) {
    char *buf = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (buf == MAP_FAILED) return;
    memset(buf, 3, 8192);
    uint64_t total = 0;
    char *p = buf + 4093;
    for (int i = 0; i < 20000000; i++) {
        total += explicit_load64(p);
    }
    sink_u64 = total;
    munmap(buf, 8192);
}

static void bench_unaligned_load64(void) {
    char *buf = xmalloc(128);
    memset(buf, 3, 128);
    uint64_t total = 0;
    char *p = buf + 1;
    for (int i = 0; i < 20000000; i++) {
        total += explicit_load64(p);
    }
    sink_u64 = total;
    free(buf);
}

static void bench_crosspage_store64(void) {
    char *buf = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (buf == MAP_FAILED) return;
    memset(buf, 3, 8192);
    char *p = buf + 4093;
    uint64_t value = 0;
    for (int i = 0; i < 20000000; i++) {
        value = (uint64_t)i;
        explicit_store64(p, value);
    }
    sink_u64 = value;
    munmap(buf, 8192);
}

static void bench_unaligned_store64(void) {
    char *buf = xmalloc(128);
    memset(buf, 3, 128);
    char *p = buf + 1;
    uint64_t value = 0;
    for (int i = 0; i < 20000000; i++) {
        value = (uint64_t)i;
        explicit_store64(p, value);
    }
    sink_u64 = value;
    free(buf);
}

static void bench_getpid(void) {
    int total = 0;
    for (int i = 0; i < 1000000; i++) total += (int) getpid();
    sink_i = total;
}

static void bench_clock_gettime(void) {
    struct timespec ts;
    uint64_t total = 0;
    for (int i = 0; i < 500000; i++) {
        clock_gettime(CLOCK_MONOTONIC, &ts);
        total += (uint64_t) ts.tv_nsec;
    }
    sink_u64 = total;
}

static void bench_read_write_pipe(void) {
    int fd[2];
    char c = 'x';
    if (pipe(fd) != 0) return;
    for (int i = 0; i < 200000; i++) {
        if (write(fd[1], &c, 1) != 1) break;
        if (read(fd[0], &c, 1) != 1) break;
    }
    close(fd[0]);
    close(fd[1]);
    sink_i = c;
}

static void bench_open_fstat_close(void) {
    struct stat st;
    int total = 0;
    for (int i = 0; i < 100000; i++) {
        int fd = open("/bin/sh", O_RDONLY);
        if (fd < 0) continue;
        total += fstat(fd, &st);
        close(fd);
    }
    sink_i = total;
}

static void bench_fstat_fd(void) {
    struct stat st;
    int total = 0;
    int fd = open("/bin/sh", O_RDONLY);
    if (fd < 0) return;
    for (int i = 0; i < 500000; i++) total += fstat(fd, &st);
    close(fd);
    sink_i = total;
}

static void bench_mmap_munmap(void) {
    int total = 0;
    for (int i = 0; i < 20000; i++) {
        void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (p == MAP_FAILED) continue;
        *(volatile char *) p = 1;
        total += munmap(p, 4096);
    }
    sink_i = total;
}

static void bench_fork_exec_true(void) {
    int total = 0;
    for (int i = 0; i < 100; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            execl("/bin/true", "true", (char *) NULL);
            _exit(127);
        }
        if (pid > 0) {
            int status = 0;
            waitpid(pid, &status, 0);
            total += status;
        }
    }
    sink_i = total;
}

int main(void) {
    verify_correctness();
    init_page_bench_buf();

    bench_one("String", "strlen_8", bench_strlen_8);
    bench_one("String", "strlen_255", bench_strlen_255);
    bench_one("String", "strlen_255_unaligned", bench_strlen_255_unaligned);
    bench_one("String", "strlen_4k", bench_strlen_4k);
    bench_one("String", "strcmp_255_eq", bench_strcmp_255_eq);
    bench_one("String", "strcmp_255_firstdiff", bench_strcmp_255_firstdiff);
    bench_one("String", "strcmp_255_lastdiff", bench_strcmp_255_lastdiff);
    bench_one("String", "memcmp_8_eq", bench_memcmp_8_eq);
    bench_one("String", "memcmp_255_eq", bench_memcmp_255_eq);
    bench_one("String", "memcmp_4k_eq", bench_memcmp_4k_eq);
    bench_one("String", "memchr_255_last", bench_memchr_255_last);
    bench_one("String", "memchr_4k_last", bench_memchr_4k_last);

    bench_one("Copy", "memcpy_8", bench_memcpy_8);
    bench_one("Copy", "memcpy_64", bench_memcpy_64);
    bench_one("Copy", "memcpy_4k", bench_memcpy_4k);
    bench_one("Copy", "memcpy_4k_unaligned", bench_memcpy_4k_unaligned);
    bench_one("Copy", "memmove_4k_overlap", bench_memmove_4k_overlap);

    bench_one("Memory", "page_seq_read", bench_page_seq_read);
    bench_one("Memory", "page_random_read", bench_page_random_read);
    bench_one("Memory", "unaligned_load64", bench_unaligned_load64);
    bench_one("Memory", "crosspage_load64", bench_crosspage_load64);
    bench_one("Memory", "unaligned_store64", bench_unaligned_store64);
    bench_one("Memory", "crosspage_store64", bench_crosspage_store64);

    bench_one("Syscall", "getpid_1M", bench_getpid);
    bench_one("Syscall", "clock_gettime_500K", bench_clock_gettime);
    bench_one("Syscall", "pipe_rw_200K", bench_read_write_pipe);
    bench_one("Syscall", "fstat_fd_500K", bench_fstat_fd);
    bench_one("Syscall", "open_fstat_close_100K", bench_open_fstat_close);
    bench_one("Syscall", "mmap_munmap_20K", bench_mmap_munmap);
    bench_one("Syscall", "fork_exec_true_100", bench_fork_exec_true);
    return 0;
}
