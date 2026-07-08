#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

int main() {
    int64_t n = 10000000;
    char* s = (char*)malloc(n + 1);
    for (int64_t i = 0; i < n; i++) s[i] = 'a' + (i % 26);
    s[n] = 0;
    char* buf = (char*)malloc(n + 1);
    struct timespec ts0, ts1;
    clock_gettime(CLOCK_MONOTONIC, &ts0);
    for (int64_t i = 0; i < n; i++)
        buf[i] = s[n - 1 - i];
    buf[n] = 0;
    clock_gettime(CLOCK_MONOTONIC, &ts1);
    int64_t elapsed = (int64_t)(ts1.tv_sec - ts0.tv_sec) * 1000000000 +
                      (int64_t)(ts1.tv_nsec - ts0.tv_nsec);
    printf("last char: %c\n", buf[n - 1]);
    printf("time: %ld ns\n", (long)elapsed);
    free(s);
    free(buf);
    return 0;
}
