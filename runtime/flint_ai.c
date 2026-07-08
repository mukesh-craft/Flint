#include "flint_ai.h"
#include <float.h>
#include <assert.h>

// =========================================================================
// Internal helpers
// =========================================================================

static int64_t _calc_size(int64_t ndim, int64_t* shape) {
    int64_t n = 1;
    for (int64_t i = 0; i < ndim; i++) n *= shape[i];
    return n;
}

static int64_t* _shape_copy(FArena* a, int64_t ndim, int64_t* shape) {
    int64_t* s = farena_alloc(a, (size_t)ndim * sizeof(int64_t));
    memcpy(s, shape, (size_t)ndim * sizeof(int64_t));
    return s;
}

static int64_t* _default_strides(int64_t ndim, int64_t* shape) {
    // Not arena-allocated — caller must manage lifetime
    int64_t* s = malloc((size_t)ndim * sizeof(int64_t));
    if (ndim > 0) {
        s[ndim - 1] = 1;
        for (int64_t d = ndim - 2; d >= 0; d--)
            s[d] = s[d + 1] * shape[d + 1];
    }
    return s;
}

static int64_t* _contiguous_strides_arena(FArena* a, int64_t ndim, int64_t* shape) {
    int64_t* s = farena_alloc(a, (size_t)ndim * sizeof(int64_t));
    if (ndim > 0) {
        s[ndim - 1] = 1;
        for (int64_t d = ndim - 2; d >= 0; d--)
            s[d] = s[d + 1] * shape[d + 1];
    }
    return s;
}

static int64_t _flat_idx(FATensor* t, int64_t flat) {
    if (t->is_contiguous) return t->offset + flat;
    int64_t idx = t->offset;
    for (int64_t d = (int64_t)t->ndim - 1; d >= 0; d--) {
        int64_t pos = flat % t->shape[d];
        idx += pos * t->strides[d];
        flat /= t->shape[d];
    }
    return idx;
}

// =========================================================================
// Arena Allocator
// =========================================================================

typedef struct FAChunk {
    struct FAChunk* next;
    size_t size;
    char data[];
} FAChunk;

static FAChunk* _chunk_alloc(size_t size) {
    FAChunk* c = (FAChunk*)malloc(sizeof(FAChunk) + size);
    if (!c) return NULL;
    c->next = NULL;
    c->size = size;
    return c;
}

void farena_init(FArena* a, size_t initial_size) {
    if (initial_size < 65536) initial_size = 65536;
    FAChunk* c = _chunk_alloc(initial_size);
    a->first = c;
    a->last = c;
    a->base = c->data;
    a->current = c->data;
    a->remaining = initial_size;
    a->chunk_size = initial_size > 67108864 ? initial_size : 67108864; // 64 MB default
}

void farena_destroy(FArena* a) {
    FAChunk* c = a->first;
    while (c) {
        FAChunk* next = c->next;
        free(c);
        c = next;
    }
    a->first = a->last = NULL;
    a->base = a->current = NULL;
    a->remaining = 0;
}

void* farena_alloc(FArena* a, size_t size) {
    if (size == 0) size = 8; // minimum alignment chunk
    size = (size + 15) & ~15; // align to 16 bytes
    if (size > a->remaining) {
        // Allocate new chunk
        size_t new_size = size > a->chunk_size ? size : a->chunk_size;
        FAChunk* c = _chunk_alloc(new_size);
        if (!c) return NULL;
        a->last->next = c;
        a->last = c;
        a->base = c->data;
        a->current = c->data;
        a->remaining = new_size;
    }
    void* ptr = a->current;
    a->current += size;
    a->remaining -= size;
    return ptr;
}

void farena_reset(FArena* a) {
    // Keep first chunk, reset to its start
    a->current = a->first->data;
    a->remaining = a->first->size;
    a->base = a->first->data;
    // Free all but first chunk
    FAChunk* c = a->first->next;
    while (c) {
        FAChunk* next = c->next;
        free(c);
        c = next;
    }
    a->first->next = NULL;
    a->last = a->first;
}

