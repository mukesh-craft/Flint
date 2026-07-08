#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdint.h>
#include <string.h>

int main() {
    int64_t limit = 10000000;
    char* sieve = (char*)calloc(limit + 1, 1);
    struct timespec ts0, ts1;
    clock_gettime(CLOCK_MONOTONIC, &ts0);
    for (int64_t i = 2; i * i <= limit; i++)
        if (!sieve[i])
            for (int64_t j = i * i; j <= limit; j += i)
                sieve[j] = 1;
    int64_t count = 0;
    for (int64_t i = 2; i <= limit; i++)
        if (!sieve[i]) count++;
    clock_gettime(CLOCK_MONOTONIC, &ts1);
    int64_t elapsed = (int64_t)(ts1.tv_sec - ts0.tv_sec) * 1000000000 +
                      (int64_t)(ts1.tv_nsec - ts0.tv_nsec);
    printf("primes up to 10M: %ld\n", (long)count);
    printf("time: %ld ns\n", (long)elapsed);
    free(sieve);
    return 0;
}
