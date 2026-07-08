#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>

int main() {
    int64_t n = 10000000;
    int64_t* data = (int64_t*)malloc(n * sizeof(int64_t));

    for (int64_t i = 0; i < n; i++)
        data[i] = i * 2;

    struct timespec ts0, ts1;
    clock_gettime(CLOCK_MONOTONIC, &ts0);
    int64_t sum = 0;
    for (int64_t i = 0; i < n; i++)
        sum += data[i];
    clock_gettime(CLOCK_MONOTONIC, &ts1);

    int64_t elapsed = (int64_t)(ts1.tv_sec - ts0.tv_sec) * 1000000000 +
                      (int64_t)(ts1.tv_nsec - ts0.tv_nsec);

    printf("sum: %ld\n", (long)sum);
    printf("time: %ld ns\n", (long)elapsed);

    free(data);
    return 0;
}
