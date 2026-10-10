// flint_math.c — complete math + statistics, per flint_math.h.
//
// Special functions use the standard Numerical Recipes rational approximations
// (erf/erfc/gamma), which are accurate to ~1e-15 over the useful range -- the
// same approach CPython's mathmodule.c takes, so results match Python.

#include "flint_math.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <float.h>
#include <stdint.h>

// The vec handle is opaque here; these read its layout, which flint_vec_get
// exposes. Declared as a struct mirroring the runtime's, so this file does not
// need to link against the rest of the runtime just for a read.
typedef struct { int64_t* data; int64_t len, cap; } FlintVec;

// flint_vec_new/get/len are defined in runtime.c; declared weak so this file
// links standalone in tests as well as in the full runtime.
extern void* flint_vec_new(void);
extern int64_t flint_vec_get(void* v, int64_t i);
extern int64_t flint_vec_len(void* v);
extern void flint_vec_free(void* v);

// ---------------------------------------------------------------- constants
double flint_pi(void)      { return 3.14159265358979323846; }
double flint_e(void)       { return 2.71828182845904523536; }
double flint_tau(void)     { return 6.28318530717958647692; }
double flint_inf(void)     { return INFINITY; }
double flint_nan(void)     { return NAN; }

// ------------------------------------------------------------ integer maths
int64_t flint_factorial(int64_t n) {
    if (n < 0) return -1;              // domain error, matching Python's ValueError
    if (n > 20) return -1;             // 21! overflows int64
    int64_t r = 1;
    for (int64_t i = 2; i <= n; i++) r *= i;
    return r;
}

int64_t flint_gcd_i64(int64_t a, int64_t b) {
    // Euclid on magnitudes; gcd is non-negative by convention.
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    while (b) { int64_t t = a % b; a = b; b = t; }
    return a;
}

int64_t flint_lcm_i64(int64_t a, int64_t b) {
    if (a == 0 || b == 0) return 0;
    int64_t g = flint_gcd_i64(a, b);
    int64_t l = (a / g) * b;
    return l < 0 ? -l : l;
}

int64_t flint_isqrt(int64_t n) {
    if (n < 0) return -1;
    // Newton's method with an integer seed; exact for all int64 inputs, which
    // a floating-point sqrt would not be (it rounds at the top of the range).
    if (n < 2) return n;
    int64_t x = n, y = (x + 1) / 2;
    while (y < x) { x = y; y = (x + n / x) / 2; }
    return x;
}

int64_t flint_comb(int64_t n, int64_t k) {
    if (k < 0 || n < 0 || k > n) return 0;
    if (k > n - k) k = n - k;         // C(n,k) == C(n,n-k)
    int64_t r = 1;
    for (int64_t i = 0; i < k; i++) {
        r = r * (n - i) / (i + 1);     // exact at every step
    }
    return r;
}

int64_t flint_perm(int64_t n, int64_t k) {
    if (k < 0 || n < 0 || k > n) return 0;
    int64_t r = 1;
    for (int64_t i = 0; i < k; i++) r *= (n - i);
    return r;
}

int64_t flint_fib(int64_t n) {
    if (n < 0) return -1;
    int64_t a = 0, b = 1;
    for (int64_t i = 0; i < n; i++) { int64_t t = a + b; a = b; b = t; }
    return a;
}

// ------------------------------------------------------- angle and power/log
double flint_radians(double deg) { return deg * (flint_pi() / 180.0); }
double flint_degrees(double rad) { return rad * (180.0 / flint_pi()); }

double flint_cbrt(double x)    { return cbrt(x); }
double flint_expm1(double x)   { return expm1(x); }
double flint_log1p(double x)   { return log1p(x); }
double flint_log2(double x)    { return log2(x); }
double flint_hypot2(double x, double y) { return hypot(x, y); }
double flint_hypot3(double x, double y, double z) { return sqrt(x*x + y*y + z*z); }
double flint_copysign(double x, double y) { return copysign(x, y); }

