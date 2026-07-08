#include "flint_ai_opt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <unistd.h>

// =========================================================================
// Hardware detection
// =========================================================================

int fao_has_neon(void) {
    #ifdef __ARM_NEON
    return 1;
    #else
    return 0;
    #endif
}

int fao_has_avx(void) {
    #ifdef __AVX__
    return 1;
    #elif defined(__x86_64__) || defined(_M_X64)
    // Runtime check
    static int avx_ok = -1;
    if (avx_ok == -1) {
        // Simple CPUID check
        unsigned int eax, ebx, ecx, edx;
        #if defined(_MSC_VER)
        int cpuInfo[4];
        __cpuid(cpuInfo, 1);
        eax = cpuInfo[0]; ebx = cpuInfo[1];
        ecx = cpuInfo[2]; edx = cpuInfo[3];
        #else
        __asm__("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
        #endif
        avx_ok = (ecx & (1 << 28)) != 0;
    }
    return avx_ok;
    #else
    return 0;
    #endif
}

int fao_core_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 4;
}

// =========================================================================
// Thread pool — simple fork-join per call
// =========================================================================

// For each parallel matmul, we create worker threads and join them.
// Thread creation overhead (~50μs) is negligible for large matmuls (>200x200).
// This avoids all the complexity of a persistent thread pool.

typedef struct {
    int64_t si, ei;
    void (*f)(int64_t, void*);
    void* a;
} FAOWorkerArg;

static void* _worker_run(void* arg) {
    FAOWorkerArg* w = (FAOWorkerArg*)arg;
    for (int64_t i = w->si; i < w->ei; i++) w->f(i, w->a);
    free(w);
    return NULL;
}

struct FAThreadPool {
    int n_threads;
};

FAThreadPool* fao_pool_create(int n_threads) {
    if (n_threads <= 0) n_threads = fao_core_count();
    if (n_threads < 1) n_threads = 1;
    FAThreadPool* pool = (FAThreadPool*)malloc(sizeof(FAThreadPool));
    pool->n_threads = n_threads;
    return pool;
}

void fao_pool_destroy(FAThreadPool* pool) { free(pool); }

void fao_pool_for(FAThreadPool* pool, int64_t start, int64_t end,
                  void (*func)(int64_t, void*), void* arg) {
    if (end <= start) return;
    int64_t n = end - start;
    int nt = pool->n_threads;
    if (nt > n) nt = (int)n;
    if (nt <= 1) {
        for (int64_t i = start; i < end; i++) func(i, arg);
        return;
    }

    pthread_t* threads = (pthread_t*)malloc((size_t)(nt - 1) * sizeof(pthread_t));
    int64_t* indices = (int64_t*)malloc((size_t)(nt + 1) * sizeof(int64_t));

    for (int i = 0; i <= nt; i++)
        indices[i] = start + (n * i) / nt;

    for (int i = 1; i < nt; i++) {
        FAOWorkerArg* w = (FAOWorkerArg*)malloc(sizeof(FAOWorkerArg));
        w->si = indices[i];
        w->ei = indices[i + 1];
        w->f = func;
        w->a = arg;
        pthread_create(&threads[i - 1], NULL, _worker_run, w);
    }

    for (int64_t i = indices[0]; i < indices[1]; i++) func(i, arg);

    for (int i = 1; i < nt; i++)
        pthread_join(threads[i - 1], NULL);

    free(threads);
    free(indices);
}

FAThreadPool* fao_global_pool(void) {
    static FAThreadPool* pool = NULL;
    static int inited = 0;
    if (!inited) {
        pool = fao_pool_create(0);
        inited = 1;
    }
    return pool;
}

// =========================================================================
// NEON SIMD matmul (f64, 2-wide)
// =========================================================================

#ifdef __ARM_NEON
#include <arm_neon.h>

// Tiled matmul with NEON: processes 2 columns per row per iteration
// Uses blocking for L1/L2 cache
#define NEON_TM 64
#define NEON_TN 64
#define NEON_TK 512

