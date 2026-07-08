#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <limits.h>
#include <math.h>
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <sys/wait.h>
#include <regex.h>

// CLI argument globals (set by compiler-generated code in main())
int64_t flint_g_argc = 0;
char** flint_g_argv = NULL;

void flint_print_i64(int64_t val) {
    printf("%ld", (long)val);
}

void flint_println_i64(int64_t val) {
    printf("%ld\n", (long)val);
}

void flint_println_f64(double val) {
    printf("%f\n", val);
}

void flint_print_str(const char* s) {
    // printf("%s", NULL) is undefined behavior; guard it.
    printf("%s", s ? s : "(null)");
}

void flint_println_str(const char* s) {
    printf("%s\n", s ? s : "(null)");
}

// NOTE: `fmt` must be a trusted compile-time literal produced by the code
// generator, never untrusted user input (format-string vulnerability).
void flint_print_fmt(const char* fmt, ...)
    __attribute__((format(printf, 1, 2)));
void flint_print_fmt(const char* fmt, ...) {
    if (!fmt) return;
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}

void flint_panic(const char* msg) {
    fprintf(stderr, "PANIC: %s\n", msg ? msg : "(null)");
    abort();
}

void flint_bounds_check(int64_t index, int64_t length) {
    if (index < 0 || index >= length) {
        fprintf(stderr, "PANIC: index %ld out of bounds for length %ld\n",
                (long)index, (long)length);
        abort();
    }
}

// Free a heap string returned by any flint_str_* / flint_i64_to_string /
// flint_file_read function. Callers own the returned memory.
void flint_str_free(char* s) {
    free(s);
}

// ---- Self-hosting runtime ----

int64_t flint_arg_count(void) {
    return flint_g_argc;
}

char* flint_get_arg(int64_t i) {
    if (i < 0 || i >= flint_g_argc || !flint_g_argv) return NULL;
    return flint_g_argv[i];
}

char* flint_str_concat(const char* a, const char* b) {
    if (!a) a = "";
    if (!b) b = "";
    size_t la = strlen(a);
    size_t lb = strlen(b);
    // Overflow guard: la + lb + 1 must not wrap.
    if (la > SIZE_MAX - lb - 1) return NULL;
    char* r = (char*)malloc(la + lb + 1);
    if (!r) return NULL;
    memcpy(r, a, la);
    memcpy(r + la, b, lb);
    r[la + lb] = '\0';
    return r;
}

int64_t flint_str_compare(const char* a, const char* b) {
    if (!a) a = "";
    if (!b) b = "";
    return (int64_t)strcmp(a, b);
}

int64_t flint_str_length(const char* s) {
    return s ? (int64_t)strlen(s) : 0;
}

int64_t flint_str_char_at(const char* s, int64_t i) {
    if (!s) flint_panic("str_char_at: null string");
    int64_t len = (int64_t)strlen(s);
    flint_bounds_check(i, len);
    return (unsigned char)s[i];
}

char* flint_str_substring(const char* s, int64_t start, int64_t end) {
    if (!s) return NULL;
    int64_t slen = (int64_t)strlen(s);
    // Validate ordering and bounds — reject inverted/out-of-range slices.
    if (start < 0 || end < start || end > slen) {
        flint_panic("str_substring: invalid range");
    }
    int64_t len = end - start;               // guaranteed >= 0
    char* r = (char*)malloc((size_t)len + 1);
    if (!r) return NULL;
    memcpy(r, s + start, (size_t)len);
    r[len] = '\0';
    return r;
}

char* flint_i64_to_string(int64_t n) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", (long long)n);
    return strdup(buf); // may return NULL on OOM; caller may check
}

char* flint_file_read(const char* path) {
    if (!path) return NULL;
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long len = ftell(f);
    if (len < 0) { fclose(f); return NULL; }              // ftell error / unseekable
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    if ((unsigned long)len >= SIZE_MAX) { fclose(f); return NULL; } // overflow guard

    char* content = (char*)malloc((size_t)len + 1);
    if (!content) { fclose(f); return NULL; }

    size_t n = fread(content, 1, (size_t)len, f);
    if (ferror(f)) { free(content); fclose(f); return NULL; }
    content[n] = '\0';   // terminate at actual bytes read
    fclose(f);
    return content;
}

#include <time.h>
#include <unistd.h>

// Returns a monotonic timestamp in nanoseconds (for benchmarking).
int64_t flint_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + (int64_t)ts.tv_nsec;
}

// Return FILE* for standard streams (as void* for Flint's str type)
void* flint_stdin(void) { return stdin; }
void* flint_stdout(void) { return stdout; }
void* flint_stderr(void) { return stderr; }

// Safe print using write() with file descriptors (avoids FILE* pointer issues)
void flint_print(const char* s) {
    if (!s) return;
    size_t len = strlen(s);
    write(STDOUT_FILENO, s, len);
}

void flint_println(const char* s) {
    if (!s) return;
    size_t len = strlen(s);
    write(STDOUT_FILENO, s, len);
    write(STDOUT_FILENO, "\n", 1);
}