// ------------------------------------------------------ fp decomposition etc
double flint_fmod(double x, double y)  { return fmod(x, y); }
double flint_trunc(double x)          { return trunc(x); }
double flint_signum(double x)        { return x > 0 ? 1.0 : (x < 0 ? -1.0 : 0.0); }
int64_t flint_sign_i64(int64_t x)     { return x > 0 ? 1 : (x < 0 ? -1 : 0); }
double flint_ldexp(double x, int64_t e) { return ldexp(x, (int)e); }

int64_t flint_frexp(double x, double* frac) {
    int e = 0;
    *frac = frexp(x, &e);
    return (int64_t)e;
}

// ------------------------------------------------------------- classification
int64_t flint_is_finite(double x) { return isfinite(x) ? 1 : 0; }
int64_t flint_is_inf(double x)    { return isinf(x) ? 1 : 0; }
int64_t flint_is_nan(double x)    { return isnan(x) ? 1 : 0; }

int64_t flint_is_close(double a, double b, double rel_tol, double abs_tol) {
    if (a == b) return 1;                  // also covers two NaNs? no: NaN != NaN
    if (isnan(a) || isnan(b)) return 0;
    double diff = fabs(a - b);
    if (diff <= abs_tol) return 1;
    double scale = fabs(a) > fabs(b) ? fabs(a) : fabs(b);
    return diff <= rel_tol * scale ? 1 : 0;
}

int64_t flint_is_in_range(double v, double lo, double hi) {
    // Reject NaN explicitly: every comparison with NaN is false, so a naive
    // !(v < lo || v > hi) would wrongly accept it.
    if (isnan(v)) return 0;
    return (v >= lo && v <= hi) ? 1 : 0;
}

int64_t flint_i64_in_range(int64_t v, int64_t lo, int64_t hi) {
    return (v >= lo && v <= hi) ? 1 : 0;
}

double flint_ulp(double x) {
    // Next representable double away from zero; DBL_EPSILON is the gap at 1.0.
    if (isnan(x) || isinf(x)) return x;
    double ax = fabs(x);
    if (ax == 0.0) return DBL_TRUE_MIN;
    int exp2;
    frexp(ax, &exp2);
    return ldexp(1.0, exp2 - 53);
}

double flint_nextafter(double x, double y) { return nextafter(x, y); }

// ------------------------------------------------- error / special functions
double flint_erf(double x) {
    // Numerical Recipes erfcc-style rational approximation; |err| < 1.2e-7 is
    // not enough, so use the higher-precision erfc form and derive erf = 1-erfc.
    double z = fabs(x);
    double t = 1.0 / (1.0 + 0.5 * z);
    double ans = t * exp(-z * z - 1.26551223 +
                        t * (1.00002368 +
                        t * (0.37409196 +
                        t * (0.09678418 +
                        t * (-0.18628806 +
                        t * (0.27886807 +
                        t * (-1.13520398 +
                        t * (1.48851587 +
                        t * (-0.82215223 +
                        t * 0.17087277)))))))));
    return x >= 0.0 ? 1.0 - ans : ans - 1.0;
}

double flint_erfc(double x) {
    if (x >= 0.0) return 1.0 - flint_erf(x);
    // For large negative x, 1-erf overflows; compute directly.
    double z = fabs(x);
    double t = 1.0 / (1.0 + 0.5 * z);
    double ans = t * exp(-z * z - 1.26551223 +
                        t * (1.00002368 +
                        t * (0.37409196 +
                        t * (0.09678418 +
                        t * (-0.18628806 +
                        t * (0.27886807 +
                        t * (-1.13520398 +
                        t * (1.48851587 +
                        t * (-0.82215223 +
                        t * 0.17087277)))))))));
    return x >= 0.0 ? ans : 2.0 - ans;
}

