#ifndef FLINT_AI_H
#define FLINT_AI_H

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

// =========================================================================
// Flint AI Engine - Architecture v2
// =========================================================================
//
// Six innovations that make AI training cheaper:
//
// 1. ARENA ALLOCATOR  —  zero malloc/free per op
//    Single pre-allocated arena. Tensor allocations are bump-pointer.
//    Entire arena can be reset in O(1) between forward/backward passes.
//    Eliminates the #1 overhead in C/C++ AI frameworks: malloc churn.
//
// 2. TILED FUSED KERNELS  —  3x fewer memory passes
//    Instead of: matmul → write → read+bias → write → read+relu → write
//    We do:      matmul tile in cache → add bias → apply relu → write ONCE
//    Saves 3× memory bandwidth. Critical on memory-bound ARM systems.
//
// 3. ZERO-COPY TENSOR VIEWS  —  no data copy for reshape/transpose
//    reshape, transpose, slice, squeeze operate via {data, strides, offset}.
//    Only materialize contiguous buffers when matmul or export requires it.
//
// 4. MEMORY POOL  —  buffer reuse across operations
//    Dead tensor buffers are returned to a pool. Next alloc of same size
//    reuses the buffer. Eliminates fragmentation and reuse latency.
//
// 5. SMART BUFFER REUSE  —  compile-style last-use optimization
//    Track when a tensor's buffer is last needed. Reuse it immediately
//    for the next operation. Like Rust ownership at runtime.
//
// 6. ACTIVATION CHECKPOINTING  —  O(√L) memory for L layers
//    Store only checkpoint tensors during forward. Recompute activations
//    during backward. Memory: O(n) → O(√n) with ~1.33× compute overhead.
//
// Together: faster than C, leaner than C++, simpler than both.
// =========================================================================

// =========================================================================
// Configuration
// =========================================================================
#ifndef FA_ARENA_SIZE
#define FA_ARENA_SIZE (512LU * 1024 * 1024)  // 512 MB arena by default
#endif
#ifndef FA_POOL_SLOTS
#define FA_POOL_SLOTS 64   // max pooled buffer sizes
#endif
#ifndef FA_TILE_M
#define FA_TILE_M 64       // matmul tile rows (tuned for L1 cache)
#endif
#ifndef FA_TILE_N
#define FA_TILE_N 64       // matmul tile cols
#endif
#ifndef FA_TILE_K
#define FA_TILE_K 256      // matmul tile depth (tuned for L2 cache)
#endif

// =========================================================================
// Arena Allocator
// =========================================================================
// A bump-pointer arena that never frees individual allocations.
// Entire arena is reset at once (e.g., between training steps).
// Grows on demand by mmap'ing new 64 MB chunks.
// =========================================================================
typedef struct {
    char*    base;          // start of current chunk
    char*    current;       // next allocation address
    size_t   remaining;     // bytes left in current chunk
    size_t   chunk_size;    // size of each chunk
    // chunk list (singly linked through first 8 bytes)
    struct FAChunk* first;
    struct FAChunk* last;
} FArena;

void  farena_init(FArena* a, size_t initial_size);
void  farena_destroy(FArena* a);
void* farena_alloc(FArena* a, size_t size);
void  farena_reset(FArena* a);
size_t farena_used(FArena* a);

// =========================================================================
// Memory Pool
// =========================================================================
// Caches freed tensor data buffers by size class.
// Returns buffers to pool on destroy, serves from pool on alloc.
// =========================================================================
typedef struct {
    void** buffers[FA_POOL_SLOTS];   // stacks of buffers per size class
    int    counts[FA_POOL_SLOTS];
    size_t slot_sizes[FA_POOL_SLOTS];
    int    n_slots;
} FMemPool;

void  fpool_init(FMemPool* p);
void  fpool_destroy(FMemPool* p);
void* fpool_alloc(FMemPool* p, size_t bytes);
void  fpool_free(FMemPool* p, void* ptr, size_t bytes);

// =========================================================================
// Tensor (v2)
// =========================================================================
typedef struct {
    double*  data;
    int64_t* shape;
    int64_t* strides;
    int64_t  ndim;
    int64_t  offset;        // element offset into data (for views)
    int64_t  size;          // total elements
    uint8_t  owns_data : 1; // 1 = allocated from arena/pool
    uint8_t  is_contiguous : 1; // 1 = contiguous layout
    uint8_t  is_view : 1;       // 1 = view (no data ownership)
    // Source tracking for buffer reuse
    int64_t  last_use_step; // training step when buffer is last needed
    // Autograd
    double*  grad;
    int64_t  grad_id;
    int64_t  grad_size;
    uint8_t  is_checkpoint : 1; // 1 = checkpoint for recompute
    // Allocator source
    FArena*  arena;         // arena this data came from (or NULL)
    size_t   data_bytes;    // size of data buffer in bytes
} FATensor;