void* flint_malloc(int64_t size) {
    if (size <= 0) return NULL;
    return malloc((size_t)size);
}

void flint_free(void* ptr) {
    free(ptr);
}

int64_t flint_ptr_to_int(void* p) {
    return (int64_t)(intptr_t)p;
}

void* flint_int_to_ptr(int64_t i) {
    return (void*)(intptr_t)i;
}

int64_t flint_array_read_i64(void* ptr, int64_t index) {
    return ((int64_t*)ptr)[index];
}

void flint_array_write_i64(void* ptr, int64_t index, int64_t value) {
    ((int64_t*)ptr)[index] = value;
}

typedef struct { void* data; int64_t len; } FlintArray;

FlintArray flint_array_alloc(int64_t n) {
    FlintArray a;
    a.data = (n > 0) ? malloc((size_t)(n * sizeof(int64_t))) : NULL;
    a.len = n;
    return a;
}

void flint_array_free(FlintArray a) {
    free(a.data);
}

int64_t flint_array_get(FlintArray a, int64_t index) {
    if (!a.data) return 0;
    if (index < 0 || index >= a.len) return 0;
    return ((int64_t*)a.data)[index];
}

void flint_array_set(FlintArray a, int64_t index, int64_t value) {
    if (!a.data) return;
    if (index >= 0 && index < a.len)
        ((int64_t*)a.data)[index] = value;
}

void flint_null_check(void* ptr, const char* msg) {
    if (!ptr) {
        fprintf(stderr, "NULL POINTER: %s\n", msg ? msg : "null pointer dereference");
        exit(1);
    }
}

FlintArray flint_array_concat(FlintArray a, FlintArray b) {
    FlintArray result;
    result.len = a.len + b.len;
    result.data = malloc((size_t)result.len * sizeof(int64_t));
    if (!result.data) { result.len = 0; return result; }
    if (a.len > 0) memcpy(result.data, a.data, (size_t)a.len * sizeof(int64_t));
    if (b.len > 0) memcpy((int64_t*)result.data + a.len, b.data, (size_t)b.len * sizeof(int64_t));
    return result;
}

// ---- Threading ----
#include <pthread.h>
#include <unistd.h>

#define FLINT_MAX_THREADS 128
static pthread_t flint_thread_pool[FLINT_MAX_THREADS];
static int flint_thread_count = 0;

int64_t flint_thread_create(void* func, int64_t arg) {
    if (flint_thread_count >= FLINT_MAX_THREADS) return -1;
    pthread_t thread;
    if (pthread_create(&thread, NULL, (void*(*)(void*))func, (void*)(intptr_t)arg) != 0)
        return -1;
    flint_thread_pool[flint_thread_count++] = thread;
    return (int64_t)(flint_thread_count - 1);
}

int64_t flint_thread_join(int64_t tid) {
    if (tid < 0 || tid >= flint_thread_count) return -1;
    void* retval;
    if (pthread_join(flint_thread_pool[tid], &retval) != 0)
        return -1;
    return (int64_t)(intptr_t)retval;
}

// ---- Parallel For (thread pool) ----
typedef struct {
    void* func;          // Flint function pointer (i64(i64))
    int64_t total;       // total iterations N
    int64_t id;          // thread ID (0..num_threads-1)
    int64_t num_threads; // total thread count
} ParallelCtx;

static void* parallel_worker(void* arg) {
    ParallelCtx* pc = (ParallelCtx*)arg;
    int64_t chunk = (pc->total + pc->num_threads - 1) / pc->num_threads;
    int64_t start = pc->id * chunk;
    int64_t end = start + chunk;
    if (end > pc->total) end = pc->total;
    void (*fn)(int64_t) = (void (*)(int64_t))pc->func;
    for (int64_t i = start; i < end; i++)
        fn(i);
    return NULL;
}

// Run a Flint function over range [0, n) in parallel.
// fn must have signature i64(i64) — called as fn(iteration_index).
int64_t flint_parallel_for(int64_t n, void* fn, int64_t num_threads) {
    if (num_threads < 1) {
        long hw = sysconf(_SC_NPROCESSORS_ONLN);
        num_threads = hw > 0 ? hw : 4;
    }
    if (num_threads > FLINT_MAX_THREADS) num_threads = FLINT_MAX_THREADS;
    if (num_threads > n) num_threads = n;

    pthread_t threads[FLINT_MAX_THREADS];
    ParallelCtx ctxs[FLINT_MAX_THREADS];
    for (int t = 0; t < num_threads; t++) {
        ctxs[t] = (ParallelCtx){fn, n, t, num_threads};
        pthread_create(&threads[t], NULL, parallel_worker, &ctxs[t]);
    }
    for (int t = 0; t < num_threads; t++)
        pthread_join(threads[t], NULL);
    return 0;
}

// Returns 0 on success, -1 on failure.
int flint_file_write(const char* path, const char* content) {
    if (!path) return -1;
    if (!content) content = "";
    FILE* f = fopen(path, "w");
    if (!f) return -1;
    int ok = (fputs(content, f) >= 0);
    if (fclose(f) != 0) ok = 0;   // flush errors surface at close
    return ok ? 0 : -1;
}

