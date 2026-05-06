// cbench_lite.c — lightweight C benchmark for emulators
// Reduced workloads to finish in ~30s under emulation
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static volatile long long sink_i;
static volatile double sink_f;
static volatile int sink_c;
static size_t (*volatile strlen_fn)(const char *) = strlen;
static int (*volatile strcmp_fn)(const char *, const char *) = strcmp;

// Integer: 8M ops
void bench_integer(void) {
    long long a = 1234567890LL, b = 987654321LL, c = 0;
    for (int i = 0; i < 8000000; i++) {
        c += a * b; c ^= (c >> 7);
        a += c & 0xFF; b -= (a << 3) ^ c;
        c = c + (a % (b | 1));
    }
    sink_i = c;
}

// Float: 4M ops
void bench_float(void) {
    double a = 1.23, b = 9.87, c = 0.0;
    for (int i = 0; i < 4000000; i++) {
        c += a * b; a = c / (b + 0.001);
        b = a - c * 0.5; c = a * a + b * b;
    }
    sink_f = c;
}

// Memory sequential: 256MB
void bench_mem_seq(void) {
    int size = 64 * 1024 * 1024; // 256MB
    int *buf = malloc(size * sizeof(int));
    if (!buf) return;
    for (int i = 0; i < size; i++) buf[i] = i;
    long long sum = 0;
    for (int i = 0; i < size; i++) sum += buf[i];
    sink_i = sum;
    free(buf);
}

// Memory random: 4MB, 16M accesses
void bench_mem_rand(void) {
    int size = 1024 * 1024;
    int *buf = malloc(size * sizeof(int));
    if (!buf) return;
    for (int i = 0; i < size; i++) buf[i] = i;
    unsigned int rng = 12345;
    long long sum = 0;
    for (int i = 0; i < 16000000; i++) {
        rng = rng * 1103515245 + 12345;
        sum += buf[(rng >> 16) & (size - 1)];
    }
    sink_i = sum;
    free(buf);
}

// Function call: 50M
int __attribute__((noinline)) add_func(int a, int b) { return a + b; }
void bench_call(void) {
    int r = 0;
    for (int i = 0; i < 50000000; i++) r = add_func(r, i);
    sink_c = r;
}

// Branch: 50M
void bench_branch(void) {
    unsigned int rng = 67890;
    int count = 0;
    for (int i = 0; i < 50000000; i++) {
        rng = rng * 1103515245 + 12345;
        if ((rng & 0xFF) < 128) count++; else count--;
    }
    sink_c = count;
}

// Matrix: 384x384
void bench_matrix(void) {
    int N = 384;
    double *A = malloc(N*N*sizeof(double));
    double *B = malloc(N*N*sizeof(double));
    double *C = calloc(N*N, sizeof(double));
    if (!A||!B||!C) return;
    for (int i = 0; i < N*N; i++) { A[i] = i*0.01; B[i] = i*0.02; }
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++) {
            double s = 0;
            for (int k = 0; k < N; k++) s += A[i*N+k] * B[k*N+j];
            C[i*N+j] = s;
        }
    sink_f = C[0];
    free(A); free(B); free(C);
}

// String: 1M ops
void bench_string(void) {
    char buf1[256], buf2[256];
    memset(buf1, 'A', 255); buf1[255] = 0;
    memset(buf2, 'A', 255); buf2[255] = 0;
    int count = 0;
    for (int i = 0; i < 1000000; i++) {
        buf1[i % 255] = 'A' + (i % 26);
        count += strlen(buf1) + strcmp(buf1, buf2);
        buf1[i % 255] = 'A';
    }
    sink_c = count;
}

// strlen: 2M scans of a 255-byte string
void bench_strlen(void) {
    char buf[256];
    memset(buf, 'A', 255); buf[255] = 0;
    int count = 0;
    for (int i = 0; i < 2000000; i++) {
        count += (int)strlen_fn(buf);
    }
    sink_c = count;
}

// strcmp: 2M equal 255-byte comparisons
void bench_strcmp(void) {
    char buf1[256], buf2[256];
    memset(buf1, 'A', 255); buf1[255] = 0;
    memset(buf2, 'A', 255); buf2[255] = 0;
    int count = 0;
    for (int i = 0; i < 2000000; i++) {
        count += strcmp_fn(buf1, buf2);
    }
    sink_c = count;
}

int main(void) {
    struct { const char *name; void (*fn)(void); } tests[] = {
        {"int_arith_8M",  bench_integer},
        {"float_arith_4M", bench_float},
        {"mem_seq_256MB",   bench_mem_seq},
        {"mem_rand_16M", bench_mem_rand},
        {"func_call_50M",  bench_call},
        {"branch_50M",     bench_branch},
        {"matrix_384x384",  bench_matrix},
        {"string_1M",   bench_string},
        {"strlen_2M",   bench_strlen},
        {"strcmp_2M",   bench_strcmp},
    };
    int n = sizeof(tests)/sizeof(tests[0]);
    for (int i = 0; i < n; i++) {
        long long t0 = now_ms();
        tests[i].fn();
        long long t1 = now_ms();
        printf("%-18s %6lld ms\n", tests[i].name, t1 - t0);
    }
    return 0;
}
