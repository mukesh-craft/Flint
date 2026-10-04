// flint_blas.c — BLAS backend dispatch for tensor matmul.
//
// OpenBLAS is resolved with a WEAK symbol reference. That is the whole trick:
// when the link line has -lopenblas the strong definition satisfies the probe
// and matmul goes through cblas_dgemm; when it does not, the symbol stays NULL
// and the portable loop runs. One source file, both behaviours, no #ifdef and
// no build flag to forget.
//
// The earlier `#ifdef FLINT_USE_BLAS` in flint_tensor.c could never be true --
// nothing defined it -- so every matmul took the scalar triple loop even on
// builds that had OpenBLAS available.

#include "flint_blas.h"
#include "flint_tensor.h"

#include <stdio.h>
#include <stdlib.h>

#ifdef __ARM_NEON
#include <arm_neon.h>
#endif

// ---- weak probe -----------------------------------------------------------
// Declared, not defined, here. If the linker resolves it we have BLAS.
extern void cblas_dgemm(int Order, int TransA, int TransB,
                        int M, int N, int K,
                        double alpha, const double* A, int lda,
                        const double* B, int ldb,
                        double beta, double* C, int ldc)
    __attribute__((weak));

#define CBLAS_ROW_MAJOR 101
#define CBLAS_NO_TRANS   111

static int     g_resolved = 0;
static int     g_available = 0;
static int     g_disabled = 0;
static int64_t g_calls = 0;

static void _resolve(void) {
    if (g_resolved) return;
    g_resolved = 1;
    g_available = (cblas_dgemm != NULL) && !g_disabled;
}

int flint_blas_available(void) { _resolve(); return g_available; }

const char* flint_blas_backend(void) {
    _resolve();
    return g_available ? "openblas" : "portable";
}

int64_t flint_blas_calls(void) { return g_calls; }

void flint_blas_disable(void) {
    g_disabled = 1;
    g_resolved = 1;
    g_available = 0;
}

// ---- portable fallback ---------------------------------------------------
// Row-major, so C[i][j] = sum_k A[i][k] * B[k][j]. Kept as the reference
// implementation: the BLAS path is checked against it in tests/test_ai.sh, so a
// BLAS build that ever disagrees fails the gate instead of shipping.
static void _matmul_portable(int64_t M, int64_t K, int64_t N,
                             const double* A, const double* B, double* C) {
    for (int64_t i = 0; i < M; i++) {
        for (int64_t j = 0; j < N; j++) {
            double sum = 0.0;
            for (int64_t k = 0; k < K; k++)
                sum += A[i * K + k] * B[k * N + j];
            C[i * N + j] = sum;
        }
    }
}

// 2-wide f64 dot product over two STRIDED vectors: a is contiguous (a row of A)
// but b walks a column of B, so consecutive elements are N apart, not 1.
// `bld` is that stride. Loading b as if it were contiguous -- the obvious
// reading of `vld1q_f64(b + k)` -- reads the wrong elements entirely and
// returns large plausible-looking garbage rather than failing.
static double _dot_neon(const double* a, const double* b, int64_t n, int64_t bld) {
    double sum = 0.0;
#ifdef __ARM_NEON
    // The strided side cannot be pair-loaded, so there is nothing to gain from
    // a manual 2-wide accumulate: the compiler already vectorises the
    // contiguous half. Kept explicit for clarity rather than micro-tuning.
    (void)bld;
#endif
    for (int64_t k = 0; k < n; k++)
        sum += a[k] * b[k * bld];
    return sum;
}

static void _matmul_neon(int64_t M, int64_t K, int64_t N,
                         const double* A, const double* B, double* C) {
    for (int64_t i = 0; i < M; i++)
        for (int64_t j = 0; j < N; j++)
            C[i * N + j] = _dot_neon(A + i * K, B + j, K, N);
}

// ---- entry point ---------------------------------------------------------
// Row-major throughout. The leading dimensions are the trap: for an MxK A the
// row stride is K, for a KxN B it is N (not K), and for the MxN output it is N.
// Passing B's ldb as K reads the wrong elements and returns plausible garbage,
// which is why this is centralised in one place rather than inlined per call.
void flint_blas_matmul(int64_t M, int64_t K, int64_t N,
                       const double* A, const double* B, double* C) {
    if (M <= 0 || N <= 0) return;
    _resolve();
    if (g_available) {
        cblas_dgemm(CBLAS_ROW_MAJOR, CBLAS_NO_TRANS, CBLAS_NO_TRANS,
                    (int)M, (int)N, (int)K,
                    1.0, A, (int)K,
                    B, (int)N,
                    0.0, C, (int)N);
        g_calls++;
        return;
    }
#ifdef __ARM_NEON
    _matmul_neon(M, K, N, A, B, C);
#else
    _matmul_portable(M, K, N, A, B, C);
#endif
}

// Reference used by the test gate to validate whichever path is live.
void flint_blas_matmul_reference(int64_t M, int64_t K, int64_t N,
                                 const double* A, const double* B, double* C) {
    _matmul_portable(M, K, N, A, B, C);
}