// =========================================================================
// Tensor API (v2) — all allocations come from arena
// =========================================================================
FATensor* fa_create(FArena* a, int64_t ndim, int64_t* shape);
FATensor* fa_create_like(FArena* a, FATensor* t);
void      fa_destroy(FATensor* t);
void      fa_fill(FATensor* t, double v);
void      fa_copy(FATensor* dst, FATensor* src);
void      fa_randn(FATensor* t);
void      fa_uniform(FATensor* t, double lo, double hi);
double    fa_get(FATensor* t, int64_t idx);
void      fa_set(FATensor* t, int64_t idx, double v);

// =========================================================================
// Fused Kernels
// =========================================================================
// These are the core innovation. Each fused kernel performs multiple
// operations in a single tiled pass, dramatically reducing memory bandwidth.
// =========================================================================

// out = relu(a @ b + bias)   — single fused pass
FATensor* fa_matmul_bias_relu(FArena* a, FATensor* x, FATensor* w,
                              FATensor* bias);

// out = a @ b + bias          — fused matmul + bias
FATensor* fa_matmul_bias(FArena* a, FATensor* x, FATensor* w,
                         FATensor* bias);

// out = a @ b                 — tiled matmul (same math, better cache)
FATensor* fa_matmul(FArena* a, FATensor* x, FATensor* w);

// out = max(0, x)             — element-wise with write combining
FATensor* fa_relu(FArena* a, FATensor* x);

// out = sigmoid(x)            — fused sigmoid
FATensor* fa_sigmoid(FArena* a, FATensor* x);

// out = x + y                 — element-wise add (broadcast)
FATensor* fa_add(FArena* a, FATensor* x, FATensor* y);

// out = x * y                 — element-wise mul (broadcast)
FATensor* fa_mul(FArena* a, FATensor* x, FATensor* y);

// =========================================================================
// Zero-Copy Views
// =========================================================================
// These operations create views into existing tensor data WITHOUT copying.
// The view shares the same underlying data buffer and arena.
// =========================================================================

// Reshape: change shape without copy (only if size matches)
FATensor* fa_reshape(FATensor* t, int64_t ndim, int64_t* shape);

// Transpose: swap two dims via stride manipulation (no data copy)
FATensor* fa_transpose(FATensor* t, int64_t dim0, int64_t dim1);

// Slice: extract a sub-tensor (still shares parent data)
FATensor* fa_slice(FATensor* t, int64_t dim, int64_t start, int64_t end);

// Squeeze: remove dims of size 1
FATensor* fa_squeeze(FATensor* t);

// Unsqueeze: add a dim of size 1
FATensor* fa_unsqueeze(FATensor* t, int64_t dim);

// Materialize: if view, make contiguous copy (needed before matmul)
FATensor* fa_contiguous(FArena* a, FATensor* t);

// =========================================================================
// Buffer Reuse Manager
// =========================================================================
// Tracks when each tensor's buffer is last needed and reclaims eagerly.
// =========================================================================
typedef struct {
    int64_t current_step;       // global step counter
    FMemPool pool;
    // Simple tracking: array of {data_ptr, step_freed}
    #define FA_TRACK_MAX 4096
    struct {
        double* data;
        int64_t step;
        size_t  bytes;
    } freed[FA_TRACK_MAX];
    int freed_count;
} FReuseMan;

void  freuse_init(FReuseMan* r);
void  freuse_destroy(FReuseMan* r);
void  freuse_mark_step(FReuseMan* r);
// Try to get a buffer from reuse; returns NULL if not available
double* freuse_claim(FReuseMan* r, size_t min_bytes);
void  freuse_release(FReuseMan* r, double* data, size_t bytes);

// =========================================================================
// Activation Checkpointing
// =========================================================================

// Segment of the compute graph that can be recomputed
typedef struct FCheckpoint {
    struct FCheckpoint* next;
    FATensor** inputs;          // inputs to this segment
    int n_inputs;
    FATensor** outputs;         // outputs from this segment
    int n_outputs;
    void (*forward_fn)(void** inputs, void** outputs, FArena* arena);
    int64_t step_recorded;
} FCheckpoint;

void fcheckpoint_init(void);
void fcheckpoint_record(FATensor** inputs, int n_in,
                        FATensor** outputs, int n_out,
                        void (*fn)(void**, void**, FArena*));
void fcheckpoint_backward(FArena* a);

// =========================================================================
// High-level layer API (for easy use from Flint)
// =========================================================================

// Linear layer: out = activation(x @ w.T + bias)
// If activation is NULL, uses no activation (identity)
FATensor* fa_linear(FArena* a, FATensor* x, FATensor* w, FATensor* bias,
                    const char* activation);

// Sequential model helper: runs ops in order
typedef FATensor* (*FALayerFn)(FArena*, FATensor*);
FATensor* fa_sequential(FArena* a, FATensor* x,
                        FALayerFn* layers, int n_layers);

// =========================================================================
// Debug / Stats
// =========================================================================
void fa_print_tensor(FATensor* t, int64_t max_elems);
void fa_print_shape(FATensor* t);
void fa_print_stats(void);  // prints arena usage, pool hits, etc.

// Global arena (for convenience)
extern FArena fa_default_arena;

#ifdef __cplusplus
}
#endif

#endif // FLINT_AI_H