size_t farena_used(FArena* a) {
    return (size_t)(a->current - a->base);
}

// =========================================================================
// Memory Pool
// =========================================================================

static int _pool_slot(FMemPool* p, size_t bytes) {
    for (int i = 0; i < p->n_slots; i++)
        if (p->slot_sizes[i] == bytes) return i;
    // Add new slot
    if (p->n_slots >= FA_POOL_SLOTS) return -1;
    int idx = p->n_slots++;
    p->slot_sizes[idx] = bytes;
    p->buffers[idx] = NULL;
    p->counts[idx] = 0;
    return idx;
}

void fpool_init(FMemPool* p) {
    memset(p, 0, sizeof(FMemPool));
}

void fpool_destroy(FMemPool* p) {
    for (int i = 0; i < p->n_slots; i++) {
        for (int j = 0; j < p->counts[i]; j++)
            free(p->buffers[i][j]);
        free(p->buffers[i]);
    }
}

void* fpool_alloc(FMemPool* p, size_t bytes) {
    int slot = _pool_slot(p, bytes);
    if (slot >= 0 && p->counts[slot] > 0) {
        p->counts[slot]--;
        return p->buffers[slot][p->counts[slot]];
    }
    return malloc(bytes);
}

void fpool_free(FMemPool* p, void* ptr, size_t bytes) {
    if (!ptr) return;
    int slot = _pool_slot(p, bytes);
    if (slot >= 0 && p->counts[slot] < FA_POOL_SLOTS) {
        // Grow stack
        if (p->counts[slot] == 0) {
            p->buffers[slot] = malloc(FA_POOL_SLOTS * sizeof(void*));
        }
        p->buffers[slot][p->counts[slot]++] = ptr;
    } else {
        free(ptr);
    }
}

// =========================================================================
// Tensor creation / destruction
// =========================================================================

FATensor* fa_create(FArena* a, int64_t ndim, int64_t* shape) {
    FATensor* t = (FATensor*)farena_alloc(a, sizeof(FATensor));
    t->ndim = ndim;
    t->shape = _shape_copy(a, ndim, shape);
    t->strides = _contiguous_strides_arena(a, ndim, shape);
    t->size = _calc_size(ndim, shape);
    t->offset = 0;
    t->owns_data = 1;
    t->is_contiguous = 1;
    t->is_view = 0;
    t->last_use_step = -1;
    t->grad = NULL;
    t->grad_id = -1;
    t->grad_size = 0;
    t->is_checkpoint = 0;
    t->arena = a;
    t->data_bytes = (size_t)t->size * sizeof(double);
    // Allocate data from arena
    t->data = (double*)farena_alloc(a, t->data_bytes);
    memset(t->data, 0, t->data_bytes);
    return t;
}

FATensor* fa_create_like(FArena* a, FATensor* t) {
    return fa_create(a, t->ndim, t->shape);
}

void fa_destroy(FATensor* t) {
    // Nothing to free — arena handles it
    (void)t;
}

void fa_fill(FATensor* t, double v) {
    if (t->is_contiguous) {
        for (int64_t i = 0; i < t->size; i++)
            t->data[t->offset + i] = v;
    } else {
        for (int64_t i = 0; i < t->size; i++)
            t->data[_flat_idx(t, i)] = v;
    }
}

void fa_copy(FATensor* dst, FATensor* src) {
    int64_t n = dst->size < src->size ? dst->size : src->size;
    if (dst->is_contiguous && src->is_contiguous) {
        memcpy(dst->data + dst->offset, src->data + src->offset,
               (size_t)n * sizeof(double));
    } else {
        for (int64_t i = 0; i < n; i++)
            dst->data[_flat_idx(dst, i)] = src->data[_flat_idx(src, i)];
    }
}

