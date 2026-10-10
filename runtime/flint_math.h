// flint_math.h — complete math and statistics surface.
//
// Flint already had the transcendentals (sin/cos/exp/log/...) in runtime.c.
// This adds what a real numeric library needs and Python's math + statistics
// modules both carry: integer maths (factorial, gcd, lcm, isqrt, comb, perm),
// angle conversion, power/log variants (expm1, log1p, log2), error and special
// functions (erf, erfc, gamma, lgamma), floating-point classification
// (isfinite/isinf/isnan/isclose/copysign/ulp/nextafter), the constants, and
// the descriptive-statistics block from Python's statistics module.
//
// Everything is scalar and f64 unless noted. No allocation.

#ifndef FLINT_MATH_H
#define FLINT_MATH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- constants (as functions; Flint has no global constant table yet) ----
double flint_pi(void);        // 3.141592653589793
double flint_e(void);         // 2.718281828459045
double flint_tau(void);       // 6.283185307179586
double flint_inf(void);
double flint_nan(void);

// ---- integer maths ----
int64_t flint_factorial(int64_t n);
int64_t flint_gcd_i64(int64_t a, int64_t b);
int64_t flint_lcm_i64(int64_t a, int64_t b);
int64_t flint_isqrt(int64_t n);          // exact, no float rounding
int64_t flint_comb(int64_t n, int64_t k); // n choose k
int64_t flint_perm(int64_t n, int64_t k); // nPk
int64_t flint_fib(int64_t n);             // Fibonacci, iterative

// ---- angle conversion ----
double flint_radians(double deg);
double flint_degrees(double rad);

// ---- power / log variants ----
double flint_cbrt(double x);
double flint_expm1(double x);
double flint_log1p(double x);
double flint_log2(double x);
double flint_hypot2(double x, double y);
double flint_hypot3(double x, double y, double z);
double flint_copysign(double x, double y);

// ---- floating-point decomposition ----
double flint_fmod(double x, double y);
double flint_trunc(double x);
double flint_signum(double x);
int64_t flint_sign_i64(int64_t x);
double flint_ldexp(double x, int64_t e);
// Writes the fraction to *frac and returns the exponent (m in 0.5<=m<1).
int64_t flint_frexp(double x, double* frac);

// ---- classification ----
int64_t flint_is_finite(double x);
int64_t flint_is_inf(double x);
int64_t flint_is_nan(double x);
int64_t flint_is_close(double a, double b, double rel_tol, double abs_tol);
// Inclusive integer range test. The idiom needed after any random draw, and
// for validating parsed input -- returning i64 from a comparison invites an
// accidental use of the value rather than the predicate.
int64_t flint_is_in_range(double v, double lo, double hi);
// Integer form. Most range checks are on a count, an index or a random draw,
// all of which are i64 -- and routing those through the double form was a type
// error at every call site.
int64_t flint_i64_in_range(int64_t v, int64_t lo, int64_t hi);
double flint_ulp(double x);
double flint_nextafter(double x, double y);

// ---- error and special functions ----
double flint_erf(double x);
double flint_erfc(double x);
double flint_gamma(double x);
double flint_lgamma(double x);

// ---- descriptive statistics over a vec handle (flt_vec_*) ----
int64_t flint_stat_mean(void* v);
int64_t flint_stat_count(void* v);
int64_t flint_stat_min(void* v);
int64_t flint_stat_max(void* v);
int64_t flint_stat_median(void* v);       // copies and sorts; 0 if empty
double  flint_stat_median_f(void* v);
double  flint_stat_variance(void* v);    // sample (n-1 denominator)
double  flint_stat_pvariance(void* v);    // population (n denominator)
double  flint_stat_stdev(void* v);
double  flint_stat_pstdev(void* v);
double  flint_stat_geometric_mean(void* v);
double  flint_stat_harmonic_mean(void* v);
// p in [0,100]; linear interpolation between ranks, like numpy.percentile.
double  flint_stat_percentile(void* v, double p);
int64_t flint_stat_mode(void* v);         // most frequent value, first on tie
double  flint_stat_skewness(void* v);
double  flint_stat_kurtosis(void* v);

// Two-sample statistics. Both vectors must be the same length.
double flint_stat_covariance(void* x, void* y);
double flint_stat_correlation(void* x, void* y);
// Least-squares fit y = slope*x + intercept over paired samples.
int64_t flint_stat_linear_regression(void* x, void* y, double* slope, double* intercept);

// ---- accumulator, for streaming statistics without a vec ----
typedef struct FlintAccum {
    int64_t n;
    double  mean;
    double  m2;      // sum of squared deviations, for variance
    double  lo, hi;  // running min/max
} FlintAccum;

FlintAccum* flint_accum_new(void);
void flint_accum_push(FlintAccum* a, double x);
double flint_accum_mean(FlintAccum* a);
double flint_accum_variance(FlintAccum* a);
double flint_accum_stdev(FlintAccum* a);
double flint_accum_min(FlintAccum* a);
double flint_accum_max(FlintAccum* a);
int64_t flint_accum_count(FlintAccum* a);
void flint_accum_free(FlintAccum* a);

#ifdef __cplusplus
}
#endif

#endif // FLINT_MATH_H