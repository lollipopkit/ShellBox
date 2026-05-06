#include <stdint.h>
#include <stdio.h>

int leaf_add(int, int);

int main(void) {
    uintptr_t expected = 0;
    uintptr_t lr = 0;
    int result = 0;

    __asm__ volatile(
        "adr %[expected], 1f\n"
        "mov w0, #19\n"
        "mov w1, #23\n"
        "bl leaf_add\n"
        "1:\n"
        "mov %[result], x0\n"
        "mov %[lr], x30\n"
        : [expected] "=r" (expected), [lr] "=r" (lr), [result] "=r" (result)
        :
        : "x0", "x1", "x30");

    if (result != 42) {
        printf("result mismatch: got=%d expected=42\n", result);
        return 1;
    }
    if (lr != expected) {
        printf("lr mismatch: got=%lx expected=%lx\n",
               (unsigned long)lr, (unsigned long)expected);
        return 1;
    }

    puts("arm64 bl/lr smoke ok");
    return 0;
}