void fa_randn(FATensor* t) {
    for (int64_t i = 0; i < t->size; i++)
        t->data[_flat_idx(t, i)] = (double)rand() / RAND_MAX * 2.0 - 1.0;
}

void fa_uniform(FATensor* t, double lo, double hi) {
    for (int64_t i = 0; i < t->size; i++)
        t->data[_flat_idx(t, i)] = lo + (hi - lo) * (double)rand() / RAND_MAX;
}

double fa_get(FATensor* t, int64_t idx) {
    return t->data[_flat_idx(t, idx)];
}

void fa_set(FATensor* t, int64_t idx, double v) {
    t->data[_flat_idx(t, idx)] = v;
}

// =========================================================================
// Tiled Fused Kernels
// =========================================================================

// Core fused kernel: out = relu(a @ b + bias)
// Implemented as a blocked/tiled matmul with bias-add and relu fused
// into the write-back path. This saves 3 memory passes:
//   before: matmul(A,B) → tmp1, add(tmp1,bias) → tmp2, relu(tmp2) → out
//   after:  fused_matmul_bias_relu(A,B,bias) → out
FATensor* fa_matmul_bias_relu(FArena* a, FATensor* x, FATensor* w,
                              FATensor* bias) {
    assert(x->ndim == 2 && w->ndim == 2);
    int64_t M = x->shape[0], K = x->shape[1], N = w->shape[1];
    assert(w->shape[0] == K);
    int64_t out_shape[2] = {M, N};
    FATensor* out = fa_create(a, 2, out_shape);
    double* A = x->data + x->offset;
    double* B = w->data + w->offset;
    double* bias_d = bias ? (bias->data + bias->offset) : NULL;
    double* C = out->data;

    // Tiled execution
    #ifdef FA_TILE_M
    int tm = FA_TILE_M;
    #else
    int tm = 64;
    #endif
    #ifdef FA_TILE_N
    int tn = FA_TILE_N;
    #else
    int tn = 64;
    #endif
    #ifdef FA_TILE_K
    int tk = FA_TILE_K;
    #else
    int tk = 256;
    #endif

    // Use stack-based micro-tile accumulator
    for (int64_t i = 0; i < M; i += tm) {
        int64_t imax = i + tm < M ? i + tm : M;
        for (int64_t j = 0; j < N; j += tn) {
            int64_t jmax = j + tn < N ? j + tn : N;
            int64_t mh = imax - i;
            int64_t nw = jmax - j;

            // Zero the micro-tile accumulator (on stack)
            double tile[64][64];  // up to tm x tn (max 64x64 = 32 KB)
            for (int64_t ti = 0; ti < mh; ti++)
                for (int64_t tj = 0; tj < nw; tj++)
                    tile[ti][tj] = 0.0;

            // Compute matmul for this tile in K-strips
            for (int64_t kk = 0; kk < K; kk += tk) {
                int64_t kmax = kk + tk < K ? kk + tk : K;
                for (int64_t ti = 0; ti < mh; ti++) {
                    int64_t row = i + ti;
                    for (int64_t k = kk; k < kmax; k++) {
                        double a_val = A[row * K + k];
                        if (a_val == 0.0) continue; // skip zero (sparsity)
                        for (int64_t tj = 0; tj < nw; tj++) {
                            int64_t col = j + tj;
                            tile[ti][tj] += a_val * B[k * N + col];
                        }
                    }
                }
            }

            // Write tile: C = relu(tile + bias)
            for (int64_t ti = 0; ti < mh; ti++) {
                for (int64_t tj = 0; tj < nw; tj++) {
                    double v = tile[ti][tj];
                    if (bias_d) v += bias_d[j + tj];
                    C[(i + ti) * N + (j + tj)] = v > 0.0 ? v : 0.0;
                }
            }
        }
    }
    return out;
}

