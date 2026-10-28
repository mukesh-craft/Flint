#include <stdio.h>
#include <time.h>
#include <stdint.h>

// Iterative fib(45) mirror of benchmarks/fib_iter.fl. The recursive fib.c is
// kept as the original workload; this file makes the Python iterative mirror
// comparable against the same algorithm in every language.
int64_t fib_iter(int64_t n) {
    if (n == 0) return 0;
    int64_t a = 0, b = 1;
    for (int64_t i = 2; i <= n; i++) {
        int64_t next = a + b;
        a = b;
        b = next;
    }
    return b;
}

int main(void) {
    volatile int64_t n = 45;   // keep the loop from being constant-folded
    struct timespec ts0, ts1;
    clock_gettime(CLOCK_MONOTONIC, &ts0);
    int64_t result = fib_iter(n);
    clock_gettime(CLOCK_MONOTONIC, &ts1);
    int64_t elapsed = (int64_t)(ts1.tv_sec - ts0.tv_sec) * 1000000000 +
                      (int64_t)(ts1.tv_nsec - ts0.tv_nsec);
    printf("fib_iter(45) = %ld\n", (long)result);
    printf("time: %ld ns\n", (long)elapsed);
    return 0;
}