void fao_matmul_neon_f64(int M, int K, int N,
                         const double* A, const double* B, double* C) {
    #pragma omp parallel for collapse(2) if(M > 100)
    for (int64_t i0 = 0; i0 < M; i0 += NEON_TM) {
        int64_t imax = i0 + NEON_TM < M ? i0 + NEON_TM : M;
        for (int64_t j0 = 0; j0 < N; j0 += NEON_TN) {
            int64_t jmax = j0 + NEON_TN < N ? j0 + NEON_TN : N;
            // Zero accumulators
            double tile[NEON_TM][NEON_TN];
            memset(tile, 0, (size_t)(imax - i0) * (jmax - j0) * sizeof(double));

            // K-strip with SIMD
            for (int64_t kk = 0; kk < K; kk += NEON_TK) {
                int64_t kmax = kk + NEON_TK < K ? kk + NEON_TK : K;
                for (int64_t i = i0; i < imax; i++) {
                    int64_t ti = i - i0;
                    for (int64_t k = kk; k < kmax; k++) {
                        double a_val = A[(size_t)i * K + k];
                        if (a_val == 0.0) continue;
                        float64x2_t a_vec = vdupq_n_f64(a_val);
                        int64_t j = j0;
                        for (; j + 1 < jmax; j += 2) {
                            float64x2_t b_vec = vld1q_f64(&B[(size_t)k * N + j]);
                            float64x2_t c_vec = vld1q_f64(&tile[ti][j - j0]);
                            c_vec = vmlaq_f64(c_vec, a_vec, b_vec);
                            vst1q_f64(&tile[ti][j - j0], c_vec);
                        }
                        if (j < jmax) {
                            tile[ti][j - j0] += a_val * B[(size_t)k * N + j];
                        }
                    }
                }
            }

            // Write results
            for (int64_t i = i0; i < imax; i++)
                for (int64_t j = j0; j < jmax; j++)
                    C[(size_t)i * N + j] = tile[i - i0][j - j0];
        }
    }
}

#else // no NEON

void fao_matmul_neon_f64(int M, int K, int N,
                         const double* A, const double* B, double* C) {
    fao_matmul(M, K, N, A, B, C);
}

#endif

// =========================================================================
// Fused NEON matmul+bias+relu (proper tiled implementation)
// =========================================================================

void fao_fused_neon_f64(int M, int K, int N,
                        const double* A, const double* W,
                        const double* bias, double* C, int has_relu) {
    #ifdef __ARM_NEON
    // Tiled execution with NEON
    for (int64_t i0 = 0; i0 < M; i0 += NEON_TM) {
        int64_t imax = i0 + NEON_TM < M ? i0 + NEON_TM : M;
        for (int64_t j0 = 0; j0 < N; j0 += NEON_TN) {
            int64_t jmax = j0 + NEON_TN < N ? j0 + NEON_TN : N;
            double tile[NEON_TM][NEON_TN];
            memset(tile, 0, (imax - i0) * (jmax - j0) * sizeof(double));

            for (int64_t kk = 0; kk < K; kk += NEON_TK) {
                int64_t kmax = kk + NEON_TK < K ? kk + NEON_TK : K;
                for (int64_t i = i0; i < imax; i++) {
                    int64_t ti = i - i0;
                    for (int64_t k = kk; k < kmax; k++) {
                        double a_val = A[(size_t)i * K + k];
                        if (a_val == 0.0) continue;
                        float64x2_t a_vec = vdupq_n_f64(a_val);
                        int64_t j = j0;
                        for (; j + 1 < jmax; j += 2) {
                            float64x2_t b = vld1q_f64(&W[(size_t)k * N + j]);
                            float64x2_t c = vld1q_f64(&tile[ti][j - j0]);
                            c = vmlaq_f64(c, a_vec, b);
                            vst1q_f64(&tile[ti][j - j0], c);
                        }
                        if (j < jmax)
                            tile[ti][j - j0] += a_val * W[(size_t)k * N + j];
                    }
                }
            }

            // Write with bias + relu fused
            for (int64_t i = i0; i < imax; i++) {
                for (int64_t j = j0; j < jmax; j++) {
                    double v = tile[i - i0][j - j0];
                    if (bias) v += bias[j];
                    if (has_relu) v = v > 0.0 ? v : 0.0;
                    C[(size_t)i * N + j] = v;
                }
            }
        }
    }
    #else
    // Fallback
    for (int64_t i = 0; i < M; i++)
        for (int64_t j = 0; j < N; j++) {
            double sum = 0.0;
            for (int64_t k = 0; k < K; k++)
                sum += A[(size_t)i * K + k] * W[(size_t)k * N + j];
            if (bias) sum += bias[j];
            if (has_relu) sum = sum > 0.0 ? sum : 0.0;
            C[(size_t)i * N + j] = sum;
        }
    #endif
}

