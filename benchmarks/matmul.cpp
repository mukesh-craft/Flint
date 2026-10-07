// Naive dense matmul C++ mirror of benchmarks/matmul.fl.
#include <cstdio>
#include <cstdlib>
#include <chrono>
int main() {
    int sizes[3] = {100, 200, 500};
    for (int si = 0; si < 3; si++) {
        int N = sizes[si];
        double* a = (double*)malloc(sizeof(double) * N * N);
        double* b = (double*)malloc(sizeof(double) * N * N);
        double* c = (double*)malloc(sizeof(double) * N * N);
        for (int i = 0; i < N * N; i++) { a[i] = (i % 100) / 10.0; b[i] = (i % 50) / 7.0; }
        for (int w = 0; w < 3; w++)
            for (int i = 0; i < N; i++) for (int j = 0; j < N; j++) {
                double s = 0; for (int k = 0; k < N; k++) s += a[i*N+k] * b[k*N+j]; c[i*N+j] = s;
            }
        volatile double sink = c[0]; (void)sink;
        auto t0 = std::chrono::steady_clock::now();
        for (int it = 0; it < 10; it++)
            for (int i = 0; i < N; i++) for (int j = 0; j < N; j++) {
                double s = 0; for (int k = 0; k < N; k++) s += a[i*N+k] * b[k*N+j]; c[i*N+j] = s;
            }
        auto t1 = std::chrono::steady_clock::now();
        double avg = std::chrono::duration<double>(t1 - t0).count() / 10.0;
        printf("size %d\n", N);
        printf("avg ms = %.4f\n", avg * 1000.0);
        free(a); free(b); free(c);
    }
}
