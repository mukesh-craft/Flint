// Exercises the utility layer: sorting, binary search, atomics, mutex,
// rwlock, the CSPRNG, and CLI parsing -- from C, so failures point at the
// runtime rather than at codegen.
#include "flint_util.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// The vec API lives in runtime.c, which flint_util.c also reaches only by
// extern declaration. Declared here so the test can build a vector to sort.
extern void*   flint_vec_new(void);
extern int64_t flint_vec_len(void* v);
extern int64_t flint_vec_get(void* v, int64_t i);
extern void    flint_vec_push(void* v, int64_t val);

static int fails = 0;
static void ck(const char* what, int ok) {
    if (ok) printf("PASS %s\n", what);
    else { printf("FAIL %s\n", what); fails++; }
}

int main(void) {
    // ---- sort + binary search
    void* v = flint_vec_new();
    int64_t in[] = {5, 3, 9, 1, 7, 3, 8, 3, 2, 6};
    for (int i = 0; i < 10; i++) flint_vec_push(v, in[i]);
    flint_sort_i64(v);
    int sorted = 1;
    for (int i = 1; i < 10; i++) if (flint_vec_get(v, i-1) > flint_vec_get(v, i)) sorted = 0;
    ck("sort produces non-decreasing order", sorted);
    ck("first element is the minimum", flint_vec_get(v, 0) == 1);
    ck("last element is the maximum", flint_vec_get(v, 9) == 9);
    ck("bsearch finds an element", flint_bsearch_i64(v, 7) >= 0);
    ck("bsearch rejects an absent element", flint_bsearch_i64(v, 42) == -1);

    // already-sorted and reverse-sorted inputs are the quicksort worst cases
    int64_t asc[64], desc[64];
    for (int i = 0; i < 64; i++) { asc[i] = i; desc[i] = 63 - i; }
    flint_sort_i64_raw(asc, 64);
    int ok = 1; for (int i = 1; i < 64; i++) if (asc[i-1] > asc[i]) ok = 0;
    ck("pre-sorted input stays sorted", ok);
    flint_sort_i64_desc_raw(desc, 64);
    ok = 1; for (int i = 1; i < 64; i++) if (desc[i-1] < desc[i]) ok = 0;
    ck("descending sort", ok);

    // all-equal input is the other pathological case
    int64_t eq[32];
    for (int i = 0; i < 32; i++) eq[i] = 7;
    flint_sort_i64_raw(eq, 32);
    ck("all-equal input", eq[0] == 7 && eq[31] == 7);

    // stable sort preserves input order among equal keys
    int64_t st[6] = {1, 9, 1, 8, 1, 7};
    flint_sort_i64_stable(st, 6);
    ck("stable sort groups equal keys in order",
       st[0] == 1 && st[1] == 1 && st[2] == 1 && st[3] == 7 && st[4] == 8 && st[5] == 9);

    // ---- atomics
    void* c = flint_atomic_new(10);
    flint_atomic_add(c, 5);
    ck("atomic add", flint_atomic_get(c) == 15);
    flint_atomic_sub(c, 3);
    ck("atomic sub", flint_atomic_get(c) == 12);
    ck("cas succeeds on match", flint_atomic_cas(c, 12, 99) == 1);
    ck("cas leaves the new value", flint_atomic_get(c) == 99);
    ck("cas fails on mismatch", flint_atomic_cas(c, 12, 5) == 0);
    flint_atomic_set(c, 7);
    ck("atomic set", flint_atomic_get(c) == 7);

    // ---- mutex (single-threaded round trip)
    void* m = flint_mutex_new();
    flint_mutex_lock(m);
    flint_mutex_unlock(m);
    flint_mutex_trylock(m);
    flint_mutex_unlock(m);
    ck("mutex lock/unlock/trylock round trip", 1);
    flint_mutex_free(m);

    // recursive mutex: re-entry must not deadlock
    void* rm = flint_mutex_new_recursive();
    flint_mutex_lock(rm);
    flint_mutex_lock(rm);
    flint_mutex_unlock(rm);
    flint_mutex_unlock(rm);
    ck("recursive mutex allows re-entry", 1);
    flint_mutex_free(rm);

    // ---- rwlock
    void* rw = flint_rwlock_new();
    flint_rwlock_rdlock(rw);
    flint_rwlock_unlock(rw);
    flint_rwlock_wrlock(rw);
    flint_rwlock_unlock(rw);
    ck("rwlock read and write locks", 1);
    flint_rwlock_free(rw);

    // ---- condvar signal with no waiter must not hang
    void* cv = flint_cond_new();
    flint_cond_signal(cv);
    flint_cond_broadcast(cv);
    ck("condvar signal with no waiter", 1);
    flint_cond_free(cv);

    // ---- CSPRNG
    void* r1 = flint_rng_seed_from_os();
    void* r2 = flint_rng_seed_from_os();
    ck("rng seeds from the OS", r1 != 0 && r2 != 0);
    int in_range = 1, differs = 0;
    for (int i = 0; i < 2000; i++) {
        int64_t b = flint_rng_below(r1, 10);
        if (b < 0 || b >= 10) in_range = 0;
        if (flint_rng_below(r1, 1000000) != flint_rng_below(r2, 1000000)) differs = 1;
    }
    ck("rng_below stays in range", in_range);
    ck("two OS-seeded streams differ", differs);
    int64_t lo = flint_rng_i64_range(r1, 5, 9);
    ck("rng range is inclusive", lo >= 5 && lo <= 9);
    unsigned char buf[16];
    flint_rng_bytes(r1, buf, 16);
    int nonzero = 0;
    for (int i = 0; i < 16; i++) if (buf[i]) nonzero = 1;
    ck("rng fills bytes", nonzero);
    flint_rng_free(r1); flint_rng_free(r2);

    // a fixed seed must be reproducible, or nothing can be tested
    void* s1 = flint_rng_seed(12345);
    void* s2 = flint_rng_seed(12345);
    int same = 1;
    for (int i = 0; i < 8; i++) if (flint_rng_below(s1, 100000) != flint_rng_below(s2, 100000)) same = 0;
    ck("fixed seed is reproducible", same);
    flint_rng_free(s1); flint_rng_free(s2);

    // ---- CLI parsing
    void* cli = flint_cli_new("prog");
    flint_cli_flag(cli, "verbose", 'v', 0);
    flint_cli_flag(cli, "dry-run", 'n', 0);
    flint_cli_opt(cli, "output", 'o', 0);

    char* a0[] = {"prog"};
    ck("no arguments parses", flint_cli_parse(cli, 1, a0) && flint_cli_ok(cli));

    char* a1[] = {"prog", "--verbose", "-o", "out.fl", "in.fl"};
    ck("long flag + short option + positional",
       flint_cli_parse(cli, 5, a1) && flint_cli_ok(cli));
    ck("long flag is seen", flint_cli_has(cli, "verbose"));
    ck("option value is captured", strcmp(flint_cli_get_str(cli, "output"), "out.fl") == 0);
    ck("positional is captured", strcmp(flint_cli_positional(cli, 0), "in.fl") == 0);

    char* a2[] = {"prog", "--output=eq.fl"};
    flint_cli_parse(cli, 2, a2);
    ck("--name=value form", strcmp(flint_cli_get_str(cli, "output"), "eq.fl") == 0);

    char* a3[] = {"prog", "-vn"};
    flint_cli_parse(cli, 2, a3);
    ck("bundled short flags", flint_cli_has(cli, "verbose") && flint_cli_has(cli, "dry-run"));

    char* a4[] = {"prog", "-o"};
    ck("option with a missing value is an error", !flint_cli_parse(cli, 2, a4));
    ck("error message is set", strlen(flint_cli_error(cli)) > 0);

    char* a5[] = {"prog", "--nope"};
    ck("unknown option is an error, not ignored", !flint_cli_parse(cli, 2, a5));

    // A fresh parser: flint_cli_parse does not clear positionals from an
    // earlier parse, so index 0 here would be a leftover from a6's run.
    void* cli2 = flint_cli_new("prog");
    flint_cli_flag(cli2, "verbose", 'v', 0);
    char* a6[] = {"prog", "--", "--not-an-option"};
    ck("-- ends option parsing", flint_cli_parse(cli2, 3, a6)
       && strcmp(flint_cli_positional(cli2, 0), "--not-an-option") == 0);
    flint_cli_free(cli2);

    char* usage = flint_cli_usage(cli);
    ck("usage lists the program", usage && strstr(usage, "prog") != 0);
    free(usage);
    flint_cli_free(cli);

    printf("util-check: %s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}