// =========================================================================
// NEON f32 matmul (4-wide SIMD)
// =========================================================================

#ifdef __ARM_NEON
void fao_matmul_neon_f32(int M, int K, int N,
                         const float* A, const float* B, float* C) {
    for (int64_t i0 = 0; i0 < M; i0 += NEON_TM) {
        int64_t imax = i0 + NEON_TM < M ? i0 + NEON_TM : M;
        for (int64_t j0 = 0; j0 < N; j0 += NEON_TN) {
            int64_t jmax = j0 + NEON_TN < N ? j0 + NEON_TN : N;
            float tile[NEON_TM][NEON_TN] = {{0}};
            for (int64_t kk = 0; kk < K; kk += NEON_TK) {
                int64_t kmax = kk + NEON_TK < K ? kk + NEON_TK : K;
                for (int64_t i = i0; i < imax; i++) {
                    int64_t ti = i - i0;
                    for (int64_t k = kk; k < kmax; k++) {
                        float a_val = A[(size_t)i * K + k];
                        if (a_val == 0.0f) continue;
                        float32x4_t a_vec = vdupq_n_f32(a_val);
                        int64_t j = j0;
                        for (; j + 3 < jmax; j += 4) {
                            float32x4_t b = vld1q_f32(&B[(size_t)k * N + j]);
                            float32x4_t c = vld1q_f32(&tile[ti][j - j0]);
                            c = vmlaq_f32(c, a_vec, b);
                            vst1q_f32(&tile[ti][j - j0], c);
                        }
                        for (; j < jmax; j++)
                            tile[ti][j - j0] += a_val * B[(size_t)k * N + j];
                    }
                }
            }
            for (int64_t i = i0; i < imax; i++)
                for (int64_t j = j0; j < jmax; j++)
                    C[(size_t)i * N + j] = tile[i - i0][j - j0];
        }
    }
}
#else
void fao_matmul_neon_f32(int M, int K, int N,
                         const float* A, const float* B, float* C) {
    for (int64_t i = 0; i < M; i++)
        for (int64_t j = 0; j < N; j++) {
            float sum = 0.0f;
            for (int64_t k = 0; k < K; k++)
                sum += A[(size_t)i * K + k] * B[(size_t)k * N + j];
            C[(size_t)i * N + j] = sum;
        }
}
#endif

// =========================================================================
// Fused linear f32
// =========================================================================

void fao_fused_linear_f32(int M, int K, int N,
                          const float* A, const float* W,
                          const float* bias, float* C, int has_relu) {
    #ifdef __ARM_NEON
    for (int64_t i = 0; i < M; i++) {
        for (int64_t j = 0; j < N; j++) {
            float32x4_t sum = vdupq_n_f32(0.0f);
            int64_t k = 0;
            for (; k + 3 < K; k += 4) {
                float32x4_t a = vld1q_f32(&A[(size_t)i * K + k]);
                float32x4_t w = { W[(size_t)k * N + j], W[((size_t)k+1)*N + j],
                                  W[((size_t)k+2)*N + j], W[((size_t)k+3)*N + j] };
                sum = vmlaq_f32(sum, a, w);
            }
            float v = sum[0] + sum[1] + sum[2] + sum[3];
            for (; k < K; k++) v += A[(size_t)i * K + k] * W[(size_t)k * N + j];
            if (bias) v += bias[j];
            if (has_relu) v = v > 0.0f ? v : 0.0f;
            C[(size_t)i * N + j] = v;
        }
    }
    #else
    for (int64_t i = 0; i < M; i++)
        for (int64_t j = 0; j < N; j++) {
            float sum = 0.0f;
            for (int64_t k = 0; k < K; k++)
                sum += A[(size_t)i * K + k] * W[(size_t)k * N + j];
            if (bias) sum += bias[j];
            if (has_relu) sum = sum > 0.0f ? sum : 0.0f;
            C[(size_t)i * N + j] = sum;
        }
    #endif
}

// =========================================================================
// Kernel dispatch — auto-selects best
// =========================================================================

