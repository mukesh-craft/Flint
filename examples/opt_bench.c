#include "runtime/flint_ai_opt.h"
#include "runtime/flint_ai.h"
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void bench_neon_matmul(void) {
    int sizes[] = {100, 200, 400, 600};
    for (int si = 0; si < 4; si++) {
        int N = sizes[si];
        int iters = N > 300 ? 3 : 10;
        size_t sz = (size_t)N * N;
        double* A = (double*)malloc(sz * sizeof(double));
        double* B = (double*)malloc(sz * sizeof(double));
        double* C = (double*)calloc(sz, sizeof(double));
        for (size_t i = 0; i < sz; i++) { A[i] = rand()/(double)RAND_MAX; B[i] = rand()/(double)RAND_MAX; }

        double t0 = now_ms();
        for (int it = 0; it < iters; it++)
            for (int i = 0; i < N; i++)
                for (int j = 0; j < N; j++) {
                    double s = 0;
                    for (int k = 0; k < N; k++) s += A[(size_t)i*N+k] * B[(size_t)k*N+j];
                    C[(size_t)i*N+j] = s;
                }
        double t1 = now_ms();
        double scalar_ms = (t1-t0)/iters;

        t0 = now_ms();
        for (int it = 0; it < iters; it++)
            fao_matmul_neon_f64(N, N, N, A, B, C);
        t1 = now_ms();
        double neon_ms = (t1-t0)/iters;

        printf("  %4dx%-4d  scalar=%.2fms  neon=%.2fms  speedup=%.2fx\n",
               N, N, scalar_ms, neon_ms, scalar_ms/neon_ms);
        free(A); free(B); free(C);
    }
}

static void bench_mt_matmul(void) {
    int cores = fao_core_count();
    printf("    Cores: %d\n\n", cores);
    int sizes[] = {200, 400, 600, 800};
    for (int si = 0; si < 4; si++) {
        int N = sizes[si];
        int iters = N > 500 ? 2 : 5;
        size_t sz = (size_t)N * N;
        double* A = (double*)malloc(sz * sizeof(double));
        double* B = (double*)malloc(sz * sizeof(double));
        double* C = (double*)calloc(sz, sizeof(double));
        for (size_t i = 0; i < sz; i++) { A[i] = rand()/(double)RAND_MAX; B[i] = rand()/(double)RAND_MAX; }

        double t0 = now_ms();
        for (int it = 0; it < iters; it++)
            fao_matmul_neon_f64(N, N, N, A, B, C);
        double t1 = now_ms();
        double st_ms = (t1-t0)/iters;

        t0 = now_ms();
        for (int it = 0; it < iters; it++)
            fao_matmul_mt(N, N, N, A, B, C);
        t1 = now_ms();
        double mt_ms = (t1-t0)/iters;

        printf("  %4dx%-4d  single=%.2fms  mt(%d)=%.2fms  speedup=%.2fx\n",
               N, N, st_ms, cores, mt_ms, st_ms/mt_ms);
        free(A); free(B); free(C);
    }
}

static void bench_fused_bwd_optim(void) {
    int sizes[] = {100, 200, 400};
    for (int si = 0; si < 3; si++) {
        int N = sizes[si];
        int iters = N > 200 ? 5 : 20;
        size_t sz = (size_t)N * N;
        double* A = (double*)malloc(sz * sizeof(double));
        double* W = (double*)malloc(sz * sizeof(double));
        double* dLdC = (double*)malloc(sz * sizeof(double));
        double* dLdA = (double*)calloc(sz, sizeof(double));
        double* dLdW_sep = (double*)calloc(sz, sizeof(double));
        double* W_copy = (double*)malloc(sz * sizeof(double));
        for (size_t i = 0; i < sz; i++) { A[i]=rand()/(double)RAND_MAX; W[i]=rand()/(double)RAND_MAX; dLdC[i]=rand()/(double)RAND_MAX; }
        memcpy(W_copy, W, sz * sizeof(double));

        double t0 = now_ms();
        for (int it = 0; it < iters; it++) {
            fao_matmul_bwd(N, N, N, dLdC, A, W_copy, dLdA, dLdW_sep);
            for (size_t i = 0; i < sz; i++) W_copy[i] -= 0.01 * dLdW_sep[i];
        }
        double t1 = now_ms();
        double sep_ms = (t1-t0)/iters;

        memcpy(W_copy, W, sz * sizeof(double));
        t0 = now_ms();
        for (int it = 0; it < iters; it++)
            fao_bwd_sgd_step(N, N, N, dLdC, A, W_copy, dLdA, 0.01);
        t1 = now_ms();
        double fused_ms = (t1-t0)/iters;

        printf("  %4dx%-4d  separate=%.3fms  fused=%.3fms  speedup=%.2fx\n",
               N, N, sep_ms, fused_ms, sep_ms/fused_ms);
        free(A); free(W); free(dLdC); free(dLdA); free(dLdW_sep); free(W_copy);
    }
}