FATensor* fa_matmul_bias(FArena* a, FATensor* x, FATensor* w,
                         FATensor* bias) {
    assert(x->ndim == 2 && w->ndim == 2);
    int64_t M = x->shape[0], K = x->shape[1], N = w->shape[1];
    assert(w->shape[0] == K);
    int64_t out_shape[2] = {M, N};
    FATensor* out = fa_create(a, 2, out_shape);
    double* A = x->data + x->offset;
    double* B = w->data + w->offset;
    double* bias_d = bias ? (bias->data + bias->offset) : NULL;
    double* C = out->data;

    int tm = 64, tn = 64, tk = 256;

    for (int64_t i = 0; i < M; i += tm) {
        int64_t imax = i + tm < M ? i + tm : M;
        for (int64_t j = 0; j < N; j += tn) {
            int64_t jmax = j + tn < N ? j + tn : N;
            int64_t mh = imax - i;
            int64_t nw = jmax - j;

            double tile[64][64];
            for (int64_t ti = 0; ti < mh; ti++)
                for (int64_t tj = 0; tj < nw; tj++)
                    tile[ti][tj] = 0.0;

            for (int64_t kk = 0; kk < K; kk += tk) {
                int64_t kmax = kk + tk < K ? kk + tk : K;
                for (int64_t ti = 0; ti < mh; ti++) {
                    int64_t row = i + ti;
                    for (int64_t k = kk; k < kmax; k++) {
                        double a_val = A[row * K + k];
                        if (a_val == 0.0) continue;
                        for (int64_t tj = 0; tj < nw; tj++) {
                            int64_t col = j + tj;
                            tile[ti][tj] += a_val * B[k * N + col];
                        }
                    }
                }
            }

            for (int64_t ti = 0; ti < mh; ti++) {
                for (int64_t tj = 0; tj < nw; tj++) {
                    double v = tile[ti][tj];
                    if (bias_d) v += bias_d[j + tj];
                    C[(i + ti) * N + (j + tj)] = v;
                }
            }
        }
    }
    return out;
}

FATensor* fa_matmul(FArena* a, FATensor* x, FATensor* w) {
    return fa_matmul_bias(a, x, w, NULL);
}

FATensor* fa_relu(FArena* a, FATensor* x) {
    FATensor* out = fa_create_like(a, x);
    if (x->is_contiguous) {
        for (int64_t i = 0; i < x->size; i++) {
            double v = x->data[x->offset + i];
            out->data[i] = v > 0.0 ? v : 0.0;
        }
    } else {
        for (int64_t i = 0; i < x->size; i++) {
            double v = x->data[_flat_idx(x, i)];
            out->data[_flat_idx(out, i)] = v > 0.0 ? v : 0.0;
        }
    }
    return out;
}

FATensor* fa_sigmoid(FArena* a, FATensor* x) {
    FATensor* out = fa_create_like(a, x);
    for (int64_t i = 0; i < x->size; i++) {
        double v = x->data[_flat_idx(x, i)];
        out->data[_flat_idx(out, i)] = 1.0 / (1.0 + exp(-v));
    }
    return out;
}

FATensor* fa_add(FArena* a, FATensor* x, FATensor* y) {
    // Simple broadcast: assume compatible shapes
    FATensor* out = fa_create_like(a, x->size >= y->size ? x : y);
    int64_t n = out->size;
    for (int64_t i = 0; i < n; i++) {
        double xv = x->data[_flat_idx(x, i % x->size)];
        double yv = y->data[_flat_idx(y, i % y->size)];
        out->data[_flat_idx(out, i)] = xv + yv;
    }
    return out;
}

FATensor* fa_mul(FArena* a, FATensor* x, FATensor* y) {
    FATensor* out = fa_create_like(a, x->size >= y->size ? x : y);
    int64_t n = out->size;
    for (int64_t i = 0; i < n; i++) {
        out->data[_flat_idx(out, i)] =
            x->data[_flat_idx(x, i % x->size)] *
            y->data[_flat_idx(y, i % y->size)];
    }
    return out;
}

