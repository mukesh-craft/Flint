// flint_util.c — sorting, binary search, synchronisation, CSPRNG and CLI
// parsing. Per flint_util.h.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "flint_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

// Flint's vec, so sorting can operate on one without a dependency cycle.
// Declared here rather than included: runtime.c owns the definition.
typedef struct { int64_t* data; int64_t len, cap; } FlintVec;
extern void*   flint_vec_new(void);
extern int64_t flint_vec_len(void* v);
extern int64_t flint_vec_get(void* v, int64_t i);
extern void    flint_vec_set(void* v, int64_t i, int64_t val);
extern void    flint_vec_push(void* v, int64_t val);

// =========================================================================
// Sorting
// =========================================================================
// Introsort-ish: quicksort with median-of-three and an insertion-sort tail.
// Worst case is O(n^2) on adversarial input, so equal-heavy data falls back
// to heapsort. Not stable -- see flint_sort_i64_stable for that.
static void _swap(int64_t* a, int64_t* b) { int64_t t = *a; *a = *b; *b = t; }

// Sift-down, shared by the build phase and the extraction phase.
static void _sift_down(int64_t* a, int64_t n, int64_t root) {
    for (;;) {
        int64_t big = root, l = 2 * root + 1, r = l + 1;
        if (l < n && a[l] > a[big]) big = l;
        if (r < n && a[r] > a[big]) big = r;
        if (big == root) break;
        _swap(&a[root], &a[big]);
        root = big;
    }
}

// Worst case O(n log n), reached when the quicksort depth limit trips: on
// already-sorted or equal-heavy input quicksort alone degrades to O(n^2).
static void _heapsort(int64_t* a, int64_t n) {
    for (int64_t i = n / 2 - 1; i >= 0; i--) _sift_down(a, n, i);
    for (int64_t i = n - 1; i > 0; i--) { _swap(&a[0], &a[i]); _sift_down(a, i, 0); }
}

static void _qsort_i64(int64_t* a, int64_t lo, int64_t hi, int depth) {
    while (lo < hi) {
        if (hi - lo < 12) {                       // insertion sort a short tail
            for (int64_t i = lo + 1; i <= hi; i++) {
                int64_t k = a[i], j = i - 1;
                while (j >= lo && a[j] > k) { a[j + 1] = a[j]; j--; }
                a[j + 1] = k;
            }
            return;
        }
        // Median of three guards against sorted input giving O(n^2).
        int64_t mid = lo + (hi - lo) / 2;
        if (a[mid] < a[lo]) _swap(&a[mid], &a[lo]);
        if (a[hi] < a[lo]) _swap(&a[hi], &a[lo]);
        if (a[hi] < a[mid]) _swap(&a[hi], &a[mid]);
        int64_t pivot = a[mid];

        int64_t i = lo, j = hi;
        for (;;) {
            while (a[i] < pivot) i++;
            while (a[j] > pivot) j--;
            if (i >= j) break;
            _swap(&a[i], &a[j]);
            i++; j--;
        }
        if (depth-- <= 0) { _heapsort(a + lo, hi - lo + 1); return; }
        // Recurse into the smaller side, loop on the larger: bounded stack.
        if (j - lo < hi - i) { _qsort_i64(a, lo, j, depth); lo = i; }
        else { _qsort_i64(a, i, hi, depth); hi = j; }
    }
}

void flint_sort_i64_raw(int64_t* a, int64_t n) {
    if (!a || n < 2) return;
    _qsort_i64(a, 0, n - 1, 2 * (int64_t)(sizeof(int64_t) * 8));
}

void flint_sort_i64_desc_raw(int64_t* a, int64_t n) {
    flint_sort_i64_raw(a, n);
    for (int64_t i = 0, j = n - 1; i < j; i++, j--) _swap(&a[i], &a[j]);
}

