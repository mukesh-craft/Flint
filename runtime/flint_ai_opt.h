#ifndef FLINT_AI_OPT_H
#define FLINT_AI_OPT_H

#include <stdint.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// =========================================================================
// Flint AI Optimizer — Advanced Optimizations
// =========================================================================
//
// Seven additional optimizations beyond the core v2 architecture:
//
// 1. NEON SIMD KERNELS   —  2-wide f64, 4-wide f32 via ARM NEON
//    Fused matmul micro-kernel processes 2 rows × 2 cols per iteration.
//    2× throughput on f64, 4× on f32 vs scalar.
//
// 2. MULTI-THREADED EXECUTION  —  full core utilisation
//    Auto-dispatches to thread pool for matmuls > 200×200.
//    Near-linear speedup on 8 cores for large matrices.
//
// 3. AUTO-TUNED KERNEL DISPATCH  —  shape-adaptive
//    Picks scalar/tiled/SIMD/multi-thread based on matrix dimensions
//    and hardware capabilities at runtime.
//
// 4. FUSED BACKWARD + OPTIMIZER STEP  —  update weights in cache
//    Compute dL/dW AND apply SGD/momentum update in the same pass.
//    Saves 2 memory round-trips per parameter per step.
//
// 5. SPARSE ACTIVATION HANDLING  —  skip ReLU zeros
//    Track non-zero mask from ReLU output, skip zero entries in
//    subsequent matmul. Typical 50-80% sparsity after ReLU.
//
// 6. MIXED PRECISION (f32)  —  half memory, double throughput
//    Full f32 tensor support alongside f64. f32 matmul is 4-wide SIMD
//    vs 2-wide for f64. Half the memory bandwidth.
//
// 7. VENDOR-INDEPENDENT C API  —  use from PyTorch/TF/NumPy via FFI
//    Simple flat-buffer API: {M,K,N, data pointers, flags}.
//    Any language with C FFI can call Flint-optimized kernels.
//
// All optimisations are auto-selected. No manual tuning required.
// =========================================================================

// =========================================================================
// Hardware detection
// =========================================================================
int fao_has_neon(void);
int fao_has_avx(void);
int fao_core_count(void);

// =========================================================================
// Kernel dispatch — auto-selects best implementation
// =========================================================================

// C = A @ B  (auto-dispatched: scalar / tiled / SIMD / multi-threaded)
void fao_matmul(int M, int K, int N,
                const double* A, const double* B, double* C);

// C = A @ B + bias  (with optional ReLU fused in)
void fao_fused_linear(int M, int K, int N,
                      const double* A, const double* W, const double* bias,
                      double* C, int has_relu);

// =========================================================================
// SIMD kernels (raw flat buffers, no tensor objects)
// =========================================================================

// ARM NEON f64: 2-wide SIMD, tiled for cache
void fao_matmul_neon_f64(int M, int K, int N,
                         const double* A, const double* B, double* C);

// ARM NEON f32: 4-wide SIMD, 2× throughput of f64
void fao_matmul_neon_f32(int M, int K, int N,
                         const float* A, const float* B, float* C);

// Fused NEON: C = relu(A @ B + bias) in one pass (f64)
void fao_fused_neon_f64(int M, int K, int N,
                        const double* A, const double* W,
                        const double* bias, double* C, int has_relu);

// =========================================================================
// Multi-threaded kernels
// =========================================================================

// Threaded matmul: divides rows across cores
void fao_matmul_mt(int M, int K, int N,
                   const double* A, const double* B, double* C);

// Threaded + fused: divides rows, each thread does SIMD+tile internally
void fao_fused_mt(int M, int K, int N,
                  const double* A, const double* W, const double* bias,
                  double* C, int has_relu);

// =========================================================================
// Fused backward + optimizer
// =========================================================================

// Compute dL/dA and dL/dW from dL/dC, A, and W
// dL/dA = dL/dC @ W^T  (M×K out)
// dL/dW = A^T @ dL/dC  (K×N out)
void fao_matmul_bwd(int M, int K, int N,
                    const double* dLdC, const double* A, const double* W,
                    double* dLdA, double* dLdW);