// ==================== ERROR STATE ====================

int64_t flint_g_err = 0;
int64_t flint_err_occurred(void) { int64_t e = flint_g_err; flint_g_err = 0; return e; }
void flint_set_err(int64_t err) { flint_g_err = err; }

// ==================== MATHEMATICS ====================

double flint_sqrt(double x) { return sqrt(x); }
double flint_pow(double x, double y) { return pow(x, y); }
double flint_abs_f64(double x) { return fabs(x); }
int64_t flint_abs_i64(int64_t x) { return x < 0 ? -x : x; }
double flint_min_f64(double a, double b) { return a < b ? a : b; }
int64_t flint_min_i64(int64_t a, int64_t b) { return a < b ? a : b; }
double flint_max_f64(double a, double b) { return a > b ? a : b; }
int64_t flint_max_i64(int64_t a, int64_t b) { return a > b ? a : b; }
double flint_floor(double x) { return floor(x); }
double flint_ceil(double x) { return ceil(x); }
double flint_round(double x) { return round(x); }
double flint_sin(double x) { return sin(x); }
double flint_cos(double x) { return cos(x); }
double flint_tan(double x) { return tan(x); }
double flint_asin(double x) { return asin(x); }
double flint_acos(double x) { return acos(x); }
double flint_atan(double x) { return atan(x); }
double flint_atan2(double y, double x) { return atan2(y, x); }
double flint_log(double x) { return log(x); }
double flint_log10(double x) { return log10(x); }
double flint_exp(double x) { return exp(x); }
double flint_sinh(double x) { return sinh(x); }
double flint_cosh(double x) { return cosh(x); }
double flint_tanh(double x) { return tanh(x); }

// ==================== PRNG ====================

static uint64_t flint_rng_state = 0;

void flint_srand(uint64_t seed) { flint_rng_state = seed ? seed : 1; }

static uint64_t flint_rand_u64(void) {
    uint64_t x = flint_rng_state;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    flint_rng_state = x;
    return x * 0x2545F4914F6CDD1DULL;
}

double flint_rand_f64(void) {
    return (double)(flint_rand_u64() >> 11) * (1.0 / 9007199254740992.0);
}

int64_t flint_rand_i64_range(int64_t lo, int64_t hi) {
    if (hi <= lo) return lo;
    uint64_t range = (uint64_t)(hi - lo);
    return lo + (int64_t)(flint_rand_u64() % range);
}

// ==================== TYPE CONVERSION ====================

char* flint_f64_to_string(double v) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%g", v);
    return strdup(buf);
}

int64_t flint_str_to_i64(const char* s) {
    if (!s) { flint_set_err(1); return 0; }
    char* end;
    errno = 0;
    long long v = strtoll(s, &end, 10);
    if (errno || *end != '\0') { flint_set_err(1); return 0; }
    return (int64_t)v;
}

double flint_str_to_f64(const char* s) {
    if (!s) { flint_set_err(1); return 0.0; }
    char* end;
    errno = 0;
    double v = strtod(s, &end);
    if (errno || *end != '\0') { flint_set_err(1); return 0.0; }
    return v;
}

int64_t flint_str_to_bool(const char* s) {
    if (!s) return 0;
    return (strcmp(s, "true") == 0 || strcmp(s, "1") == 0) ? 1 : 0;
}

double flint_i64_to_f64(int64_t v) { return (double)v; }

int64_t flint_f64_to_i64(double v) { return (int64_t)v; }

// ==================== ADDITIONAL STRING OPERATIONS ====================

char* flint_str_repeat(const char* s, int64_t n) {
    if (!s || n < 0) return NULL;
    int64_t slen = (int64_t)strlen(s);
    if (slen == 0 || n == 0) return strdup("");
    int64_t total = slen * n;
    if (total / n != slen) return NULL;
    char* r = (char*)malloc((size_t)total + 1);
    if (!r) return NULL;
    for (int64_t i = 0; i < n; i++)
        memcpy(r + i * slen, s, (size_t)slen);
    r[total] = '\0';
    return r;
}

char* flint_str_to_upper(const char* s) {
    if (!s) return NULL;
    char* r = strdup(s);
    if (!r) return NULL;
    for (char* p = r; *p; p++) *p = (char)toupper((unsigned char)*p);
    return r;
}

char* flint_str_to_lower(const char* s) {
    if (!s) return NULL;
    char* r = strdup(s);
    if (!r) return NULL;
    for (char* p = r; *p; p++) *p = (char)tolower((unsigned char)*p);
    return r;
}

char* flint_str_trim(const char* s) {
    if (!s) return strdup("");
    const char* start = s;
    while (isspace((unsigned char)*start)) start++;
    if (*start == '\0') return strdup("");
    const char* end = start + strlen(start) - 1;
    while (end > start && isspace((unsigned char)*end)) end--;
    int64_t len = end - start + 1;
    char* r = (char*)malloc((size_t)len + 1);
    if (!r) return NULL;
    memcpy(r, start, (size_t)len);
    r[len] = '\0';
    return r;
}