void fao_matmul(int M, int K, int N,
                const double* A, const double* B, double* C) {
    #ifdef __ARM_NEON
    // Use NEON if available
    if (M > 64 && N > 64 && M * N * K > 1000000) {
        // Large: use multi-threaded NEON
        // (fall through to NEON for now; MT is handled by fao_matmul_mt)
        fao_matmul_neon_f64(M, K, N, A, B, C);
    } else {
        fao_matmul_neon_f64(M, K, N, A, B, C);
    }
    #else
    // Fallback: tiled scalar
    for (int64_t i = 0; i < M; i++)
        for (int64_t j = 0; j < N; j++) {
            double sum = 0.0;
            for (int64_t k = 0; k < K; k++)
                sum += A[(size_t)i * K + k] * B[(size_t)k * N + j];
            C[(size_t)i * N + j] = sum;
        }
    #endif
}

void fao_fused_linear(int M, int K, int N,
                      const double* A, const double* W, const double* bias,
                      double* C, int has_relu) {
    fao_fused_neon_f64(M, K, N, A, W, bias, C, has_relu);
}

// =========================================================================
// Multi-threaded kernels
// =========================================================================

// Chunk-based parallelism: each thread processes a contiguous block of rows
// using the tiled NEON kernel. This preserves cache locality.

typedef struct {
    int64_t i0, i1;
    int M, K, N;
    const double *A, *B, *bias;
    double* C;
    int has_relu;
} MTChunk;

static void* _mt_chunk_worker(void* arg) {
    MTChunk* c = (MTChunk*)arg;
    int64_t mh = c->i1 - c->i0;
    if (mh <= 0) return NULL;
    // Use the full NEON tiled matmul on just the thread's rows
    #ifdef __ARM_NEON
    for (int64_t i0 = c->i0; i0 < c->i1; i0 += NEON_TM) {
        int64_t imax = i0 + NEON_TM < c->i1 ? i0 + NEON_TM : c->i1;
        for (int64_t j0 = 0; j0 < c->N; j0 += NEON_TN) {
            int64_t jmax = j0 + NEON_TN < c->N ? j0 + NEON_TN : c->N;
            double tile[NEON_TM][NEON_TN];
            memset(tile, 0, (size_t)(imax - i0) * (jmax - j0) * sizeof(double));
            for (int64_t kk = 0; kk < c->K; kk += NEON_TK) {
                int64_t kmax = kk + NEON_TK < c->K ? kk + NEON_TK : c->K;
                for (int64_t i = i0; i < imax; i++) {
                    int64_t ti = i - i0;
                    for (int64_t k = kk; k < kmax; k++) {
                        double a_val = c->A[(size_t)i * c->K + k];
                        if (a_val == 0.0) continue;
                        float64x2_t a_vec = vdupq_n_f64(a_val);
                        int64_t j = j0;
                        for (; j + 1 < jmax; j += 2) {
                            float64x2_t b = vld1q_f64(&c->B[(size_t)k * c->N + j]);
                            float64x2_t acc = vld1q_f64(&tile[ti][j - j0]);
                            acc = vmlaq_f64(acc, a_vec, b);
                            vst1q_f64(&tile[ti][j - j0], acc);
                        }
                        if (j < jmax)
                            tile[ti][j - j0] += a_val * c->B[(size_t)k * c->N + j];
                    }
                }
            }
            // Write (with bias + relu)
            for (int64_t i = i0; i < imax; i++)
                for (int64_t j = j0; j < jmax; j++) {
                    double v = tile[i - i0][j - j0];
                    if (c->bias) v += c->bias[j];
                    if (c->has_relu) v = v > 0.0 ? v : 0.0;
                    c->C[(size_t)i * c->N + j] = v;
                }
        }
    }
    #else
    for (int64_t i = c->i0; i < c->i1; i++)
        for (int64_t j = 0; j < c->N; j++) {
            double sum = 0;
            for (int64_t k = 0; k < c->K; k++)
                sum += c->A[(size_t)i * c->K + k] * c->B[(size_t)k * c->N + j];
            if (c->bias) sum += c->bias[j];
            if (c->has_relu) sum = sum > 0 ? sum : 0;
            c->C[(size_t)i * c->N + j] = sum;
        }
    #endif
    return NULL;
}

