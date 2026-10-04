// Verifies the live matmul path against the portable reference, and reports
// which backend is in use. Run by tests/test_ai.sh.
#include "flint_blas.h"
#include "flint_tensor.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static int failures = 0;

static void check(const char* name, int M, int K, int N) {
    double* A = (double*)malloc(sizeof(double) * M * K);
    double* B = (double*)malloc(sizeof(double) * K * N);
    double* live = (double*)calloc(M * N, sizeof(double));
    double* ref  = (double*)calloc(M * N, sizeof(double));

    // Deterministic pseudo-random data so a failure is reproducible.
    unsigned seed = 12345u;
    for (int64_t i = 0; i < M * K; i++) {
        seed = seed * 1103515245u + 12345u;
        A[i] = (double)((seed >> 16) % 2000 - 1000) / 100.0;
    }
    for (int64_t i = 0; i < K * N; i++) {
        seed = seed * 1103515245u + 12345u;
        B[i] = (double)((seed >> 16) % 2000 - 1000) / 100.0;
    }

    flint_blas_matmul(M, K, N, A, B, live);
    flint_blas_matmul_reference(M, K, N, A, B, ref);

    double worst = 0.0;
    for (int64_t i = 0; i < M * N; i++) {
        double d = fabs(live[i] - ref[i]);
        double scale = fabs(ref[i]) > 1.0 ? fabs(ref[i]) : 1.0;
        if (d / scale > worst) worst = d / scale;
    }
    if (worst > 1e-9) {
        printf("FAIL %s: max relative diff %g\n", name, worst);
        failures++;
    } else {
        printf("PASS %s (%lld calls so far)\n", name, (long long)flint_blas_calls());
    }
    free(A); free(B); free(live); free(ref);
}

int main(void) {
    printf("backend: %s\n", flint_blas_backend());
    // Shapes chosen to catch leading-dimension mistakes: non-square, and a
    // K that differs from both M and N.
    check("2x3 * 3x2", 2, 3, 2);
    check("3x2 * 2x4", 3, 2, 4);
    check("1x1 * 1x1", 1, 1, 1);
    check("4x7 * 7x5", 4, 7, 5);
    check("16x16 * 16x16", 16, 16, 16);
    if (failures) printf("blas-check: %d failed\n", failures);
    else printf("blas-check: all passed (%s)\n", flint_blas_backend());
    return failures ? 1 : 0;
}