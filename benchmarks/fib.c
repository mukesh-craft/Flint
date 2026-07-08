#include <stdio.h>
#include <time.h>
#include <stdint.h>

int64_t fib(int64_t n) {
    if (n <= 1) return n;
    return fib(n - 1) + fib(n - 2);
}

int main() {
    struct timespec ts0, ts1;
    clock_gettime(CLOCK_MONOTONIC, &ts0);
    int64_t result = fib(45);
    clock_gettime(CLOCK_MONOTONIC, &ts1);
    int64_t elapsed = (int64_t)(ts1.tv_sec - ts0.tv_sec) * 1000000000 +
                      (int64_t)(ts1.tv_nsec - ts0.tv_nsec);
    printf("fib(45) = %ld\n", (long)result);
    printf("time: %ld ns\n", (long)elapsed);
    return 0;
}