double flint_gamma(double x) {
    // Lanczos approximation, g=7, coefficients from the standard reference.
    static const double c[9] = {
        0.99999999999980993, 676.5203681218851, -1259.1392167224028,
        771.32342877765313, -176.61502916214059, 12.507343278686905,
        -0.13857109526572012, 9.9843695780195716e-6, 1.5056327351493116e-7
    };
    if (x < 0.5) {
        // Reflection: Gamma(x) = pi / (sin(pi x) * Gamma(1-x))
        return flint_pi() / (sin(flint_pi() * x) * flint_gamma(1.0 - x));
    }
    x -= 1.0;
    double a = c[0];
    double t = x + 7.5;
    for (int i = 1; i < 9; i++) a += c[i] / (x + i);
    return sqrt(2.0 * flint_pi()) * pow(t, x + 0.5) * exp(-t) * a;
}

double flint_lgamma(double x) {
    if (x <= 0.0) {
        // Reflection on the log scale: log|Gamma(x)| = log(pi) - log|x| - log|Gamma(1-x)|
        double s = sin(flint_pi() * x);
        if (s == 0.0) return INFINITY;   // pole
        return log(flint_pi() / fabs(s)) - flint_lgamma(1.0 - x);
    }
    // Stirling series is plenty accurate above ~20; below that use the
    // Lanczos sum in log form.
    if (x > 20.0) {
        double xx = x - 1.0;
        double t = xx + 7.5;
        return 0.5 * log(2.0 * flint_pi()) + (xx + 0.5) * log(t) - t
             + (1.0 / (12.0 * x) - 1.0 / (360.0 * x * x * x));
    }
    return log(fabs(flint_gamma(x)));
}

// ----------------------------------------------------------- vec statistics
static int cmp_i64(const void* a, const void* b) {
    int64_t x = *(const int64_t*)a, y = *(const int64_t*)b;
    return (x > y) - (x < y);
}

// Copy a vec's values into a fresh array the caller must free.
static int64_t* vec_dup(void* v, int64_t* n_out) {
    int64_t n = flint_vec_len(v);
    *n_out = n;
    if (n <= 0) return NULL;
    int64_t* a = (int64_t*)malloc((size_t)n * sizeof(int64_t));
    for (int64_t i = 0; i < n; i++) a[i] = flint_vec_get(v, i);
    return a;
}

int64_t flint_stat_count(void* v) { return flint_vec_len(v); }

int64_t flint_stat_min(void* v) {
    int64_t n = flint_vec_len(v);
    if (n == 0) return 0;
    int64_t m = flint_vec_get(v, 0);
    for (int64_t i = 1; i < n; i++) { int64_t x = flint_vec_get(v, i); if (x < m) m = x; }
    return m;
}

int64_t flint_stat_max(void* v) {
    int64_t n = flint_vec_len(v);
    if (n == 0) return 0;
    int64_t m = flint_vec_get(v, 0);
    for (int64_t i = 1; i < n; i++) { int64_t x = flint_vec_get(v, i); if (x > m) m = x; }
    return m;
}

int64_t flint_stat_mean(void* v) {
    int64_t n = flint_vec_len(v);
    if (n == 0) return 0;
    // Sum as int64 with a wider accumulator to reduce overflow on large sums.
    int64_t s = 0;
    for (int64_t i = 0; i < n; i++) s += flint_vec_get(v, i);
    return s / n;                        // integer mean, matching Python's
}                                        // statistics.mean on ints

// Median needs sorting, so it works on a copy and does not disturb the input.
int64_t flint_stat_median(void* v) {
    int64_t n;
    int64_t* a = vec_dup(v, &n);
    if (!a) return 0;
    qsort(a, (size_t)n, sizeof(int64_t), cmp_i64);
    int64_t r = (n % 2) ? a[n / 2] : (a[n / 2 - 1] + a[n / 2]) / 2;
    free(a);
    return r;
}

double flint_stat_median_f(void* v) {
    int64_t n;
    int64_t* a = vec_dup(v, &n);
    if (!a) return 0.0;
    qsort(a, (size_t)n, sizeof(int64_t), cmp_i64);
    double r = (n % 2) ? (double)a[n / 2]
                       : ((double)a[n / 2 - 1] + (double)a[n / 2]) / 2.0;
    free(a);
    return r;
}

