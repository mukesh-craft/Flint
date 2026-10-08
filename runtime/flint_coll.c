// flint_coll.c — set, priority queue, deque. Per flint_coll.h.
//
// Open-addressed hash table for the set (linear probing, power-of-two
// capacity), binary min-heap for the queue, and a growable ring for the deque.
// No allocation on lookup.

#include "flint_coll.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define EMPTY ((int64_t)0x7FFFFFFFFFFFFFFFLL)   // distinct from any live key

// ============================================================ hash set
typedef struct {
    int64_t* slots;
    int64_t  cap;      // power of two, or 0
    int64_t  len;
} Set;

// Fibonacci hashing: the keys here are often small dense integers, which
// cluster badly under identity or a plain modulo.
static int64_t hash_i64(int64_t x) {
    uint64_t z = (uint64_t)x + 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return (int64_t)(z ^ (z >> 31));
}

void* flint_set_new(void) {
    Set* s = (Set*)calloc(1, sizeof(Set));
    if (!s) return NULL;
    s->cap = 16;
    s->slots = (int64_t*)malloc((size_t)s->cap * sizeof(int64_t));
    if (!s->slots) { free(s); return NULL; }
    for (int64_t i = 0; i < s->cap; i++) s->slots[i] = EMPTY;
    return s;
}

void flint_set_free(void* sp) {
    Set* s = (Set*)sp;
    if (!s) return;
    free(s->slots);
    free(s);
}

// Grow and rehash, preserving every member.
static void set_grow(Set* s) {
    int64_t ncap = s->cap * 2;
    int64_t* ns = (int64_t*)malloc((size_t)ncap * sizeof(int64_t));
    if (!ns) return;                       // stay at the old size; add still works
    for (int64_t i = 0; i < ncap; i++) ns[i] = EMPTY;
    for (int64_t i = 0; i < s->cap; i++) {
        int64_t v = s->slots[i];
        if (v == EMPTY) continue;
        int64_t j = (int64_t)((uint64_t)hash_i64(v) & (uint64_t)(ncap - 1));
        while (ns[j] != EMPTY) j = (j + 1) & (ncap - 1);
        ns[j] = v;
    }
    free(s->slots);
    s->slots = ns;
    s->cap = ncap;
}

int64_t flint_set_add(void* sp, int64_t v) {
    Set* s = (Set*)sp;
    if (!s) return 0;
    // Keep the load factor under 0.7 so probe chains stay short.
    if ((s->len + 1) * 10 >= s->cap * 7) set_grow(s);
    int64_t mask = s->cap - 1;
    int64_t i = (int64_t)((uint64_t)hash_i64(v) & (uint64_t)mask);
    while (s->slots[i] != EMPTY) {
        if (s->slots[i] == v) return 0;     // already present
        i = (i + 1) & mask;
    }
    s->slots[i] = v;
    s->len++;
    return 1;
}

int64_t flint_set_has(void* sp, int64_t v) {
    Set* s = (Set*)sp;
    if (!s || s->len == 0) return 0;
    int64_t mask = s->cap - 1;
    int64_t i = (int64_t)((uint64_t)hash_i64(v) & (uint64_t)mask);
    int64_t probes = 0;
    while (s->slots[i] != EMPTY && probes <= s->len) {
        if (s->slots[i] == v) return 1;
        i = (i + 1) & mask;
        probes++;
    }
    return 0;
}

// Backward-shift deletion: no tombstones, so the table stays dense. Tombstones
// would need periodic rehashing and would inflate `len`.
int64_t flint_set_remove(void* sp, int64_t v) {
    Set* s = (Set*)sp;
    if (!s || s->len == 0) return 0;
    int64_t mask = s->cap - 1;
    int64_t i = (int64_t)((uint64_t)hash_i64(v) & (uint64_t)mask);
    int64_t probes = 0;
    while (s->slots[i] != EMPTY && probes <= s->len) {
        if (s->slots[i] == v) break;
        i = (i + 1) & mask;
        probes++;
    }
    if (s->slots[i] != v) return 0;
    s->slots[i] = EMPTY;
    s->len--;
    // Pull back any entry that probed through the hole, or it becomes
    // unreachable.
    int64_t j = (i + 1) & mask;
    while (s->slots[j] != EMPTY) {
        int64_t moved = s->slots[j];
        s->slots[j] = EMPTY;
        s->len--;
        flint_set_add(s, moved);
        j = (j + 1) & mask;
    }
    return 1;
}

int64_t flint_set_len(void* sp)   { Set* s = (Set*)sp; return s ? s->len : 0; }

void flint_set_clear(void* sp) {
    Set* s = (Set*)sp;
    if (!s) return;
    for (int64_t i = 0; i < s->cap; i++) s->slots[i] = EMPTY;
    s->len = 0;
}

int64_t flint_set_values(void* sp, int64_t* out, int64_t max) {
    Set* s = (Set*)sp;
    if (!s || !out) return 0;
    int64_t n = 0;
    for (int64_t i = 0; i < s->cap && n < max; i++)
        if (s->slots[i] != EMPTY) out[n++] = s->slots[i];
    return n;
}

// ====================================================== priority queue
typedef struct {
    int64_t* a;
    int64_t  len, cap;
} Heap;

void* flint_heap_new(void) {
    Heap* h = (Heap*)calloc(1, sizeof(Heap));
    if (!h) return NULL;
    h->cap = 16;
    h->a = (int64_t*)malloc((size_t)h->cap * sizeof(int64_t));
    return h;
}