void fao_matmul_mt(int M, int K, int N,
                   const double* A, const double* B, double* C) {
    int nt = fao_global_pool()->n_threads;
    if (nt > M) nt = M;
    if (nt <= 1) { fao_matmul_neon_f64(M, K, N, A, B, C); return; }

    pthread_t threads[64];
    MTChunk chunks[64];
    int64_t chunk_size = M / nt;

    for (int i = 0; i < nt; i++) {
        chunks[i].i0 = i * chunk_size;
        chunks[i].i1 = (i == nt - 1) ? M : (i + 1) * chunk_size;
        chunks[i].M = M; chunks[i].K = K; chunks[i].N = N;
        chunks[i].A = A; chunks[i].B = B; chunks[i].C = C;
        chunks[i].bias = NULL; chunks[i].has_relu = 0;
    }

    for (int i = 1; i < nt; i++)
        pthread_create(&threads[i], NULL, _mt_chunk_worker, &chunks[i]);

    _mt_chunk_worker(&chunks[0]);

    for (int i = 1; i < nt; i++)
        pthread_join(threads[i], NULL);
}

void fao_fused_mt(int M, int K, int N,
                  const double* A, const double* W, const double* bias,
                  double* C, int has_relu) {
    int nt = fao_global_pool()->n_threads;
    if (nt > M) nt = M;
    if (nt <= 1) { fao_fused_neon_f64(M, K, N, A, W, bias, C, has_relu); return; }

    pthread_t threads[64];
    MTChunk chunks[64];
    int64_t chunk_size = M / nt;

    for (int i = 0; i < nt; i++) {
        chunks[i].i0 = i * chunk_size;
        chunks[i].i1 = (i == nt - 1) ? M : (i + 1) * chunk_size;
        chunks[i].M = M; chunks[i].K = K; chunks[i].N = N;
        chunks[i].A = A; chunks[i].B = W; chunks[i].bias = bias;
        chunks[i].C = C; chunks[i].has_relu = has_relu;
    }

    for (int i = 1; i < nt; i++)
        pthread_create(&threads[i], NULL, _mt_chunk_worker, &chunks[i]);

    _mt_chunk_worker(&chunks[0]);

    for (int i = 1; i < nt; i++)
        pthread_join(threads[i], NULL);
}

// =========================================================================
// Fused backward + optimizer
// =========================================================================

// dL/dA[M][K] = dL/dC[M][N] @ W[K][N]^T → dL/dA[i][k] = sum_j dLdC[i][j] * W[k][j]
// dL/dW[K][N] = A[M][K]^T @ dL/dC[M][N] → dL/dW[k][j] = sum_i A[i][k] * dLdC[i][j]

void fao_matmul_bwd(int M, int K, int N,
                    const double* dLdC, const double* A, const double* W,
                    double* dLdA, double* dLdW) {
    // dL/dA[M][K] = dLdC[M][N] @ W[K][N]^T
    // Use tiled approach: for each i, compute all k at once
    for (int64_t i = 0; i < M; i++) {
        for (int64_t k = 0; k < K; k++) {
            double sum = 0;
            #ifdef __ARM_NEON
            float64x2_t s = vdupq_n_f64(0.0);
            int64_t j = 0;
            for (; j + 1 < N; j += 2) {
                float64x2_t dc = vld1q_f64(&dLdC[(size_t)i * N + j]);
                float64x2_t w = vld1q_f64(&W[(size_t)k * N + j]);
                s = vmlaq_f64(s, dc, w);
            }
            sum = s[0] + s[1];
            for (; j < N; j++)
                sum += dLdC[(size_t)i * N + j] * W[(size_t)k * N + j];
            #else
            for (int64_t j = 0; j < N; j++)
                sum += dLdC[(size_t)i * N + j] * W[(size_t)k * N + j];
            #endif
            dLdA[(size_t)i * K + k] = sum;
        }
    }

    // dL/dW[K][N] = A[M][K]^T @ dLdC[M][N]
    memset(dLdW, 0, (size_t)K * N * sizeof(double));
    for (int64_t k = 0; k < K; k++) {
        #ifdef __ARM_NEON
        for (int64_t jj = 0; jj < N; jj += 2) {
            float64x2_t sum = vdupq_n_f64(0.0);
            for (int64_t i = 0; i < M; i++) {
                float64x2_t a = vdupq_n_f64(A[(size_t)i * K + k]);
                float64x2_t dc = vld1q_f64(&dLdC[(size_t)i * N + jj]);
                sum = vmlaq_f64(sum, a, dc);
            }
            dLdW[(size_t)k * N + jj] = sum[0];
            if (jj + 1 < N) dLdW[(size_t)k * N + jj + 1] = sum[1];
        }
        #else
        for (int64_t j = 0; j < N; j++) {
            double sum = 0;
            for (int64_t i = 0; i < M; i++)
                sum += A[(size_t)i * K + k] * dLdC[(size_t)i * N + j];
            dLdW[(size_t)k * N + j] = sum;
        }
        #endif
    }
}