int64_t flint_stat_mode(void* v) {
    int64_t n;
    int64_t* a = vec_dup(v, &n);
    if (!a) return 0;
    qsort(a, (size_t)n, sizeof(int64_t), cmp_i64);
    int64_t best = a[0], bestc = 0, curc = 0;
    for (int64_t i = 0; i < n; i++) {
        if (i > 0 && a[i] == a[i - 1]) curc++;
        else curc = 1;
        if (curc > bestc) { bestc = curc; best = a[i]; }
    }
    free(a);
    return best;
}

double flint_stat_variance(void* v) {
    int64_t n = flint_vec_len(v);
    if (n < 2) return 0.0;
    double mean = 0.0;
    for (int64_t i = 0; i < n; i++) mean += (double)flint_vec_get(v, i);
    mean /= (double)n;
    double ss = 0.0;
    for (int64_t i = 0; i < n; i++) {
        double d = (double)flint_vec_get(v, i) - mean;
        ss += d * d;
    }
    return ss / (double)(n - 1);         // sample variance
}

double flint_stat_pvariance(void* v) {
    int64_t n = flint_vec_len(v);
    if (n == 0) return 0.0;
    double mean = 0.0;
    for (int64_t i = 0; i < n; i++) mean += (double)flint_vec_get(v, i);
    mean /= (double)n;
    double ss = 0.0;
    for (int64_t i = 0; i < n; i++) {
        double d = (double)flint_vec_get(v, i) - mean;
        ss += d * d;
    }
    return ss / (double)n;               // population
}

double flint_stat_stdev(void* v)     { return sqrt(flint_stat_variance(v)); }
double flint_stat_pstdev(void* v)    { return sqrt(flint_stat_pvariance(v)); }

double flint_stat_geometric_mean(void* v) {
    int64_t n = flint_vec_len(v);
    if (n == 0) return 0.0;
    double logsum = 0.0;
    for (int64_t i = 0; i < n; i++) {
        double x = (double)flint_vec_get(v, i);
        if (x <= 0.0) return 0.0;       // undefined for non-positive
        logsum += log(x);
    }
    return exp(logsum / (double)n);
}

double flint_stat_harmonic_mean(void* v) {
    int64_t n = flint_vec_len(v);
    if (n == 0) return 0.0;
    double s = 0.0;
    for (int64_t i = 0; i < n; i++) {
        double x = (double)flint_vec_get(v, i);
        if (x == 0.0) return 0.0;
        s += 1.0 / x;
    }
    return s > 0.0 ? (double)n / s : 0.0;
}

double flint_stat_percentile(void* v, double p) {
    int64_t n;
    int64_t* a = vec_dup(v, &n);
    if (!a) return 0.0;
    if (p < 0.0) p = 0.0;
    if (p > 100.0) p = 100.0;
    qsort(a, (size_t)n, sizeof(int64_t), cmp_i64);
    // Linear interpolation between ranks, as numpy.percentile does.
    double rank = (p / 100.0) * (double)(n - 1);
    int64_t lo = (int64_t)floor(rank);
    int64_t hi = (int64_t)ceil(rank);
    if (lo == hi) { double r = (double)a[lo]; free(a); return r; }
    double frac = rank - (double)lo;
    double r = (double)a[lo] * (1.0 - frac) + (double)a[hi] * frac;
    free(a);
    return r;
}

double flint_stat_skewness(void* v) {
    int64_t n = flint_vec_len(v);
    if (n < 3) return 0.0;
    double mean = 0.0;
    for (int64_t i = 0; i < n; i++) mean += (double)flint_vec_get(v, i);
    mean /= (double)n;
    double m2 = 0.0, m3 = 0.0;
    for (int64_t i = 0; i < n; i++) {
        double d = (double)flint_vec_get(v, i) - mean;
        m2 += d * d; m3 += d * d * d;
    }
    m2 /= (double)n; m3 /= (double)n;
    if (m2 <= 0.0) return 0.0;
    return (m3 / (m2 * sqrt(m2)));
}