int64_t flint_str_index_of(const char* s, const char* sub) {
    if (!s || !sub) return -1;
    const char* p = strstr(s, sub);
    return p ? (int64_t)(p - s) : -1;
}

int64_t flint_str_last_index_of(const char* s, const char* sub) {
    if (!s || !sub) return -1;
    int64_t slen = (int64_t)strlen(s);
    int64_t sublen = (int64_t)strlen(sub);
    if (sublen == 0) return slen;
    for (int64_t i = slen - sublen; i >= 0; i--) {
        if (strncmp(s + i, sub, (size_t)sublen) == 0) return i;
    }
    return -1;
}

int64_t flint_str_starts_with(const char* s, const char* pre) {
    if (!s || !pre) return 0;
    return strncmp(s, pre, strlen(pre)) == 0 ? 1 : 0;
}

int64_t flint_str_ends_with(const char* s, const char* suf) {
    if (!s || !suf) return 0;
    int64_t slen = (int64_t)strlen(s);
    int64_t suflen = (int64_t)strlen(suf);
    if (suflen > slen) return 0;
    return strncmp(s + slen - suflen, suf, (size_t)suflen) == 0 ? 1 : 0;
}

char* flint_str_replace(const char* s, const char* old, const char* newstr) {
    if (!s || !old || !newstr) return NULL;
    int64_t slen = (int64_t)strlen(s);
    int64_t olen = (int64_t)strlen(old);
    int64_t nlen = (int64_t)strlen(newstr);
    if (olen == 0) return strdup(s);
    int64_t count = 0;
    const char* p = s;
    while ((p = strstr(p, old)) != NULL) { count++; p += olen; }
    int64_t reslen = slen + count * (nlen - olen);
    if (reslen < 0) return NULL;
    char* result = (char*)malloc((size_t)reslen + 1);
    if (!result) return NULL;
    char* dst = result;
    const char* src = s;
    const char* next;
    while ((next = strstr(src, old)) != NULL) {
        int64_t prelen = next - src;
        memcpy(dst, src, (size_t)prelen);
        dst += prelen;
        memcpy(dst, newstr, (size_t)nlen);
        dst += nlen;
        src = next + olen;
    }
    int64_t rem = slen - (src - s);
    memcpy(dst, src, (size_t)rem);
    dst[rem] = '\0';
    return result;
}

char* flint_str_join(char** parts, int64_t n, const char* sep) {
    if (!parts || n < 0) return NULL;
    if (n == 0) return strdup("");
    if (!sep) sep = "";
    int64_t seplen = (int64_t)strlen(sep);
    int64_t total = 0;
    for (int64_t i = 0; i < n; i++) {
        if (parts[i]) total += (int64_t)strlen(parts[i]);
    }
    total += seplen * (n - 1);
    char* r = (char*)malloc((size_t)total + 1);
    if (!r) return NULL;
    char* dst = r;
    for (int64_t i = 0; i < n; i++) {
        if (i > 0) { memcpy(dst, sep, (size_t)seplen); dst += seplen; }
        const char* src = parts[i] ? parts[i] : "";
        int64_t len = (int64_t)strlen(src);
        memcpy(dst, src, (size_t)len);
        dst += len;
    }
    *dst = '\0';
    return r;
}

char* flint_str_reverse(const char* s) {
    if (!s) return NULL;
    int64_t len = (int64_t)strlen(s);
    char* r = (char*)malloc((size_t)len + 1);
    if (!r) return NULL;
    for (int64_t i = 0; i < len; i++)
        r[i] = s[len - 1 - i];
    r[len] = '\0';
    return r;
}

int64_t flint_str_is_ascii(const char* s) {
    if (!s) return 1;
    for (; *s; s++) { if ((unsigned char)*s >= 128) return 0; }
    return 1;
}

