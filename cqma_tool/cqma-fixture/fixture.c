#include <stdlib.h>

static int unused_helper(int x) { return x * 2; }

int shadow_demo(int value) {
    int total = value;
    for (int total = 0; total < 2; total++) { }
    unsigned char small = 300;
    return total + (int)small;
}

int main(void) {
    volatile char *buf = malloc(4);
    buf[4] = 'x';
    free((void *)buf);
    int *np = NULL;
    *np = 5;
    return 0;
}
