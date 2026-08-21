// A very small test harness. Enough to name a check, say what it got and what
// it wanted, and answer with an exit status meson can read. Deliberately not a
// framework: these tests link against kernel objects, and anything with its
// own allocator or signal handling would be one more thing between a failure
// and the line that caused it.
#ifndef TESTS_UNIT_H
#define TESTS_UNIT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int unit_failures;
static const char *unit_current = "";

#define TEST(name) \
    static void name(void); \
    static void unit_run_##name(void) { unit_current = #name; name(); } \
    static void name(void)

#define RUN(name) unit_run_##name()

#define UNIT_FAIL(fmt, ...) do { \
    unit_failures++; \
    fprintf(stderr, "FAIL %s:%d [%s]: " fmt "\n", \
            __FILE__, __LINE__, unit_current, ##__VA_ARGS__); \
} while (0)

#define CHECK(cond) do { \
    if (!(cond)) \
        UNIT_FAIL("%s", #cond); \
} while (0)

// Compared and printed as unsigned: most of what these tests look at is a bit
// pattern, and a decimal negative is the wrong way to read one.
#define CHECK_EQ(actual, expected) do { \
    unsigned long long unit_a = (unsigned long long) (actual); \
    unsigned long long unit_e = (unsigned long long) (expected); \
    if (unit_a != unit_e) \
        UNIT_FAIL("%s: got %#llx, want %#llx (%s)", \
                  #actual, unit_a, unit_e, #expected); \
} while (0)

#define CHECK_EQ_INT(actual, expected) do { \
    long long unit_a = (long long) (actual); \
    long long unit_e = (long long) (expected); \
    if (unit_a != unit_e) \
        UNIT_FAIL("%s: got %lld, want %lld (%s)", \
                  #actual, unit_a, unit_e, #expected); \
} while (0)

#define UNIT_REPORT() \
    (unit_failures == 0 ? (printf("ok\n"), 0) \
                        : (fprintf(stderr, "%d check(s) failed\n", unit_failures), 1))

#endif