// Fused backward + SGD update:
// 1. Compute dL/dW = A^T @ dL/dC  (in cache tiles)
// 2. Apply W -= lr * dL/dW  (before dL/dW leaves cache)
// Saves 1 write + 1 read per parameter per step
void fao_bwd_sgd_step(int M, int K, int N,
                      const double* dLdC, const double* A,
                      double* W,           // updated in-place
                      double* dLdA,
                      double lr);

// Fused backward + SGD+momentum:
// v = momentum * v + lr * dL/dW   (velocity kept in cache)
// W -= v
void fao_bwd_sgd_momentum(int M, int K, int N,
                          const double* dLdC, const double* A,
                          double* W,
                          double* dLdA,
                          double* velocity,  // per-parameter velocity buffer
                          double lr, double momentum);

// =========================================================================
// Sparse matmul (skip ReLU zeros)
// =========================================================================

// C = A @ B, skipping columns of A where mask is 0
// mask[j] = 1 means column j of A is non-zero
// Returns number of non-zero columns processed
int fao_matmul_sparse(int M, int K, int N,
                      const double* A, const double* B, double* C,
                      const uint8_t* nonzero_mask);

// Extract ReLU sparsity mask: mask[i] = 1 if x[i] > 0
// Returns sparsity fraction (0.0 = all zeros, 1.0 = all non-zero)
double fao_sparsity_mask(int64_t n, const double* x, uint8_t* mask);

// =========================================================================
// Mixed precision conversion
// =========================================================================

// Convert f64 ↔ f32 (used when switching precision modes)
void fao_convert_f32_to_f64(int64_t n, const float* src, double* dst);
void fao_convert_f64_to_f32(int64_t n, const double* src, float* dst);

// f32 fused linear kernel (for mixed-precision training)
void fao_fused_linear_f32(int M, int K, int N,
                          const float* A, const float* W,
                          const float* bias, float* C, int has_relu);

// =========================================================================
// Thread pool
// =========================================================================

typedef struct FAThreadPool FAThreadPool;
FAThreadPool* fao_pool_create(int n_threads);
void fao_pool_destroy(FAThreadPool* pool);

// Run func(i, arg) for i in [start, end) in parallel
void fao_pool_for(FAThreadPool* pool, int64_t start, int64_t end,
                  void (*func)(int64_t i, void* arg), void* arg);

// Get global pool (created on first call, destroyed at exit)
FAThreadPool* fao_global_pool(void);

// =========================================================================
// Optimizer state (for use inside fused bwd+optimizer)
// =========================================================================

typedef struct {
    double lr;
    double momentum;
    double weight_decay;
} FAOptimConfig;

typedef struct {
    double* velocity;   // per-element velocity buffer
    int64_t size;
} FAOptimState;

FAOptimState* fao_optim_create(int64_t size);
void fao_optim_destroy(FAOptimState* s);

// =========================================================================
// High-level: one-call training step
// =========================================================================

// Complete forward+backward+update for a linear layer:
//   y = A @ W^T + bias
//   loss = mse(y, target)
//   W -= lr * dL/dW  (fused into backward)
//
// All pointer-based, no tensor objects. Usable from any C/C++ library.
void fao_linear_train_step(int M, int K, int N,
                           const double* A,       // input (M×K)
                           double* W,             // weight (K×N) — updated
                           const double* bias,    // bias (N)
                           double* y,             // output (M×N)
                           const double* target,  // target (M×N)
                           double lr);

// =========================================================================
// Configuration & stats
// =========================================================================

// Enable/disable specific optimizations at runtime
void fao_enable_neon(int enabled);
void fao_enable_mt(int enabled);
void fao_enable_sparse(int enabled);
void fao_set_thread_count(int n);

// Print hardware and optimization status
void fao_print_config(void);

#ifdef __cplusplus
}
#endif

#endif // FLINT_AI_OPT_H