// Stable merge sort, for when equal keys must keep input order (records keyed
// by an id). One scratch buffer, so O(n) extra space and O(n log n) time.
void flint_sort_i64_stable(int64_t* a, int64_t n) {
    if (!a || n < 2) return;
    int64_t* tmp = (int64_t*)malloc((size_t)n * sizeof(int64_t));
    if (!tmp) { flint_sort_i64_raw(a, n); return; }   // degrade, don't fail
    for (int64_t width = 1; width < n; width *= 2) {
        for (int64_t i = 0; i < n; i += 2 * width) {
            int64_t l = i, m = i + width < n ? i + width : n, r = i + 2 * width < n ? i + 2 * width : n;
            int64_t x = l, y = m, k = l;
            while (x < m && y < r) tmp[k++] = (a[x] <= a[y]) ? a[x++] : a[y++];
            while (x < m) tmp[k++] = a[x++];
            while (y < r) tmp[k++] = a[y++];
        }
        memcpy(a, tmp, (size_t)n * sizeof(int64_t));
    }
    free(tmp);
}

int64_t flint_sort_i64(void* v) {
    int64_t n = flint_vec_len(v);
    if (n > 1) {
        int64_t* buf = (int64_t*)malloc((size_t)n * sizeof(int64_t));
        if (buf) {
            for (int64_t i = 0; i < n; i++) buf[i] = flint_vec_get(v, i);
            flint_sort_i64_raw(buf, n);
            for (int64_t i = 0; i < n; i++) flint_vec_set(v, i, buf[i]);
            free(buf);
            return n;
        }
    }
    return n;
}

int64_t flint_sort_desc_i64(void* v) {
    int64_t n = flint_vec_len(v);
    if (n > 1) {
        int64_t* buf = (int64_t*)malloc((size_t)n * sizeof(int64_t));
        if (buf) {
            for (int64_t i = 0; i < n; i++) buf[i] = flint_vec_get(v, i);
            flint_sort_i64_desc_raw(buf, n);
            for (int64_t i = 0; i < n; i++) flint_vec_set(v, i, buf[i]);
            free(buf);
            return n;
        }
    }
    return n;
}