// =========================================================================
// Zero-Copy Views
// =========================================================================

FATensor* fa_reshape(FATensor* t, int64_t ndim, int64_t* shape) {
    int64_t new_size = _calc_size(ndim, shape);
    assert(new_size == t->size);
    if (!t->is_contiguous) {
        // Cannot reshape a non-contiguous view — materialize first
        return NULL; // caller should use fa_contiguous first
    }
    // Create view — shares same arena allocation
    FATensor* v = (FATensor*)farena_alloc(t->arena, sizeof(FATensor));
    v->data = t->data;
    v->shape = _shape_copy(t->arena, ndim, shape);
    v->strides = _contiguous_strides_arena(t->arena, ndim, shape);
    v->ndim = ndim;
    v->offset = t->offset;
    v->size = new_size;
    v->owns_data = 0;
    v->is_contiguous = 1;
    v->is_view = 1;
    v->last_use_step = t->last_use_step;
    v->grad = NULL;
    v->grad_id = -1;
    v->grad_size = 0;
    v->is_checkpoint = 0;
    v->arena = t->arena;
    v->data_bytes = t->data_bytes;
    return v;
}

FATensor* fa_transpose(FATensor* t, int64_t dim0, int64_t dim1) {
    assert(dim0 >= 0 && dim0 < t->ndim && dim1 >= 0 && dim1 < t->ndim);
    FATensor* v = (FATensor*)farena_alloc(t->arena, sizeof(FATensor));
    v->data = t->data;
    v->shape = _shape_copy(t->arena, t->ndim, t->shape);
    v->strides = _contiguous_strides_arena(t->arena, t->ndim, t->shape);
    v->ndim = t->ndim;
    v->offset = t->offset;
    v->size = t->size;
    v->owns_data = 0;
    v->is_view = 1;
    v->last_use_step = t->last_use_step;
    v->grad = NULL;
    v->grad_id = -1;
    v->grad_size = 0;
    v->is_checkpoint = 0;
    v->arena = t->arena;
    v->data_bytes = t->data_bytes;

    // Swap shape and stride
    int64_t tmp_s = v->shape[dim0];
    v->shape[dim0] = v->shape[dim1];
    v->shape[dim1] = tmp_s;
    int64_t tmp_str = v->strides[dim0];
    v->strides[dim0] = v->strides[dim1];
    v->strides[dim1] = tmp_str;

    // Check if still contiguous
    v->is_contiguous = 0;
    for (int64_t d = 0; d < v->ndim - 1; d++)
        if (v->strides[d] != v->strides[d + 1] * v->shape[d + 1]) goto not_contig;
    v->is_contiguous = 1;
    not_contig:;

    return v;
}

FATensor* fa_slice(FATensor* t, int64_t dim, int64_t start, int64_t end) {
    assert(dim >= 0 && dim < t->ndim);
    assert(start >= 0 && end <= t->shape[dim] && start < end);
    FATensor* v = (FATensor*)farena_alloc(t->arena, sizeof(FATensor));
    v->data = t->data;
    v->ndim = t->ndim;
    v->shape = _shape_copy(t->arena, t->ndim, t->shape);
    v->shape[dim] = end - start;
    v->strides = _contiguous_strides_arena(t->arena, t->ndim, t->shape);
    // Override stride for the sliced dim
    if (t->strides) v->strides[dim] = t->strides[dim];
    else v->strides[dim] = _calc_size(t->ndim - dim - 1, t->shape + dim + 1);
    v->offset = t->offset + start * v->strides[dim];
    v->size = _calc_size(v->ndim, v->shape);
    v->owns_data = 0;
    v->is_contiguous = 0; // may be contiguous; recompute
    v->is_view = 1;
    v->last_use_step = t->last_use_step;
    v->grad = NULL; v->grad_id = -1; v->grad_size = 0;
    v->is_checkpoint = 0;
    v->arena = t->arena;
    v->data_bytes = t->data_bytes;
    return v;
}

