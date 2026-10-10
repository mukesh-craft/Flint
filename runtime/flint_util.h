// flint_util.h — the utility layer a language is expected to have and Flint
// did not: sorting, binary search, mutexes/atomics, a CLI argument parser,
// and a cryptographic RNG.
//
// These are the gaps that stop a language being usable rather than merely
// interesting. flint_vec is an unsorted growable array with no way to order or
// search it; flint_thread_create exists with no way to protect shared state;
// and every program hand-rolls argv parsing.

#ifndef FLINT_UTIL_H
#define FLINT_UTIL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- sorting ------------------------------------------------------------
// In-place on a vec of i64. Not stable; use flint_sort_i64 stable-by-value
// (the values are unique keys in practice) or sort your own structs.
//
// Returns the number of elements written, for chaining.
int64_t flint_sort_i64(void* v);
int64_t flint_sort_desc_i64(void* v);
// Sorts a raw array of `n` i64, for callers holding their own buffer.
void flint_sort_i64_raw(int64_t* a, int64_t n);
void flint_sort_i64_desc_raw(int64_t* a, int64_t n);
// Stable merge sort for when equal keys must keep their input order (records
// keyed by an id). Allocates a scratch buffer of n i64.
void flint_sort_i64_stable(int64_t* a, int64_t n);
// Sorts vec-of-vec? Not provided: the language has no generics yet, so a
// comparator callback cannot be expressed. Callers sort int64 keys and index
// their own storage.

// ---- binary search ------------------------------------------------------
// All return the index of a match, or -1. On a sorted vec or raw array.
int64_t flint_bsearch_i64(void* v, int64_t key);
int64_t flint_bsearch_raw(const int64_t* a, int64_t n, int64_t key);
int64_t flint_bsearch_desc_i64(void* v, int64_t key);

// ---- mutex --------------------------------------------------------------
// Opaque handle so the pthread type never leaks into Flint code.
void*  flint_mutex_new(void);
int64_t flint_mutex_lock(void* m);
int64_t flint_mutex_trylock(void* m);
int64_t flint_mutex_unlock(void* m);
void   flint_mutex_free(void* m);

// Recursive: the same thread may lock it more than once, and must unlock as
// many times. A non-recursive mutex deadlocks on re-entry, which is easy to
// hit through two functions that each take the same lock.
void*  flint_mutex_new_recursive(void);

// ---- condition variable (needs a mutex; the mutex must be held) ---------
void*  flint_cond_new(void);
int64_t flint_cond_wait(void* cv, void* mutex);
int64_t flint_cond_signal(void* cv);
int64_t flint_cond_broadcast(void* cv);
void   flint_cond_free(void* cv);

// ---- atomics ------------------------------------------------------------
// Atomic read-modify-write on an i64 cell, plus a compare-and-swap loop.
// The cell must be 8-byte aligned; allocate with flint_atomic_new.
void*   flint_atomic_new(int64_t initial);
int64_t flint_atomic_get(void* cell);
int64_t flint_atomic_set(void* cell, int64_t v);
int64_t flint_atomic_add(void* cell, int64_t delta);
int64_t flint_atomic_sub(void* cell, int64_t delta);
// Returns 1 if the swap happened, 0 if the cell was not `expected`.
int64_t flint_atomic_cas(void* cell, int64_t expected, int64_t desired);
void    flint_atomic_free(void* cell);

// ---- read/write lock ----------------------------------------------------
// Many readers OR one writer. A plain mutex serialises readers against each
// other, which defeats the point of a reader-writer lock on read-heavy work.
void*  flint_rwlock_new(void);
int64_t flint_rwlock_rdlock(void* rw);
int64_t flint_rwlock_wrlock(void* rw);
int64_t flint_rwlock_unlock(void* rw);
void   flint_rwlock_free(void* rw);

// ---- cryptographic RNG ---------------------------------------------------
// xoshiro256** seeded from the OS. flint_rand_f64 is a plain LCG: fine for
// jitter, not for keys, tokens or nonces.
//
//   flint_rng_seed_from_os() -> handle
//   flint_rng_u64(h)         uniform 64-bit
//   flint_rng_below(h, n)    uniform in [0, n), unbiased (rejection sampled)
//   flint_rng_bytes(h, buf, n)
void*  flint_rng_seed_from_os(void);
void*  flint_rng_seed(uint64_t seed);
uint64_t flint_rng_u64(void* r);
int64_t flint_rng_below(void* r, int64_t n);
int64_t flint_rng_i64_range(void* r, int64_t lo, int64_t hi);   // inclusive
void   flint_rng_bytes(void* r, void* buf, int64_t n);
void   flint_rng_free(void* r);

// ---- command-line parsing ------------------------------------------------
// A declarative parser so `prog --verbose -o out.fl input.fl` does not need
// hand-rolled index arithmetic. Flags are declared, then queried.
//
//   flint_cli_new(prog)
//   flint_cli_flag(cli, "verbose", 'v', 0)        0 = boolean
//   flint_cli_opt(cli, "output", 'o', 1)          1 = takes a value
//   flint_cli_parse(cli, argc, argv)
//   flint_cli_get_str(cli, "output")               "" if unset
//   flint_cli_get_i64(cli, "count")
//   flint_cli_has(cli, "verbose")
//   flint_cli_positional(cli, i)                   "" past the end
//   flint_cli_usage(cli)                           malloc'd, caller frees
//   flint_cli_free(cli)
//
// "--name=value" and "--name value" both work. Unknown options are an error
// rather than silently ignored, so a typo does not become a silent default.
void*  flint_cli_new(const char* prog);
void   flint_cli_flag(void* cli, const char* name, int64_t shortname, int64_t doc);
void   flint_cli_opt(void* cli, const char* name, int64_t shortname, int64_t doc);
char   flint_cli_short_for(void* cli, const char* name);   // 0 when unset
int64_t flint_cli_parse(void* cli, int64_t argc, char** argv);
int64_t flint_cli_ok(void* cli);
const char* flint_cli_error(void* cli);
const char* flint_cli_get_str(void* cli, const char* name);
int64_t flint_cli_get_i64(void* cli, const char* name);
int64_t flint_cli_has(void* cli, const char* name);
const char* flint_cli_positional(void* cli, int64_t i);
char*  flint_cli_usage(void* cli);
void   flint_cli_free(void* cli);

#ifdef __cplusplus
}
#endif

#endif // FLINT_UTIL_H