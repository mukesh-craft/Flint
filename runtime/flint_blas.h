// flint_blas.h — BLAS backend for tensor matmul (OpenBLAS).
//
// Why a runtime dispatch instead of the previous `#ifdef FLINT_USE_BLAS`:
// that macro was defined by nothing in the tree, so the branch was dead and
// matmul was permanently the scalar triple loop. Here the backend is chosen
// once, at first use, from a weak symbol probe -- so a build linked against
// OpenBLAS uses it, and a build without it still links and still runs.
//
// Linking: add -lopenblas to use it. Without that flag the weak probe resolves
// to NULL and flt_matmul uses the portable triple loop. No #define needed.
#ifndef FLINT_BLAS_H
#define FLINT_BLAS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 1 when a BLAS dgemm was found and matmul will use it, 0 otherwise.
// Resolved on first call; cheap thereafter.
int flint_blas_available(void);

// Human-readable backend name for diagnostics ("openblas" / "portable").
const char* flint_blas_backend(void);

// Number of times the BLAS path has served a matmul (0 while portable).
int64_t flint_blas_calls(void);

// Force the portable path regardless of what was linked. Test hook; also the
// escape hatch if a BLAS build ever produces wrong answers on some target.
void flint_blas_disable(void);

// Row-major C[MxN] = A[MxK] * B[KxN]. Dispatches to BLAS when present, else to
// a NEON-accelerated portable loop. Leading dimensions are derived from M/K/N
// here in one place -- getting B's ldb wrong yields plausible garbage.
void flint_blas_matmul(int64_t M, int64_t K, int64_t N,
                       const double* A, const double* B, double* C);

// f32 variant. On aarch64 this is where the real win is: half the bytes
// through memory and, with OpenBLAS, a tuned single-precision kernel.
// The leading dimensions are the same trap as the f64 path -- for a KxN B the
// stride is N, not K.
void flint_blas_matmul_f32(int64_t M, int64_t K, int64_t N,
                           const float* A, const float* B, float* C);
void flint_blas_matmul_f32_reference(int64_t M, int64_t K, int64_t N,
                                     const float* A, const float* B, float* C);

// Portable triple loop, always. The test gate compares the live path against
// this so a BLAS build that disagrees fails instead of shipping.
void flint_blas_matmul_reference(int64_t M, int64_t K, int64_t N,
                                 const double* A, const double* B, double* C);

#ifdef __cplusplus
}
#endif

#endif // FLINT_BLAS_H