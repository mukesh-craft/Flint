// Naive dense matmul, matching benchmarks/matmul.fl shapes. Build:
//   clang -O2 matmul.c -o matmul            (portable)
//   clang -O2 matmul.c -lopenblas -DMATMUL_BLAS=1 -o matmul_blas
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef MATMUL_BLAS
#include <openblas/cblas.h>   // Termux installs the headers under openblas/
#endif

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(void) {
    int sizes[3] = {100, 200, 500};
    for (int si = 0; si < 3; si++) {
        int N = sizes[si];
        double* a = malloc(sizeof(double) * N * N);
        double* b = malloc(sizeof(double) * N * N);
        double* c = malloc(sizeof(double) * N * N);
        for (int i = 0; i < N * N; i++) { a[i] = (i % 100) / 10.0; b[i] = (i % 50) / 7.0; }
#ifdef MATMUL_BLAS
        /* no warmup, single call */
        double t0 = now();
        for (int it = 0; it < 10; it++)
            cblas_dgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans, N, N, N, 1.0, a, N, b, N, 0.0, c, N);
        double t1 = now();
#else
        for (int w = 0; w < 3; w++) {
            for (int i = 0; i < N; i++) for (int j = 0; j < N; j++) {
                double s = 0; for (int k = 0; k < N; k++) s += a[i*N+k] * b[k*N+j]; c[i*N+j] = s;
            }
        }
        double t0 = now();
        for (int it = 0; it < 10; it++) {
            for (int i = 0; i < N; i++) for (int j = 0; j < N; j++) {
                double s = 0; for (int k = 0; k < N; k++) s += a[i*N+k] * b[k*N+j]; c[i*N+j] = s;
            }
        }
        double t1 = now();
#endif
        volatile double sink = c[0]; (void)sink;
        printf("size %d\n", N);
        printf("avg ms = %.4f\n", (t1 - t0) / 10.0 * 1000.0);
        free(a); free(b); free(c);
    }
    return 0;
}