void fao_bwd_sgd_step(int M, int K, int N,
                      const double* dLdC, const double* A,
                      double* W,
                      double* dLdA,
                      double lr) {
    // Fused: compute dL/dW and apply W -= lr * dL/dW in one pass.
    // W[k,:] row stays hot in cache across dL/dA and dL/dW computations.
    for (int64_t k = 0; k < K; k++) {
        // Step 1: dL/dA[i][k] = sum_j dLdC[i][j] * W[k][j]
        #ifdef __ARM_NEON
        for (int64_t i = 0; i < M; i++) {
            float64x2_t sum = vdupq_n_f64(0.0);
            int64_t j = 0;
            for (; j + 1 < N; j += 2) {
                float64x2_t dc = vld1q_f64(&dLdC[(size_t)i * N + j]);
                float64x2_t w = vld1q_f64(&W[(size_t)k * N + j]);
                sum = vmlaq_f64(sum, dc, w);
            }
            double s = sum[0] + sum[1];
            for (; j < N; j++)
                s += dLdC[(size_t)i * N + j] * W[(size_t)k * N + j];
            dLdA[(size_t)i * K + k] = s;
        }
        // Step 2 & 3: compute grad and update (W[k,:] still hot from step 1)
        for (int64_t jj = 0; jj < N; jj += 2) {
            float64x2_t grad = vdupq_n_f64(0.0);
            for (int64_t i = 0; i < M; i++) {
                float64x2_t a = vdupq_n_f64(A[(size_t)i * K + k]);
                float64x2_t dc = vld1q_f64(&dLdC[(size_t)i * N + jj]);
                grad = vmlaq_f64(grad, a, dc);
            }
            // Apply update while grad is still a register
            float64x2_t w = vld1q_f64(&W[(size_t)k * N + jj]);
            float64x2_t lr_v = {lr, lr};
            w = vsubq_f64(w, vmulq_f64(lr_v, grad));
            vst1q_f64(&W[(size_t)k * N + jj], w);
        }
        #else
        for (int64_t i = 0; i < M; i++) {
            double sum = 0;
            for (int64_t j = 0; j < N; j++)
                sum += dLdC[(size_t)i * N + j] * W[(size_t)k * N + j];
            dLdA[(size_t)i * K + k] = sum;
        }
        for (int64_t j = 0; j < N; j++) {
            double grad = 0;
            for (int64_t i = 0; i < M; i++)
                grad += A[(size_t)i * K + k] * dLdC[(size_t)i * N + j];
            W[(size_t)k * N + j] -= lr * grad;
        }
        #endif
    }
}

void fao_bwd_sgd_momentum(int M, int K, int N,
                          const double* dLdC, const double* A,
                          double* W,
                          double* dLdA,
                          double* velocity,
                          double lr, double momentum) {
    for (int64_t k = 0; k < K; k++) {
        // dL/dA for this k
        for (int64_t i = 0; i < M; i++) {
            double sum = 0.0;
            for (int64_t j = 0; j < N; j++)
                sum += dLdC[(size_t)i * N + j] * W[(size_t)k * N + j];
            dLdA[(size_t)i * K + k] = sum;
        }
        // Fused grad + momentum + update
        for (int64_t j = 0; j < N; j++) {
            double grad = 0.0;
            for (int64_t i = 0; i < M; i++)
                grad += A[(size_t)i * K + k] * dLdC[(size_t)i * N + j];
            double v = momentum * velocity[(size_t)k * N + j] + lr * grad;
            velocity[(size_t)k * N + j] = v;
            W[(size_t)k * N + j] -= v;
        }
    }
}

// =========================================================================
// Sparse matmul
// =========================================================================

