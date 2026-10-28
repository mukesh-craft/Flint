#include <stdlib.h>
#include <string.h>

static char *global_cache;

int main(void) {
    volatile char *leaked = malloc(64);
    if (!leaked) return 1;
    leaked[0] = 1;
    leaked[63] = 2;
    global_cache = malloc(32);
    if (!global_cache) return 1;
    global_cache[0] = 3;
    return 0;
}