FATensor* fa_squeeze(FATensor* t) {
    int64_t new_ndim = 0;
    for (int64_t d = 0; d < t->ndim; d++)
        if (t->shape[d] != 1) new_ndim++;
    if (new_ndim == t->ndim) return t; // nothing to squeeze
    int64_t* new_shape = farena_alloc(t->arena, (size_t)new_ndim * sizeof(int64_t));
    int64_t* new_strides = farena_alloc(t->arena, (size_t)new_ndim * sizeof(int64_t));
    int64_t j = 0;
    for (int64_t d = 0; d < t->ndim; d++) {
        if (t->shape[d] != 1) {
            new_shape[j] = t->shape[d];
            if (t->strides) new_strides[j] = t->strides[d];
            else new_strides[j] = _calc_size(t->ndim - d - 1, t->shape + d + 1);
            j++;
        }
    }
    FATensor* v = (FATensor*)farena_alloc(t->arena, sizeof(FATensor));
    v->data = t->data;
    v->ndim = new_ndim;
    v->shape = new_shape;
    v->strides = new_strides;
    v->offset = t->offset;
    v->size = t->size;
    v->owns_data = 0; v->is_view = 1;
    v->is_contiguous = 0;
    v->last_use_step = t->last_use_step;
    v->grad = NULL; v->grad_id = -1;
    v->arena = t->arena;
    v->data_bytes = t->data_bytes;
    return v;
}

FATensor* fa_unsqueeze(FATensor* t, int64_t dim) {
    int64_t new_ndim = t->ndim + 1;
    int64_t* new_shape = farena_alloc(t->arena, (size_t)new_ndim * sizeof(int64_t));
    int64_t* new_strides = farena_alloc(t->arena, (size_t)new_ndim * sizeof(int64_t));
    int64_t j = 0;
    for (int64_t d = 0; d < new_ndim; d++) {
        if (d == dim) {
            new_shape[d] = 1;
            new_strides[d] = 0; // stride for size-1 dim doesn't matter
        } else {
            new_shape[d] = t->shape[j];
            new_strides[d] = t->strides ? t->strides[j] : 1;
            j++;
        }
    }
    FATensor* v = (FATensor*)farena_alloc(t->arena, sizeof(FATensor));
    v->data = t->data;
    v->ndim = new_ndim;
    v->shape = new_shape;
    v->strides = new_strides;
    v->offset = t->offset;
    v->size = t->size;
    v->owns_data = 0; v->is_view = 1;
    v->is_contiguous = 0;
    v->last_use_step = t->last_use_step;
    v->grad = NULL; v->grad_id = -1;
    v->arena = t->arena;
    v->data_bytes = t->data_bytes;
    return v;
}

FATensor* fa_contiguous(FArena* a, FATensor* t) {
    if (t->is_contiguous) return t;
    FATensor* out = fa_create(a, t->ndim, t->shape);
    for (int64_t i = 0; i < t->size; i++)
        out->data[i] = t->data[_flat_idx(t, i)];
    return out;
}

// =========================================================================
// Buffer Reuse Manager
// =========================================================================

void freuse_init(FReuseMan* r) {
    r->current_step = 0;
    fpool_init(&r->pool);
    r->freed_count = 0;
}

void freuse_destroy(FReuseMan* r) {
    fpool_destroy(&r->pool);
}

void freuse_mark_step(FReuseMan* r) {
    r->current_step++;
}