int64_t flint_str_codepoint_at(const char* s, int64_t i) {
    if (!s) return -1;
    int64_t len = (int64_t)strlen(s);
    if (i < 0 || i >= len) return -1;
    unsigned char c = (unsigned char)s[i];
    if (c < 0x80) return c;
    int n;
    uint32_t cp;
    if ((c & 0xE0) == 0xC0) { n = 2; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { n = 3; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { n = 4; cp = c & 0x07; }
    else return -1;
    if (i + n > len) return -1;
    for (int j = 1; j < n; j++) {
        unsigned char b = (unsigned char)s[i + j];
        if ((b & 0xC0) != 0x80) return -1;
        cp = (cp << 6) | (b & 0x3F);
    }
    return (int64_t)cp;
}

// ==================== COLLECTIONS — StrBuilder ====================

typedef struct { char* data; int64_t len; int64_t cap; } FlintStrBuilder;

FlintStrBuilder* flint_sb_new(void) {
    FlintStrBuilder* sb = (FlintStrBuilder*)malloc(sizeof(FlintStrBuilder));
    if (!sb) return NULL;
    sb->data = NULL; sb->len = 0; sb->cap = 0;
    return sb;
}

void flint_sb_free(FlintStrBuilder* sb) {
    if (!sb) return; free(sb->data); free(sb);
}

void flint_sb_append(FlintStrBuilder* sb, const char* s) {
    if (!sb || !s) return;
    int64_t slen = (int64_t)strlen(s);
    if (sb->len + slen > sb->cap) {
        int64_t newCap = sb->cap ? sb->cap * 2 : 64;
        while (newCap < sb->len + slen) newCap *= 2;
        char* nd = (char*)realloc(sb->data, (size_t)newCap);
        if (!nd) { flint_panic("sb_append: OOM"); return; }
        sb->data = nd; sb->cap = newCap;
    }
    memcpy(sb->data + sb->len, s, (size_t)slen);
    sb->len += slen;
}

void flint_sb_append_char(FlintStrBuilder* sb, char c) {
    if (!sb) return;
    char buf[2] = {c, '\0'};
    flint_sb_append(sb, buf);
}

void flint_sb_append_i64(FlintStrBuilder* sb, int64_t v) {
    if (!sb) return;
    char buf[32]; snprintf(buf, sizeof(buf), "%lld", (long long)v);
    flint_sb_append(sb, buf);
}

void flint_sb_append_f64(FlintStrBuilder* sb, double v) {
    if (!sb) return;
    char buf[64]; snprintf(buf, sizeof(buf), "%g", v);
    flint_sb_append(sb, buf);
}

int64_t flint_sb_len(FlintStrBuilder* sb) { return sb ? sb->len : 0; }

char* flint_sb_build(FlintStrBuilder* sb) {
    if (!sb) return strdup("");
    char* r = (char*)malloc((size_t)sb->len + 1);
    if (!r) return NULL;
    memcpy(r, sb->data, (size_t)sb->len);
    r[sb->len] = '\0';
    return r;
}

// ==================== COLLECTIONS — Vec ====================

typedef struct { int64_t* data; int64_t len; int64_t cap; } FlintVec;

FlintVec* flint_vec_new(void) {
    FlintVec* v = (FlintVec*)malloc(sizeof(FlintVec));
    if (!v) return NULL;
    v->data = NULL; v->len = 0; v->cap = 0;
    return v;
}

void flint_vec_free(FlintVec* v) {
    if (!v) return; free(v->data); free(v);
}

int64_t flint_vec_len(FlintVec* v) { return v ? v->len : 0; }

int64_t flint_vec_cap(FlintVec* v) { return v ? v->cap : 0; }

int64_t flint_vec_get(FlintVec* v, int64_t i) {
    if (!v) flint_panic("vec_get: null vector");
    if (i < 0 || i >= v->len) flint_panic("vec_get: index out of bounds");
    return v->data[i];
}

void flint_vec_set(FlintVec* v, int64_t i, int64_t val) {
    if (!v) flint_panic("vec_set: null vector");
    if (i < 0 || i >= v->len) flint_panic("vec_set: index out of bounds");
    v->data[i] = val;
}

void flint_vec_push(FlintVec* v, int64_t val) {
    if (!v) return;
    if (v->len >= v->cap) {
        int64_t newCap = v->cap ? v->cap * 2 : 8;
        int64_t* nd = (int64_t*)realloc(v->data, (size_t)(newCap * sizeof(int64_t)));
        if (!nd) { flint_panic("vec_push: OOM"); return; }
        v->data = nd; v->cap = newCap;
    }
    v->data[v->len++] = val;
}

int64_t flint_vec_pop(FlintVec* v) {
    if (!v) flint_panic("vec_pop: null vector");
    if (v->len <= 0) flint_panic("vec_pop: empty vector");
    return v->data[--v->len];
}

void flint_vec_clear(FlintVec* v) {
    if (v) v->len = 0;
}

// ==================== COLLECTIONS — Map ====================

static uint64_t fnv1a(const char* s) {
    uint64_t h = 14695981039346656037ULL;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ULL; }
    return h;
}

typedef struct { char** keys; int64_t* values; int64_t cap; int64_t len; } FlintMap;

FlintMap* flint_map_new(void) {
    FlintMap* m = (FlintMap*)malloc(sizeof(FlintMap));
    if (!m) return NULL;
    m->cap = 16;
    m->len = 0;
    m->keys = (char**)calloc((size_t)m->cap, sizeof(char*));
    m->values = (int64_t*)calloc((size_t)m->cap, sizeof(int64_t));
    if (!m->keys || !m->values) { free(m->keys); free(m->values); free(m); return NULL; }
    return m;
}

void flint_map_free(FlintMap* m) {
    if (!m) return;
    for (int64_t i = 0; i < m->cap; i++) {
        if (m->keys[i]) { free(m->keys[i]); m->keys[i] = NULL; }
    }
    free(m->keys); free(m->values); free(m);
}

static int64_t map_probe(FlintMap* m, const char* key, int64_t* slot) {
    uint64_t h = fnv1a(key);
    int64_t idx = (int64_t)(h % (uint64_t)m->cap);
    while (m->keys[idx] != NULL) {
        if (strcmp(m->keys[idx], key) == 0) {
            *slot = idx;
            return 1;
        }
        idx = (idx + 1) % m->cap;
    }
    *slot = idx;
    return 0;
}

int64_t flint_map_has(FlintMap* m, const char* key) {
    if (!m || !key) return 0;
    int64_t slot;
    return map_probe(m, key, &slot);
}

int64_t flint_map_get(FlintMap* m, const char* key) {
    if (!m || !key) { flint_set_err(1); return 0; }
    int64_t slot;
    if (map_probe(m, key, &slot)) return m->values[slot];
    flint_set_err(1);
    return 0;
}

static void map_grow(FlintMap* m) {
    int64_t oldCap = m->cap;
    char** oldKeys = m->keys;
    int64_t* oldVals = m->values;
    m->cap = oldCap * 2;
    m->keys = (char**)calloc((size_t)m->cap, sizeof(char*));
    m->values = (int64_t*)calloc((size_t)m->cap, sizeof(int64_t));
    if (!m->keys || !m->values) { flint_panic("map_grow: OOM"); return; }
    m->len = 0;
    for (int64_t i = 0; i < oldCap; i++) {
        if (oldKeys[i]) {
            int64_t slot;
            map_probe(m, oldKeys[i], &slot);
            m->keys[slot] = oldKeys[i];
            m->values[slot] = oldVals[i];
            m->len++;
        }
    }
    free(oldKeys); free(oldVals);
}

void flint_map_set(FlintMap* m, const char* key, int64_t val) {
    if (!m || !key) return;
    if ((double)(m->len + 1) > (double)m->cap * 0.7) map_grow(m);
    int64_t slot;
    if (map_probe(m, key, &slot)) {
        m->values[slot] = val;
    } else {
        m->keys[slot] = strdup(key);
        m->values[slot] = val;
        m->len++;
    }
}

int64_t flint_map_len(FlintMap* m) { return m ? m->len : 0; }

char* flint_map_keys(FlintMap* m) {
    if (!m) return strdup("");
    FlintStrBuilder* sb = flint_sb_new();
    if (!sb) return NULL;
    for (int64_t i = 0; i < m->cap; i++) {
        if (m->keys[i]) {
            if (sb->len > 0) flint_sb_append_char(sb, '\n');
            flint_sb_append(sb, m->keys[i]);
        }
    }
    char* result = flint_sb_build(sb);
    flint_sb_free(sb);
    return result;
}

// ==================== SYSTEM & OS ====================

char* flint_getenv(const char* name) {
    if (!name) return NULL;
    char* val = getenv(name);
    return val ? strdup(val) : NULL;
}

void flint_exit(int64_t code) {
    exit((int)code);
}

void flint_sleep_ms(int64_t ms) {
    if (ms <= 0) return;
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

int64_t flint_mkdir(const char* path) {
    if (!path) return -1;
    return mkdir(path, 0755) == 0 ? 0 : -1;
}

int64_t flint_rmdir(const char* path) {
    if (!path) return -1;
    return rmdir(path) == 0 ? 0 : -1;
}

int64_t flint_remove(const char* path) {
    if (!path) return -1;
    return unlink(path) == 0 ? 0 : -1;
}

int64_t flint_exists(const char* path) {
    if (!path) return 0;
    struct stat st;
    return stat(path, &st) == 0 ? 1 : 0;
}

int64_t flint_is_dir(const char* path) {
    if (!path) return 0;
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISDIR(st.st_mode) ? 1 : 0;
}

int64_t flint_is_file(const char* path) {
    if (!path) return 0;
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISREG(st.st_mode) ? 1 : 0;
}

int64_t flint_file_size(const char* path) {
    if (!path) return -1;
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (int64_t)st.st_size;
}

char* flint_listdir(const char* path) {
    if (!path) return NULL;
    DIR* dir = opendir(path);
    if (!dir) return NULL;
    FlintStrBuilder* sb = flint_sb_new();
    if (!sb) { closedir(dir); return NULL; }
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (sb->len > 0) flint_sb_append_char(sb, '\n');
        flint_sb_append(sb, entry->d_name);
    }
    closedir(dir);
    char* result = flint_sb_build(sb);
    flint_sb_free(sb);
    return result;
}

char* flint_cwd(void) {
    char* buf = getcwd(NULL, 0);
    return buf;
}

int64_t flint_chdir(const char* path) {
    if (!path) return -1;
    return chdir(path) == 0 ? 0 : -1;
}

// ==================== FILE I/O ====================

char* flint_read_file(const char* path) {
    if (!path) { flint_set_err(1); return NULL; }
    FILE* f = fopen(path, "rb");
    if (!f) { flint_set_err(1); return NULL; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    if (len < 0) { fclose(f); flint_set_err(1); return NULL; }
    rewind(f);
    char* buf = (char*)malloc((size_t)(len + 1));
    if (!buf) { fclose(f); flint_set_err(1); return NULL; }
    size_t n = fread(buf, 1, (size_t)len, f);
    if (ferror(f)) { fclose(f); free(buf); flint_set_err(1); return NULL; }
    buf[n] = '\0';
    fclose(f);
    return buf;
}

int64_t flint_write_file(const char* path, const char* data) {
    if (!path || !data) { flint_set_err(1); return -1; }
    FILE* f = fopen(path, "wb");
    if (!f) { flint_set_err(1); return -1; }
    size_t len = strlen(data);
    if (fwrite(data, 1, len, f) != len) { fclose(f); flint_set_err(1); return -1; }
    fclose(f);
    return 0;
}

int64_t flint_append_file(const char* path, const char* data) {
    if (!path || !data) { flint_set_err(1); return -1; }
    FILE* f = fopen(path, "ab");
    if (!f) { flint_set_err(1); return -1; }
    size_t len = strlen(data);
    if (fwrite(data, 1, len, f) != len) { fclose(f); flint_set_err(1); return -1; }
    fclose(f);
    return 0;
}

int64_t flint_file_copy(const char* src, const char* dst) {
    if (!src || !dst) { flint_set_err(1); return -1; }
    char* data = flint_read_file(src);
    if (!data) return -1;
    int64_t ret = flint_write_file(dst, data);
    free(data);
    return ret;
}

char* flint_temp_dir(void) {
    const char* tmp = getenv("TMPDIR");
    if (!tmp) tmp = getenv("TEMP");
    if (!tmp) tmp = "/tmp";
    char* result = strdup(tmp);
    return result;
}

// ==================== PROCESS SPAWNING ====================

int64_t flint_command(const char* cmd) {
    if (!cmd) { flint_set_err(1); return -1; }
    int ret = system(cmd);
    if (ret == -1) { flint_set_err(1); return -1; }
    return (int64_t)WEXITSTATUS(ret);
}

char* flint_command_output(const char* cmd) {
    if (!cmd) { flint_set_err(1); return NULL; }
    FILE* pipe = popen(cmd, "r");
    if (!pipe) { flint_set_err(1); return NULL; }
    FlintStrBuilder* sb = flint_sb_new();
    if (!sb) { pclose(pipe); return NULL; }
    char buf[4096];
    while (fgets(buf, sizeof(buf), pipe)) {
        flint_sb_append(sb, buf);
    }
    int rc = pclose(pipe);
    if (rc == -1) { flint_sb_free(sb); flint_set_err(1); return NULL; }
    // Trim trailing newline
    char* result = flint_sb_build(sb);
    flint_sb_free(sb);
    if (result) {
        size_t len = strlen(result);
        while (len > 0 && result[len - 1] == '\n') result[--len] = '\0';
    }
    return result;
}

// ==================== REGEX ====================

int64_t flint_regex_match(const char* pattern, const char* str) {
    if (!pattern || !str) { flint_set_err(1); return 0; }
    regex_t re;
    int rc = regcomp(&re, pattern, REG_EXTENDED);
    if (rc != 0) { flint_set_err(1); return 0; }
    rc = regexec(&re, str, 0, NULL, 0);
    regfree(&re);
    return rc == 0 ? 1 : 0;
}

char* flint_regex_replace(const char* pattern, const char* repl, const char* str) {
    if (!pattern || !repl || !str) { flint_set_err(1); return NULL; }
    regex_t re;
    int rc = regcomp(&re, pattern, REG_EXTENDED);
    if (rc != 0) { flint_set_err(1); return NULL; }
    regmatch_t pm;
    const char* p = str;
    FlintStrBuilder* sb = flint_sb_new();
    if (!sb) { regfree(&re); return NULL; }
    while (regexec(&re, p, 1, &pm, 0) == 0) {
        if (pm.rm_so > 0) {
            char* seg = strndup(p, (size_t)pm.rm_so);
            if (seg) { flint_sb_append(sb, seg); free(seg); }
        }
        flint_sb_append(sb, repl);
        p += pm.rm_eo;
    }
    flint_sb_append(sb, p);
    regfree(&re);
    char* result = flint_sb_build(sb);
    flint_sb_free(sb);
    return result;
}

// ==================== CSV ====================

char* flint_csv_parse_line(const char* line, int64_t col) {
    if (!line || col < 0) { flint_set_err(1); return NULL; }
    int64_t c = 0;
    const char* p = line;
    int in_quotes = 0;
    FlintStrBuilder* sb = flint_sb_new();
    if (!sb) return NULL;
    while (*p) {
        if (*p == '"') { in_quotes = !in_quotes; p++; continue; }
        if (*p == ',' && !in_quotes) {
            if (c == col) { char* r = flint_sb_build(sb); flint_sb_free(sb); return r; }
            c++; flint_sb_free(sb); sb = flint_sb_new(); if (!sb) return NULL;
            p++; continue;
        }
        char ch[2] = { *p, '\0' };
        flint_sb_append(sb, ch);
        p++;
    }
    char* result = (c == col) ? flint_sb_build(sb) : NULL;
    flint_sb_free(sb);
    if (c != col) { flint_set_err(1); return NULL; }
    return result;
}

// ==================== JSON BUILDER ====================

char* flint_json_build_object(const char* key1, const char* val1, int64_t has_val2, const char* key2, const char* val2) {
    FlintStrBuilder* sb = flint_sb_new();
    if (!sb) return NULL;
    flint_sb_append(sb, "{");
    if (key1) {
        flint_sb_append(sb, "\""); flint_sb_append(sb, key1);
        flint_sb_append(sb, "\":\""); flint_sb_append(sb, val1 ? val1 : ""); flint_sb_append(sb, "\"");
    }
    if (has_val2 && key2) {
        flint_sb_append(sb, ",\"");
        flint_sb_append(sb, key2); flint_sb_append(sb, "\":\"");
        flint_sb_append(sb, val2 ? val2 : ""); flint_sb_append(sb, "\"");
    }
    flint_sb_append(sb, "}");
    char* r = flint_sb_build(sb);
    flint_sb_free(sb);
    return r;
}

char* flint_json_build_array(const char* items) {
    FlintStrBuilder* sb = flint_sb_new();
    if (!sb) return NULL;
    flint_sb_append(sb, "[");
    flint_sb_append(sb, items ? items : "");
    flint_sb_append(sb, "]");
    char* r = flint_sb_build(sb);
    flint_sb_free(sb);
    return r;
}

// ==================== READ LINES ====================

char* flint_read_lines(const char* path) {
    char* content = flint_read_file(path);
    if (!content) return NULL;
    return content; // lines are separated by \n, same as file content
}

// ==================== FILE MOVE ====================

int64_t flint_file_move(const char* src, const char* dst) {
    if (!src || !dst) { flint_set_err(1); return -1; }
    if (rename(src, dst) != 0) { flint_set_err(1); return -1; }
    return 0;
}

// ==================== ARRAY SLICE ====================

FlintVec* flint_array_slice(int64_t* data, int64_t len, int64_t start, int64_t end) {
    if (!data) { flint_set_err(1); return NULL; }
    if (start < 0) start = 0;
    if (end > len) end = len;
    if (start >= end) return flint_vec_new();
    FlintVec* v = flint_vec_new();
    if (!v) return NULL;
    for (int64_t i = start; i < end; i++) {
        flint_vec_push(v, data[i]);
    }
    return v;
}

// ==================== DATE & TIME ====================

int64_t flint_time_now(void) {
    return (int64_t)time(NULL);
}

int64_t flint_time_ns_monotonic(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + (int64_t)ts.tv_nsec;
}

char* flint_time_format(const char* fmt) {
    if (!fmt) return NULL;
    time_t now = time(NULL);
    struct tm result;
    localtime_r(&now, &result);
    char buf[256];
    if (strftime(buf, sizeof(buf), fmt, &result) == 0) return NULL;
    return strdup(buf);
}

// ==================== TESTING ====================

void flint_assert(int64_t cond, const char* msg) {
    if (!cond) {
        fprintf(stderr, "ASSERTION FAILED: %s\n", msg ? msg : "");
        abort();
    }
}

void flint_assert_eq_i64(int64_t a, int64_t b, const char* msg) {
    if (a != b) {
        fprintf(stderr, "ASSERTION FAILED: %s — expected %lld, got %lld\n",
                msg ? msg : "", (long long)a, (long long)b);
        abort();
    }
}

void flint_assert_eq_f64(double a, double b, double eps, const char* msg) {
    if (fabs(a - b) > eps) {
        fprintf(stderr, "ASSERTION FAILED: %s — expected %g, got %g (eps %g)\n",
                msg ? msg : "", a, b, eps);
        abort();
    }
}

void flint_assert_eq_str(const char* a, const char* b, const char* msg) {
    if (strcmp(a ? a : "", b ? b : "") != 0) {
        fprintf(stderr, "ASSERTION FAILED: %s — expected \"%s\", got \"%s\"\n",
                msg ? msg : "", a ? a : "", b ? b : "");
        abort();
    }
}

int64_t flint_test_run(const char* name, int64_t (*fn)(void)) {
    printf("TEST %s...", name ? name : "");
    fflush(stdout);
    int64_t result = fn();
    if (result == 0) {
        printf("OK\n");
        return 0;
    } else {
        printf("FAIL\n");
        return 1;
    }
}

// Unwrap: returns value if tag != 0, panics otherwise
// Usage: flint_unwrap(tag, value) — for i64 payloads
int64_t flint_unwrap(int64_t tag, int64_t value) {
    if (tag == 0) {
        fprintf(stderr, "panic: unwrap of None/Err\n");
        exit(1);
    }
    return value;
}

// Unwrap string: returns string ptr if tag != 0, panics otherwise
const char* flint_unwrap_str(int64_t tag, const char* value) {
    if (tag == 0) {
        fprintf(stderr, "panic: unwrap of None/Err\n");
        exit(1);
    }
    return value;
}

// Expect: panics with custom message if tag == 0
int64_t flint_expect(int64_t tag, int64_t value, const char* msg) {
    if (tag == 0) {
        fprintf(stderr, "panic: %s\n", msg ? msg : "expect failed");
        exit(1);
    }
    return value;
}