// =========================================================================
// Binary search
// =========================================================================
int64_t flint_bsearch_raw(const int64_t* a, int64_t n, int64_t key) {
    if (!a || n <= 0) return -1;
    int64_t lo = 0, hi = n - 1;
    while (lo <= hi) {
        int64_t mid = lo + (hi - lo) / 2;
        if (a[mid] == key) return mid;
        if (a[mid] < key) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

int64_t flint_bsearch_i64(void* v, int64_t key) {
    int64_t n = flint_vec_len(v);
    if (n <= 0) return -1;
    int64_t* buf = (int64_t*)malloc((size_t)n * sizeof(int64_t));
    if (!buf) return -1;
    for (int64_t i = 0; i < n; i++) buf[i] = flint_vec_get(v, i);
    int64_t r = flint_bsearch_raw(buf, n, key);
    free(buf);
    return r;
}

int64_t flint_bsearch_desc_i64(void* v, int64_t key) {
    int64_t n = flint_vec_len(v);
    if (n <= 0) return -1;
    int64_t* buf = (int64_t*)malloc((size_t)n * sizeof(int64_t));
    if (!buf) return -1;
    for (int64_t i = 0; i < n; i++) buf[i] = flint_vec_get(v, i);
    flint_sort_i64_desc_raw(buf, n);
    int64_t lo = 0, hi = n - 1, r = -1;
    while (lo <= hi) {
        int64_t mid = lo + (hi - lo) / 2;
        if (buf[mid] == key) { r = mid; break; }
        if (buf[mid] > key) lo = mid + 1; else hi = mid - 1;   // descending
    }
    free(buf);
    return r;
}

// =========================================================================
// Mutex / cond / rwlock
// =========================================================================
// Opaque wrapper so pthread_t never crosses into Flint code, which keeps the
// builtin's LLVM signature to a single pointer.
typedef struct { pthread_mutex_t m; int recursive; } FMutex;
typedef struct { pthread_cond_t c; } FCond;
typedef struct { pthread_rwlock_t rw; } FRWLock;

void* flint_mutex_new(void) {
    FMutex* f = (FMutex*)calloc(1, sizeof(FMutex));
    if (!f) return NULL;
    pthread_mutexattr_t at;
    pthread_mutexattr_init(&at);
    pthread_mutex_init(&f->m, &at);
    pthread_mutexattr_destroy(&at);
    return f;
}

void* flint_mutex_new_recursive(void) {
    FMutex* f = (FMutex*)calloc(1, sizeof(FMutex));
    if (!f) return NULL;
    pthread_mutexattr_t at;
    pthread_mutexattr_init(&at);
    pthread_mutexattr_settype(&at, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&f->m, &at);
    pthread_mutexattr_destroy(&at);
    f->recursive = 1;
    return f;
}

int64_t flint_mutex_lock(void* p)   { FMutex* f = (FMutex*)p; return f ? pthread_mutex_lock(&f->m) == 0 : 0; }
int64_t flint_mutex_trylock(void* p){ FMutex* f = (FMutex*)p; return f ? pthread_mutex_trylock(&f->m) == 0 : 0; }
int64_t flint_mutex_unlock(void* p){ FMutex* f = (FMutex*)p; return f ? pthread_mutex_unlock(&f->m) == 0 : 0; }
void   flint_mutex_free(void* p)    { if (!p) return; pthread_mutex_destroy(&((FMutex*)p)->m); free(p); }

void* flint_cond_new(void) {
    FCond* c = (FCond*)calloc(1, sizeof(FCond));
    if (!c) return NULL;
    pthread_cond_init(&c->c, NULL);
    return c;
}
int64_t flint_cond_wait(void* cp, void* mp) {
    FCond* c = (FCond*)cp; FMutex* m = (FMutex*)mp;
    return (c && m) ? pthread_cond_wait(&c->c, &m->m) == 0 : 0;
}
int64_t flint_cond_signal(void* cp)   { FCond* c = (FCond*)cp; return c ? pthread_cond_signal(&c->c) == 0 : 0; }
int64_t flint_cond_broadcast(void* cp){ FCond* c = (FCond*)cp; return c ? pthread_cond_broadcast(&c->c) == 0 : 0; }
void   flint_cond_free(void* cp)     { if (!cp) return; pthread_cond_destroy(&((FCond*)cp)->c); free(cp); }

void* flint_rwlock_new(void) {
    FRWLock* r = (FRWLock*)calloc(1, sizeof(FRWLock));
    if (!r) return NULL;
    pthread_rwlock_init(&r->rw, NULL);
    return r;
}
int64_t flint_rwlock_rdlock(void* p){ FRWLock* r = (FRWLock*)p; return r ? pthread_rwlock_rdlock(&r->rw) == 0 : 0; }
int64_t flint_rwlock_wrlock(void* p){ FRWLock* r = (FRWLock*)p; return r ? pthread_rwlock_wrlock(&r->rw) == 0 : 0; }
int64_t flint_rwlock_unlock(void* p){ FRWLock* r = (FRWLock*)p; return r ? pthread_rwlock_unlock(&r->rw) == 0 : 0; }
void   flint_rwlock_free(void* p)   { if (!p) return; pthread_rwlock_destroy(&((FRWLock*)p)->rw); free(p); }

// =========================================================================
// Atomics
// =========================================================================
// A heap cell rather than a raw i64, because a compiler may pass an i64 by
// value -- a lock-free counter needs an address, not a copy.
void* flint_atomic_new(int64_t initial) {
    int64_t* c = (int64_t*)malloc(sizeof(int64_t));
    if (c) *c = initial;
    return c;
}
int64_t flint_atomic_get(void* p) {
    int64_t* c = (int64_t*)p;
    return c ? __atomic_load_n(c, __ATOMIC_SEQ_CST) : 0;
}
int64_t flint_atomic_set(void* p, int64_t v) {
    int64_t* c = (int64_t*)p;
    if (!c) return 0;
    __atomic_store_n(c, v, __ATOMIC_SEQ_CST);
    return v;
}
int64_t flint_atomic_add(void* p, int64_t d) {
    int64_t* c = (int64_t*)p;
    return c ? __atomic_fetch_add(c, d, __ATOMIC_SEQ_CST) : 0;
}
int64_t flint_atomic_sub(void* p, int64_t d) {
    int64_t* c = (int64_t*)p;
    return c ? __atomic_fetch_sub(c, d, __ATOMIC_SEQ_CST) : 0;
}
int64_t flint_atomic_cas(void* p, int64_t expected, int64_t desired) {
    int64_t* c = (int64_t*)p;
    if (!c) return 0;
    return __atomic_compare_exchange_n(c, &expected, desired, 0,
                                      __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ? 1 : 0;
}
void flint_atomic_free(void* p) { free(p); }

// =========================================================================
// CSPRNG: xoshiro256-star-star (the 256-bit variant).
// =========================================================================
// flint_rand_f64 is a plain LCG -- fine for jitter, wrong for keys, tokens
// and nonces. SplitMix64 seeds this from the OS.
typedef struct { uint64_t s[4]; } FLinkRng;

static uint64_t _rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

static uint64_t _splitmix64(uint64_t* x) {
    uint64_t z = (*x += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

void* flint_rng_seed(uint64_t seed) {
    FLinkRng* r = (FLinkRng*)calloc(1, sizeof(FLinkRng));
    if (!r) return NULL;
    uint64_t x = seed ? seed : 0x853C49E6748FEA9BULL;
    for (int i = 0; i < 4; i++) r->s[i] = _splitmix64(&x);
    return r;
}

void* flint_rng_seed_from_os(void) {
    // Prefer getrandom: it is the kernel CSPRNG and cannot be seeded wrong.
    uint64_t seed = 0;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        ssize_t got = read(fd, &seed, sizeof seed);
        close(fd);
        if (got == (ssize_t)sizeof seed && seed != 0) return flint_rng_seed(seed);
    }
    // Fallback: clock XOR pid XOR address entropy.
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    seed = (uint64_t)ts.tv_nsec * 1000003ULL
         ^ (uint64_t)ts.tv_sec
         ^ (uint64_t)getpid() << 32
         ^ (uint64_t)(uintptr_t)&ts;
    return flint_rng_seed(seed);
}

uint64_t flint_rng_u64(void* p) {
    FLinkRng* r = (FLinkRng*)p;
    if (!r) return 0;
    uint64_t* s = r->s;
    const uint64_t result = _rotl(s[1] * 5, 7) * 9;
    const uint64_t t = s[1] << 17;
    s[2] ^= s[0]; s[3] ^= s[1]; s[1] ^= s[2]; s[0] ^= s[3];
    s[2] ^= t;    s[3] = _rotl(s[3], 45);
    return result;
}

// Rejection sampling: taking u64 % n biases toward the low values whenever n
// does not divide 2^64, which matters for anything security-relevant.
int64_t flint_rng_below(void* p, int64_t n) {
    if (n <= 0) return 0;
    uint64_t bound = (uint64_t)n;
    uint64_t limit = UINT64_MAX - (UINT64_MAX % bound) - 1;
    uint64_t v;
    do { v = flint_rng_u64(p); } while (v > limit);
    return (int64_t)(v % bound);
}

int64_t flint_rng_i64_range(void* p, int64_t lo, int64_t hi) {
    if (hi < lo) { int64_t t = lo; lo = hi; hi = t; }
    uint64_t span = (uint64_t)(hi - lo) + 1;
    if (span == 0) return (int64_t)flint_rng_u64(p);   // full 64-bit span
    return lo + flint_rng_below(p, (int64_t)span);
}

void flint_rng_bytes(void* p, void* buf, int64_t n) {
    unsigned char* out = (unsigned char*)buf;
    for (int64_t i = 0; i < n; i++) out[i] = (unsigned char)(flint_rng_u64(p) & 0xFF);
}

void flint_rng_free(void* p) { free(p); }

// =========================================================================
// CLI argument parsing
// =========================================================================
typedef struct {
    const char* name;
    char        shortname;
    int64_t     takes_value;   // 0 = boolean flag, 1 = option with a value
    int64_t     seen;
    char*       value;
    int64_t     doc;
} CliOpt;

typedef struct {
    const char* prog;
    CliOpt*     opts;
    int64_t     nopts, cap;
    char**      pos;
    int64_t     npos, poscap;
    int64_t     ok;
    char        err[192];
} Cli;

void* flint_cli_new(const char* prog) {
    Cli* c = (Cli*)calloc(1, sizeof(Cli));
    if (!c) return NULL;
    c->prog = prog ? prog : "program";
    c->ok = 1;
    return c;
}

static CliOpt* _find_long(Cli* c, const char* n);

static CliOpt* _cli_add(void* cp, const char* name, char sn, int64_t takes) {
    Cli* c = (Cli*)cp;
    if (c->nopts == c->cap) {
        int64_t ncap = c->cap ? c->cap * 2 : 8;
        CliOpt* no = (CliOpt*)realloc(c->opts, (size_t)ncap * sizeof(CliOpt));
        if (!no) return NULL;
        c->opts = no;
        c->cap = ncap;
    }
    CliOpt* o = &c->opts[c->nopts];
    memset(o, 0, sizeof *o);
    o->name = name;
    o->shortname = sn;
    o->takes_value = takes;
    c->nopts++;
    return o;
}

void flint_cli_flag(void* cli, const char* name, int64_t sn, int64_t doc) {
    CliOpt* o = _cli_add(cli, name, (char)(sn ? sn : 0), 0);
    if (o) o->doc = doc;
}
void flint_cli_opt(void* cli, const char* name, int64_t sn, int64_t doc) {
    CliOpt* o = _cli_add(cli, name, (char)(sn ? sn : 0), 1);
    if (o) o->doc = doc;
}

// Which short letter was registered for a long name, or 0. Lets a caller
// report or test the binding without a char literal.
char flint_cli_short_for(void* cli, const char* name) {
    Cli* c = (Cli*)cli;
    CliOpt* o = c ? _find_long(c, name) : NULL;
    return o ? o->shortname : 0;
}

static CliOpt* _find_long(Cli* c, const char* n) {
    for (int64_t i = 0; i < c->nopts; i++)
        if (strcmp(c->opts[i].name, n) == 0) return &c->opts[i];
    return NULL;
}
static CliOpt* _find_short(Cli* c, char ch) {
    if (!ch) return NULL;
    for (int64_t i = 0; i < c->nopts; i++)
        if (c->opts[i].shortname == ch) return &c->opts[i];
    return NULL;
}

static void _cli_push_pos(Cli* c, char* s) {
    if (c->npos == c->poscap) {
        int64_t ncap = c->poscap ? c->poscap * 2 : 8;
        char** np = (char**)realloc(c->pos, (size_t)ncap * sizeof(char*));
        if (!np) return;
        c->pos = np;
        c->poscap = ncap;
    }
    c->pos[c->npos++] = s;
}

int64_t flint_cli_parse(void* cp, int64_t argc, char** argv) {
    Cli* c = (Cli*)cp;
    if (!c) return 0;
    c->ok = 1;
    c->err[0] = '\0';
    int64_t only_pos = 0;              // after "--", everything is positional

    for (int64_t i = 1; i < argc; i++) {
        char* a = argv[i];
        if (only_pos) { _cli_push_pos(c, a); continue; }
        if (strcmp(a, "--") == 0) { only_pos = 1; continue; }

        if (a[0] == '-' && a[1] == '-') {
            char* body = a + 2;
            char* eq = strchr(body, '=');
            char namebuf[64];
            const char* inline_val = NULL;
            if (eq) {
                size_t n = (size_t)(eq - body);
                if (n >= sizeof namebuf) n = sizeof namebuf - 1;
                memcpy(namebuf, body, n);
                namebuf[n] = '\0';
                inline_val = eq + 1;
                body = namebuf;
            }
            CliOpt* o = _find_long(c, body);
            if (!o) {
                c->ok = 0;
                snprintf(c->err, sizeof c->err, "unknown option --%s", body);
                return 0;
            }
            if (o->takes_value) {
                const char* v = inline_val;
                if (!v) {
                    if (i + 1 >= argc) {
                        c->ok = 0;
                        snprintf(c->err, sizeof c->err, "option --%s needs a value", body);
                        return 0;
                    }
                    v = argv[++i];
                }
                free(o->value);
                o->value = strdup(v);
            } else if (inline_val) {
                c->ok = 0;
                snprintf(c->err, sizeof c->err, "option --%s takes no value", body);
                return 0;
            }
            o->seen = 1;
            continue;
        }

        if (a[0] == '-' && a[1] != '\0') {
            // Short flags, bundled: -abc is -a -b -c. A value-taking short
            // option consumes the rest of the bundle or the next argument.
            for (int64_t k = 1; a[k]; k++) {
                CliOpt* o = _find_short(c, a[k]);
                if (!o) {
                    c->ok = 0;
                    snprintf(c->err, sizeof c->err, "unknown option -%c", a[k]);
                    return 0;
                }
                o->seen = 1;
                if (o->takes_value) {
                    const char* v;
                    if (a[k + 1]) { v = a + k + 1; }
                    else if (i + 1 < argc) { v = argv[++i]; }
                    else {
                        c->ok = 0;
                        snprintf(c->err, sizeof c->err, "option -%c needs a value", a[k]);
                        return 0;
                    }
                    free(o->value);
                    o->value = strdup(v);
                    break;
                }
            }
            continue;
        }

        _cli_push_pos(c, a);
    }
    return 1;
}

int64_t     flint_cli_ok(void* p)         { Cli* c = (Cli*)p; return c ? c->ok : 0; }
const char* flint_cli_error(void* p)      { Cli* c = (Cli*)p; return c ? c->err : ""; }
const char* flint_cli_get_str(void* p, const char* name) {
    Cli* c = (Cli*)p;
    CliOpt* o = c ? _find_long(c, name) : NULL;
    return (o && o->value) ? o->value : "";
}
int64_t flint_cli_has(void* p, const char* name) {
    Cli* c = (Cli*)p;
    CliOpt* o = c ? _find_long(c, name) : NULL;
    return o ? o->seen : 0;
}
int64_t flint_cli_get_i64(void* p, const char* name) {
    const char* s = flint_cli_get_str(p, name);
    return (s && *s) ? (int64_t)strtoll(s, NULL, 10) : 0;
}
const char* flint_cli_positional(void* p, int64_t i) {
    Cli* c = (Cli*)p;
    if (!c || i < 0 || i >= c->npos) return "";
    return c->pos[i];
}
char* flint_cli_usage(void* p) {
    Cli* c = (Cli*)p;
    if (!c) return NULL;
    size_t cap = 256 + (size_t)c->nopts * 48;
    char* out = (char*)calloc(1, cap);
    if (!out) return NULL;
    int n = snprintf(out, cap, "usage: %s [options]", c->prog);
    for (int64_t i = 0; i < c->nopts && n > 0 && (size_t)n < cap; i++) {
        CliOpt* o = &c->opts[i];
        if (o->shortname)
            n += snprintf(out + n, cap - (size_t)n, " [-%c|--%s%s]", o->shortname, o->name,
                          o->takes_value ? "=VALUE" : "");
        else
            n += snprintf(out + n, cap - (size_t)n, " [--%s%s]", o->name,
                          o->takes_value ? "=VALUE" : "");
    }
    if (n > 0 && (size_t)n < cap)
        snprintf(out + n, cap - (size_t)n, " [FILE...]");
    return out;
}
void flint_cli_free(void* p) {
    Cli* c = (Cli*)p;
    if (!c) return;
    for (int64_t i = 0; i < c->nopts; i++) free(c->opts[i].value);
    free(c->opts);
    free(c->pos);
    free(c);
}