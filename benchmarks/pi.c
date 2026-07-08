#include <stdio.h>
#include <time.h>
#include <stdint.h>

int main() {
    int64_t n = 100000000;
    struct timespec ts0, ts1;
    clock_gettime(CLOCK_MONOTONIC, &ts0);
    double pi = 0.0;
    for (int64_t k = 0; k < n; k++) {
        pi += (k % 2 == 0 ? 1.0 : -1.0) / (2.0 * k + 1.0);
    }
    pi *= 4.0;
    clock_gettime(CLOCK_MONOTONIC, &ts1);
    int64_t elapsed = (int64_t)(ts1.tv_sec - ts0.tv_sec) * 1000000000 +
                      (int64_t)(ts1.tv_nsec - ts0.tv_nsec);
    printf("pi = %.10f\n", pi);
    printf("time: %ld ns\n", (long)elapsed);
    return 0;
}