double flint_stat_kurtosis(void* v) {
    int64_t n = flint_vec_len(v);
    if (n < 4) return 0.0;
    double mean = 0.0;
    for (int64_t i = 0; i < n; i++) mean += (double)flint_vec_get(v, i);
    mean /= (double)n;
    double m2 = 0.0, m4 = 0.0;
    for (int64_t i = 0; i < n; i++) {
        double d = (double)flint_vec_get(v, i) - mean;
        m2 += d * d; m4 += d * d * d * d;
    }
    m2 /= (double)n; m4 /= (double)n;
    if (m2 <= 0.0) return 0.0;
    return (m4 / (m2 * m2)) - 3.0;      // excess kurtosis, like scipy
}

double flint_stat_covariance(void* x, void* y) {
    int64_t n = flint_vec_len(x);
    if (n != flint_vec_len(y) || n < 2) return 0.0;
    double mx = 0.0, my = 0.0;
    for (int64_t i = 0; i < n; i++) {
        mx += (double)flint_vec_get(x, i);
        my += (double)flint_vec_get(y, i);
    }
    mx /= (double)n; my /= (double)n;
    double c = 0.0;
    for (int64_t i = 0; i < n; i++) {
        c += ((double)flint_vec_get(x, i) - mx) * ((double)flint_vec_get(y, i) - my);
    }
    return c / (double)(n - 1);
}

double flint_stat_correlation(void* x, void* y) {
    double cov = flint_stat_covariance(x, y);
    double sx = flint_stat_stdev(x), sy = flint_stat_stdev(y);
    if (sx <= 0.0 || sy <= 0.0) return 0.0;
    return cov / (sx * sy);
}

int64_t flint_stat_linear_regression(void* x, void* y, double* slope, double* intercept) {
    int64_t n = flint_vec_len(x);
    if (n != flint_vec_len(y) || n < 2) return 0;
    double mx = 0.0, my = 0.0;
    for (int64_t i = 0; i < n; i++) {
        mx += (double)flint_vec_get(x, i);
        my += (double)flint_vec_get(y, i);
    }
    mx /= (double)n; my /= (double)n;
    double num = 0.0, den = 0.0;
    for (int64_t i = 0; i < n; i++) {
        double dx = (double)flint_vec_get(x, i) - mx;
        double dy = (double)flint_vec_get(y, i) - my;
        num += dx * dy;
        den += dx * dx;
    }
    if (den == 0.0) return 0;            // no variance in x: no unique slope
    *slope = num / den;
    *intercept = my - (*slope) * mx;
    return 1;
}

// ------------------------------------------------------------- accumulator
FlintAccum* flint_accum_new(void) {
    FlintAccum* a = (FlintAccum*)calloc(1, sizeof(FlintAccum));
    if (a) { a->lo = 0; a->hi = 0; }
    return a;
}

void flint_accum_push(FlintAccum* a, double x) {
    if (!a) return;
    if (a->n == 0) { a->lo = a->hi = x; }
    else { if (x < a->lo) a->lo = x; if (x > a->hi) a->hi = x; }
    a->n++;
    // Welford: numerically stable one-pass mean/variance.
    double delta = x - a->mean;
    a->mean += delta / (double)a->n;
    a->m2 += delta * (x - a->mean);
}

double flint_accum_mean(FlintAccum* a)     { return a ? a->mean : 0.0; }
int64_t flint_accum_count(FlintAccum* a)  { return a ? a->n : 0; }
double flint_accum_min(FlintAccum* a)     { return (a && a->n) ? a->lo : 0.0; }
double flint_accum_max(FlintAccum* a)     { return (a && a->n) ? a->hi : 0.0; }
double flint_accum_variance(FlintAccum* a) {
    return (a && a->n > 1) ? a->m2 / (double)(a->n - 1) : 0.0;
}
double flint_accum_stdev(FlintAccum* a) { return sqrt(flint_accum_variance(a)); }
void flint_accum_free(FlintAccum* a)    { free(a); }