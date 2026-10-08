// flint_coll.h — the collections Flint was missing.
//
// Present before this: vec (dynamic array), sb (string builder), map, chan.
// Absent, and needed by ordinary algorithms: set, a priority queue, and a
// deque. All three are O(1)-amortised where you would expect and follow the
// same handle idiom as the existing collections: flint_coll_new returns an
// opaque pointer, flint_coll_free releases it.
//
// Numbers are i64 keys/values, matching vec. That covers the algorithmic
// uses (visited sets, Dijkstra frontiers, work queues) which is what was
// missing; a generic element type would need the type parameters the language
// does not yet carry.

#ifndef FLINT_COLL_H
#define FLINT_COLL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- hash set ----
void*   flint_set_new(void);
void   flint_set_free(void* s);
int64_t flint_set_add(void* s, int64_t v);     // 1 if newly added, 0 if present
int64_t flint_set_has(void* s, int64_t v);     // 1/0
int64_t flint_set_remove(void* s, int64_t v);  // 1 if removed
int64_t flint_set_len(void* s);
void   flint_set_clear(void* s);
// Fills `out` with the members and returns how many; the caller sizes `out`
// to flint_set_len(s).
int64_t flint_set_values(void* s, int64_t* out, int64_t max);

// ---- priority queue (binary min-heap over i64) ----
void*   flint_heap_new(void);
void   flint_heap_free(void* h);
void   flint_heap_push(void* h, int64_t v);
int64_t flint_heap_pop(void* h);        // removes and returns the minimum
int64_t flint_heap_peek(void* h);       // minimum without removing
int64_t flint_heap_len(void* h);
int64_t flint_heap_empty(void* h);
void   flint_heap_clear(void* h);
// Fills `out` in ascending order (drains a copy, leaving `h` unchanged).
int64_t flint_heap_sorted(void* h, int64_t* out, int64_t max);

// ---- deque (double-ended queue) ----
void*   flint_deque_new(void);
void   flint_deque_free(void* d);
void   flint_deque_push_back(void* d, int64_t v);
void   flint_deque_push_front(void* d, int64_t v);
int64_t flint_deque_pop_back(void* d);
int64_t flint_deque_pop_front(void* d);
int64_t flint_deque_front(void* d);     // peek, no remove
int64_t flint_deque_back(void* d);
int64_t flint_deque_len(void* d);
int64_t flint_deque_empty(void* d);
void   flint_deque_clear(void* d);
// Fills `out` front-to-back.
int64_t flint_deque_values(void* d, int64_t* out, int64_t max);

#ifdef __cplusplus
}
#endif

#endif // FLINT_COLL_H