double* freuse_claim(FReuseMan* r, size_t min_bytes) {
    // Check pool first
    void* p = fpool_alloc(&r->pool, min_bytes);
    if (p) return (double*)p;
    // Check recently freed buffers
    for (int i = 0; i < r->freed_count; i++) {
        if (r->freed[i].bytes >= min_bytes) {
            double* data = r->freed[i].data;
            // Remove from list
            r->freed[i] = r->freed[r->freed_count - 1];
            r->freed_count--;
            return data;
        }
    }
    return NULL; // no reuse available
}

void freuse_release(FReuseMan* r, double* data, size_t bytes) {
    if (!data) return;
    fpool_free(&r->pool, data, bytes);
}

// =========================================================================
// High-level layer API
// =========================================================================

FATensor* fa_linear(FArena* a, FATensor* x, FATensor* w, FATensor* bias,
                    const char* activation) {
    if (activation && strcmp(activation, "relu") == 0) {
        return fa_matmul_bias_relu(a, x, w, bias);
    }
    FATensor* out = fa_matmul_bias(a, x, w, bias);
    if (activation && strcmp(activation, "sigmoid") == 0) {
        FATensor* act = fa_sigmoid(a, out);
        return act;
    }
    if (activation && strcmp(activation, "tanh") == 0) {
        // fa_tanh not implemented — use sigmoid for now
        return out;
    }
    return out;
}

FATensor* fa_sequential(FArena* a, FATensor* x,
                        FALayerFn* layers, int n_layers) {
    for (int i = 0; i < n_layers; i++)
        x = layers[i](a, x);
    return x;
}

// =========================================================================
// Stats
// =========================================================================
static size_t _total_arena_allocated = 0;
static size_t _total_pool_hits = 0;
static size_t _total_pool_misses = 0;

void fa_print_tensor(FATensor* t, int64_t max_elems) {
    int64_t n = t->size < max_elems ? t->size : max_elems;
    printf("[");
    for (int64_t i = 0; i < n; i++) {
        if (i > 0) printf(", ");
        printf("%f", fa_get(t, i));
    }
    if (n < t->size) printf(", ...");
    printf("]\n");
}

void fa_print_shape(FATensor* t) {
    printf("[");
    for (int64_t i = 0; i < t->ndim; i++) {
        if (i > 0) printf(" x ");
        printf("%ld", (long)t->shape[i]);
    }
    printf("]\n");
}

void fa_print_stats(void) {
    printf("--- FA Stats ---\n");
    printf("arena allocated: %zu bytes\n", _total_arena_allocated);
    printf("pool hits: %zu, misses: %zu\n", _total_pool_hits, _total_pool_misses);
}

// =========================================================================
// Global arena
// =========================================================================
FArena fa_default_arena;

// =========================================================================
// Activation Checkpointing
// =========================================================================

static FCheckpoint* _checkpoint_list = NULL;
static int _checkpoint_inited = 0;

void fcheckpoint_init(void) {
    _checkpoint_list = NULL;
    _checkpoint_inited = 1;
}

void fcheckpoint_record(FATensor** inputs, int n_in,
                        FATensor** outputs, int n_out,
                        void (*fn)(void**, void**, FArena*)) {
    if (!_checkpoint_inited) fcheckpoint_init();
    FCheckpoint* cp = (FCheckpoint*)malloc(sizeof(FCheckpoint));
    cp->inputs = inputs;
    cp->n_inputs = n_in;
    cp->outputs = outputs;
    cp->n_outputs = n_out;
    cp->forward_fn = fn;
    cp->next = _checkpoint_list;
    cp->step_recorded = 0;
    _checkpoint_list = cp;
    // Mark outputs as checkpoints
    for (int i = 0; i < n_out; i++)
        if (outputs[i]) outputs[i]->is_checkpoint = 1;
}

void fcheckpoint_backward(FArena* a) {
    // For now, stub — real implementation would:
    // 1. Walk checkpoints in reverse
    // 2. Restore inputs from checkpoint
    // 3. Re-run forward
    // 4. Compute gradients
    (void)a;
}