void flint_heap_free(void* hp) {
    Heap* h = (Heap*)hp;
    if (!h) return;
    free(h->a);
    free(h);
}

void flint_heap_push(void* hp, int64_t v) {
    Heap* h = (Heap*)hp;
    if (!h) return;
    if (h->len == h->cap) {
        int64_t ncap = h->cap * 2;
        int64_t* na = (int64_t*)realloc(h->a, (size_t)ncap * sizeof(int64_t));
        if (!na) return;
        h->a = na;
        h->cap = ncap;
    }
    // Sift up from the end.
    int64_t i = h->len++;
    h->a[i] = v;
    while (i > 0) {
        int64_t parent = (i - 1) / 2;
        if (h->a[parent] <= h->a[i]) break;
        int64_t t = h->a[parent]; h->a[parent] = h->a[i]; h->a[i] = t;
        i = parent;
    }
}

int64_t flint_heap_pop(void* hp) {
    Heap* h = (Heap*)hp;
    if (!h || h->len == 0) return 0;
    int64_t top = h->a[0];
    h->a[0] = h->a[--h->len];
    // Sift down from the root.
    int64_t i = 0;
    for (;;) {
        int64_t l = 2 * i + 1, r = 2 * i + 2, m = i;
        if (l < h->len && h->a[l] < h->a[m]) m = l;
        if (r < h->len && h->a[r] < h->a[m]) m = r;
        if (m == i) break;
        int64_t t = h->a[m]; h->a[m] = h->a[i]; h->a[i] = t;
        i = m;
    }
    return top;
}

int64_t flint_heap_peek(void* hp) {
    Heap* h = (Heap*)hp;
    return (h && h->len > 0) ? h->a[0] : 0;
}

int64_t flint_heap_len(void* hp)   { Heap* h = (Heap*)hp; return h ? h->len : 0; }
int64_t flint_heap_empty(void* hp) { return flint_heap_len(hp) == 0; }

void flint_heap_clear(void* hp) {
    Heap* h = (Heap*)hp;
    if (h) h->len = 0;
}

int64_t flint_heap_sorted(void* hp, int64_t* out, int64_t max) {
    Heap* h = (Heap*)hp;
    if (!h || !out) return 0;
    int64_t n = 0;
    while (n < max && h->len > 0) out[n++] = flint_heap_pop(h);
    return n;
}

// ============================================================== deque
// Ring buffer: push/pop at both ends without shifting.
typedef struct {
    int64_t* a;
    int64_t  cap, head, len;
} Deque;

void* flint_deque_new(void) {
    Deque* d = (Deque*)calloc(1, sizeof(Deque));
    if (!d) return NULL;
    d->cap = 16;
    d->a = (int64_t*)malloc((size_t)d->cap * sizeof(int64_t));
    return d;
}

void flint_deque_free(void* dp) {
    Deque* d = (Deque*)dp;
    if (!d) return;
    free(d->a);
    free(d);
}

static void deque_grow(Deque* d) {
    int64_t ncap = d->cap * 2;
    int64_t* na = (int64_t*)malloc((size_t)ncap * sizeof(int64_t));
    if (!na) return;
    // Linearise, then copy: the ring order is no longer contiguous.
    for (int64_t i = 0; i < d->len; i++) na[i] = d->a[(d->head + i) % d->cap];
    free(d->a);
    d->a = na;
    d->cap = ncap;
    d->head = 0;
}

void flint_deque_push_back(void* dp, int64_t v) {
    Deque* d = (Deque*)dp;
    if (!d) return;
    if (d->len == d->cap) deque_grow(d);
    d->a[(d->head + d->len) % d->cap] = v;
    d->len++;
}

void flint_deque_push_front(void* dp, int64_t v) {
    Deque* d = (Deque*)dp;
    if (!d) return;
    if (d->len == d->cap) deque_grow(d);
    d->head = (d->head - 1 + d->cap) % d->cap;
    d->a[d->head] = v;
    d->len++;
}

int64_t flint_deque_pop_front(void* dp) {
    Deque* d = (Deque*)dp;
    if (!d || d->len == 0) return 0;
    int64_t v = d->a[d->head];
    d->head = (d->head + 1) % d->cap;
    d->len--;
    return v;
}

int64_t flint_deque_pop_back(void* dp) {
    Deque* d = (Deque*)dp;
    if (!d || d->len == 0) return 0;
    d->len--;
    return d->a[(d->head + d->len) % d->cap];
}

int64_t flint_deque_front(void* dp) {
    Deque* d = (Deque*)dp;
    return (d && d->len > 0) ? d->a[d->head] : 0;
}

int64_t flint_deque_back(void* dp) {
    Deque* d = (Deque*)dp;
    return (d && d->len > 0) ? d->a[(d->head + d->len - 1) % d->cap] : 0;
}

int64_t flint_deque_len(void* dp)   { Deque* d = (Deque*)dp; return d ? d->len : 0; }
int64_t flint_deque_empty(void* dp) { return flint_deque_len(dp) == 0; }

void flint_deque_clear(void* dp) {
    Deque* d = (Deque*)dp;
    if (!d) return;
    d->head = 0;
    d->len = 0;
}

int64_t flint_deque_values(void* dp, int64_t* out, int64_t max) {
    Deque* d = (Deque*)dp;
    if (!d || !out) return 0;
    int64_t n = 0;
    for (int64_t i = 0; i < d->len && n < max; i++) out[n++] = d->a[(d->head + i) % d->cap];
    return n;
}