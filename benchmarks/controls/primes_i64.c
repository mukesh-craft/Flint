#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdint.h>

/* Same-representation control for benchmarks/primes.c.
 *
 * Flint's only array element type is i64, so benchmarks/primes.fl uses an
 * 80 MB sieve.  The main C/C++/Rust mirrors use 1-byte elements (10 MB),
 * which is a different memory-traffic regime, not a compiler difference.
 * This file is the i64 mirror: same algorithm, same element size.
 */
int main() {
    int64_t limit = 10000000;
    int64_t* sieve = (int64_t*)calloc(limit + 1, sizeof(int64_t));
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