double fao_sparsity_mask(int64_t n, const double* x, uint8_t* mask) {
    int64_t nz = 0;
    for (int64_t i = 0; i < n; i++) {
        mask[i] = x[i] > 0.0 ? 1 : 0;
        nz += mask[i];
    }
    return (double)nz / (double)n;
}

int fao_matmul_sparse(int M, int K, int N,
                      const double* A, const double* B, double* C,
                      const uint8_t* nonzero_mask) {
    memset(C, 0, (size_t)M * N * sizeof(double));
    int cols_used = 0;
    for (int64_t k = 0; k < K; k++) {
        if (!nonzero_mask[k]) continue;
        cols_used++;
        for (int64_t i = 0; i < M; i++) {
            double a_val = A[(size_t)i * K + k];
            if (a_val == 0.0) continue;
            for (int64_t j = 0; j < N; j++)
                C[(size_t)i * N + j] += a_val * B[(size_t)k * N + j];
        }
    }
    return cols_used;
}

// =========================================================================
// Mixed precision conversion
// =========================================================================

void fao_convert_f32_to_f64(int64_t n, const float* src, double* dst) {
    for (int64_t i = 0; i < n; i++) dst[i] = (double)src[i];
}

void fao_convert_f64_to_f32(int64_t n, const double* src, float* dst) {
    for (int64_t i = 0; i < n; i++) dst[i] = (float)src[i];
}

// =========================================================================
// Optimizer state
// =========================================================================

FAOptimState* fao_optim_create(int64_t size) {
    FAOptimState* s = (FAOptimState*)calloc(1, sizeof(FAOptimState));
    s->size = size;
    s->velocity = (double*)calloc((size_t)size, sizeof(double));
    return s;
}

void fao_optim_destroy(FAOptimState* s) {
    if (!s) return;
    free(s->velocity);
    free(s);
}

// =========================================================================
// High-level: one-call training step
// =========================================================================

void fao_linear_train_step(int M, int K, int N,
                           const double* A,
                           double* W,
                           const double* bias,
                           double* y,
                           const double* target,
                           double lr) {
    // Forward: y = A @ W + bias  (with ReLU)
    for (int64_t i = 0; i < M; i++)
        for (int64_t j = 0; j < N; j++) {
            double sum = 0.0;
            for (int64_t k = 0; k < K; k++)
                sum += A[(size_t)i * K + k] * W[(size_t)k * N + j];
            if (bias) sum += bias[j];
            y[(size_t)i * N + j] = sum > 0.0 ? sum : 0.0; // ReLU
        }

    // dL/dy = 2 * (y - target) / M
    double* dLdy = (double*)malloc((size_t)M * N * sizeof(double));
    for (int64_t i = 0; i < M * N; i++)
        dLdy[i] = 2.0 * (y[i] - target[i]) / M;

    // Fused backward + SGD: compute dL/dW and update W in one pass
    // Since we only need to update W, skip dL/dA computation
    for (int64_t k = 0; k < K; k++) {
        for (int64_t j = 0; j < N; j++) {
            double grad = 0.0;
            for (int64_t i = 0; i < M; i++)
                grad += A[(size_t)i * K + k] * dLdy[(size_t)i * N + j];
            W[(size_t)k * N + j] -= lr * grad;
        }
    }

    free(dLdy);
}

// =========================================================================
// Configuration & stats
// =========================================================================

static int _enable_neon = 1;
static int _enable_mt = 1;
static int _enable_sparse = 1;
static int _thread_count = 0;

void fao_enable_neon(int enabled) { _enable_neon = enabled; }
void fao_enable_mt(int enabled) { _enable_mt = enabled; }
void fao_enable_sparse(int enabled) { _enable_sparse = enabled; }
void fao_set_thread_count(int n) { _thread_count = n; }

void fao_print_config(void) {
    printf("--- FA Optimizer Config ---\n");
    printf("NEON: %s\n", fao_has_neon() ? "yes" : "no");
    printf("AVX:  %s\n", fao_has_avx() ? "yes" : "no");
    printf("Cores: %d\n", fao_core_count());
    printf("Thread pool: %d threads\n", _thread_count > 0 ? _thread_count : fao_core_count() - 1);
    printf("Optimizations: NEON=%d MT=%d Sparse=%d\n", _enable_neon, _enable_mt, _enable_sparse);
}