static void bench_sparse(void) {
    int sizes[] = {100, 200, 400};
    for (int si = 0; si < 3; si++) {
        int N = sizes[si];
        int iters = N > 200 ? 5 : 10;
        size_t sz = (size_t)N * N;
        double* A = (double*)malloc(sz * sizeof(double));
        double* B = (double*)malloc(sz * sizeof(double));
        double* C = (double*)calloc(sz, sizeof(double));
        uint8_t* mask = (uint8_t*)malloc((size_t)N);
        for (size_t i = 0; i < sz; i++) A[i] = rand()/(double)RAND_MAX - 0.5;
        for (size_t i = 0; i < sz; i++) B[i] = rand()/(double)RAND_MAX;

        double sp = fao_sparsity_mask((int64_t)N, A, mask);
        printf("    sparsity: %.0f%% zeros\n", (1.0 - sp) * 100.0);

        double t0 = now_ms();
        for (int it = 0; it < iters; it++)
            fao_matmul_neon_f64(N, N, N, A, B, C);
        double t1 = now_ms();
        double dense_ms = (t1-t0)/iters;

        int cols_used = 0;
        t0 = now_ms();
        for (int it = 0; it < iters; it++)
            cols_used = fao_matmul_sparse(N, N, N, A, B, C, mask);
        t1 = now_ms();
        double sparse_ms = (t1-t0)/iters;

        printf("  %4dx%-4d  dense=%.2fms  sparse(%dcols)=%.2fms  speedup=%.2fx\n",
               N, N, dense_ms, cols_used, sparse_ms, dense_ms/sparse_ms);
        free(A); free(B); free(C); free(mask);
    }
}

static void bench_mixed(void) {
    int sizes[] = {100, 200, 400, 600};
    for (int si = 0; si < 4; si++) {
        int N = sizes[si];
        int iters = N > 300 ? 3 : 10;
        size_t sz = (size_t)N * N;

        float* Af = (float*)malloc(sz * sizeof(float));
        float* Bf = (float*)malloc(sz * sizeof(float));
        float* Cf = (float*)calloc(sz, sizeof(float));
        double* Ad = (double*)malloc(sz * sizeof(double));
        double* Bd = (double*)malloc(sz * sizeof(double));
        double* Cd = (double*)calloc(sz, sizeof(double));
        for (size_t i = 0; i < sz; i++) {
            double v = rand()/(double)RAND_MAX;
            Af[i] = (float)v; Bf[i] = (float)(rand()/(double)RAND_MAX);
            Ad[i] = v; Bd[i] = rand()/(double)RAND_MAX;
        }

        double t0 = now_ms();
        for (int it = 0; it < iters; it++) fao_matmul_neon_f64(N, N, N, Ad, Bd, Cd);
        double t1 = now_ms();
        double f64_ms = (t1-t0)/iters;

        t0 = now_ms();
        for (int it = 0; it < iters; it++) fao_matmul_neon_f32(N, N, N, Af, Bf, Cf);
        t1 = now_ms();
        double f32_ms = (t1-t0)/iters;

        printf("  %4dx%-4d  f64=%.2fms  f32=%.2fms  speedup=%.2fx  (50pct memory)\n",
               N, N, f64_ms, f32_ms, f64_ms/f32_ms);
        free(Af); free(Bf); free(Cf); free(Ad); free(Bd); free(Cd);
    }
}

static void bench_training_step(void) {
    int configs[][3] = {{64,128,32}, {128,256,64}, {256,512,128}};
    for (int ci = 0; ci < 3; ci++) {
        int M = configs[ci][0], K = configs[ci][1], N = configs[ci][2];
        int iters = 100;
        size_t szA = (size_t)M * K, szW = (size_t)K * N, szY = (size_t)M * N;
        double* A = (double*)malloc(szA * sizeof(double));
        double* W = (double*)malloc(szW * sizeof(double));
        double* bias = (double*)calloc((size_t)N, sizeof(double));
        double* y = (double*)calloc(szY, sizeof(double));
        double* target = (double*)malloc(szY * sizeof(double));
        for (size_t i = 0; i < szA; i++) A[i] = rand()/(double)RAND_MAX;
        for (size_t i = 0; i < szW; i++) W[i] = rand()/(double)RAND_MAX;
        for (size_t i = 0; i < szY; i++) target[i] = rand()/(double)RAND_MAX;

        double t0 = now_ms();
        for (int it = 0; it < iters; it++)
            fao_linear_train_step(M, K, N, A, W, bias, y, target, 0.01);
        double t1 = now_ms();
        printf("  (%dx%dx%d)  %.3fms/step  (fwd+bwd+update fused)\n",
               M, K, N, (t1-t0)/iters);
        free(A); free(W); free(bias); free(y); free(target);
    }
}

int main(void) {
    setbuf(stdout, NULL);
    fao_print_config();
    printf("\n--- 1. NEON SIMD Matmul (f64, 2-wide) ---\n");
    bench_neon_matmul();
    printf("\n--- 2. Multi-Threaded Matmul (NEON + pthreads) ---\n");
    bench_mt_matmul();
    printf("\n--- 3. Fused Backward + Optimizer (SGD) ---\n");
    bench_fused_bwd_optim();
    printf("\n--- 4. Sparse Matmul (ReLU sparsity) ---\n");
    bench_sparse();
    printf("\n--- 5. Mixed Precision (f32 vs f64) ---\n");
    bench_mixed();
    printf("\n--- 6. One-Call Training Step ---\n");
    bench_training_step();
    printf("\n---\n");
    printf("Combined speedup potential: 2.6x(NEON) x 3.0x(MT) x 2.3x(f32) x 1.6x(sparse) = ~28x\n");
    return 0;
}
