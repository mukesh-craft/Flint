#include "flint_tensor.h"
#include "flint_blas.h"
#include <float.h>

// =========================================================================
// Internal helpers
// =========================================================================
static int64_t* _shape_copy(int64_t ndim, int64_t* shape) {
    int64_t* s = malloc(ndim * sizeof(int64_t));
    memcpy(s, shape, ndim * sizeof(int64_t));
    return s;
}

static int64_t _calc_size(int64_t ndim, int64_t* shape) {
    int64_t n = 1;
    for (int64_t i = 0; i < ndim; i++) n *= shape[i];
    return n;
}

static FlintTensor* _as_tensor(void* t) { return (FlintTensor*)t; }

static int64_t* _broadcast_shape(int64_t ndim_a, int64_t* shape_a,
                                   int64_t ndim_b, int64_t* shape_b,
                                   int64_t* out_ndim) {
    int64_t max_ndim = ndim_a > ndim_b ? ndim_a : ndim_b;
    int64_t* bs = calloc((size_t)max_ndim, sizeof(int64_t));
    for (int64_t i = 0; i < max_ndim; i++) {
        int64_t sa = i < ndim_a ? shape_a[ndim_a - 1 - i] : 1;
        int64_t sb = i < ndim_b ? shape_b[ndim_b - 1 - i] : 1;
        if (sa != sb && sa != 1 && sb != 1) { free(bs); return NULL; }
        bs[max_ndim - 1 - i] = sa > sb ? sa : sb;
    }
    *out_ndim = max_ndim;
    return bs;
}

static int64_t _strided_idx(int64_t i, int64_t ndim, int64_t* shape, int64_t* strides) {
    int64_t idx = 0;
    for (int64_t d = ndim - 1; d >= 0; d--) {
        int64_t pos = i % shape[d];
        idx += pos * strides[d];
        i /= shape[d];
    }
    return idx;
}

// =========================================================================
// Tensor creation / destruction
// =========================================================================
// Rank-1 and rank-2 constructors.
//
// flt_create takes an `int64_t*`, but a Flint array is a vec handle (a
// pointer to an opaque object), not a bare i64 buffer -- so a Flint caller
// cannot build one, and every attempt to pass a literal shape either fails the
// type check or reinterprets the handle as a pointer. These take the
// dimensions directly, which is what every call site actually wants.
void* flt_create_1d(int64_t n0) {
    int64_t shape[1] = { n0 };
    return flt_create(1, shape);
}

void* flt_create_2d(int64_t n0, int64_t n1) {
    int64_t shape[2] = { n0, n1 };
    return flt_create(2, shape);
}

void* flt_create_3d(int64_t n0, int64_t n1, int64_t n2) {
    int64_t shape[3] = { n0, n1, n2 };
    return flt_create(3, shape);
}

// 4-D (N, C, H, W) is the layout convolutions and pooling work in, so it needs
// a constructor like the others -- a Flint caller cannot pass the int64_t*
// that flt_create takes.
void* flt_create_4d(int64_t n0, int64_t n1, int64_t n2, int64_t n3) {
    int64_t shape[4] = { n0, n1, n2, n3 };
    return flt_create(4, shape);
}

// 5-D (N, C, D, H, W) for 3-D convolutions and volumes.
void* flt_create_5d(int64_t n0, int64_t n1, int64_t n2, int64_t n3, int64_t n4) {
    int64_t shape[5] = { n0, n1, n2, n3, n4 };
    return flt_create(5, shape);
}

// Fill from a caller-supplied array, so a tensor can be seeded without N
// separate flt_set calls.
void flt_set_all(void* t, const double* vals, int64_t n) {
    FlintTensor* tt = _as_tensor(t);
    if (!tt) return;
    int64_t k = n < tt->size ? n : tt->size;
    for (int64_t i = 0; i < k; i++) tt->data[i] = vals[i];
}

// Allocate with no element type yet; callers fill in one of data/data32.
static FlintTensor* _alloc(int64_t ndim, int64_t* shape, int is_f32) {
    FlintTensor* t = calloc(1, sizeof(FlintTensor));
    t->ndim = ndim;
    t->shape = _shape_copy(ndim, shape);
    t->size = _calc_size(ndim, shape);
    t->dtype = is_f32 ? FLT_F32 : FLT_F64;
    if (is_f32) t->data32 = calloc((size_t)t->size, sizeof(float));
    else        t->data = calloc((size_t)t->size, sizeof(double));
    t->device = DEVICE_CPU;
    t->grad = NULL;
    t->grad_id = -1;
    return t;
}

void* flt_create(int64_t ndim, int64_t* shape) {
    return _alloc(ndim, shape, 0);
}

void* flt_empty(int64_t ndim, int64_t* shape, int32_t is_f32) {
    return _alloc(ndim, shape, is_f32 ? 1 : 0);
}

// ---- f32 constructors, mirroring the f64 rank-specific helpers ----
void* flt_create_f32_1d(int64_t n0) {
    int64_t shape[1] = { n0 };
    return _alloc(1, shape, 1);
}
void* flt_create_f32_2d(int64_t n0, int64_t n1) {
    int64_t shape[2] = { n0, n1 };
    return _alloc(2, shape, 1);
}
void* flt_create_f32_3d(int64_t n0, int64_t n1, int64_t n2) {
    int64_t shape[3] = { n0, n1, n2 };
    return _alloc(3, shape, 1);
}
void* flt_create_f32_4d(int64_t n0, int64_t n1, int64_t n2, int64_t n3) {
    int64_t shape[4] = { n0, n1, n2, n3 };
    return _alloc(4, shape, 1);
}

int64_t flt_is_f32(void* t) {
    FlintTensor* t_ = _as_tensor(t);
    return (t_ && t_->dtype == FLT_F32) ? 1 : 0;
}

float* flt_data_f32(void* t) {
    FlintTensor* t_ = _as_tensor(t);
    return (t_ && t_->dtype == FLT_F32) ? t_->data32 : NULL;
}

int64_t flt_bytes(void* t) {
    FlintTensor* t_ = _as_tensor(t);
    if (!t_) return 0;
    return t_->size * (int64_t)(t_->dtype == FLT_F32 ? sizeof(float) : sizeof(double));
}

// ---- casts ----
// Widening and narrowing both go through double, which is exact for f32 -> f64
// and round-to-nearest for f64 -> f32.
void* flt_to_f32(void* t) {
    FlintTensor* s = _as_tensor(t);
    if (!s) return NULL;
    if (s->dtype == FLT_F32) return flt_clone(t);
    FlintTensor* r = _alloc(s->ndim, s->shape, 1);
    for (int64_t i = 0; i < s->size; i++) r->data32[i] = (float)s->data[i];
    return r;
}

void* flt_to_f64(void* t) {
    FlintTensor* s = _as_tensor(t);
    if (!s) return NULL;
    if (s->dtype == FLT_F64) return flt_clone(t);
    FlintTensor* r = _alloc(s->ndim, s->shape, 0);
    for (int64_t i = 0; i < s->size; i++) r->data[i] = (double)s->data32[i];
    return r;
}

void flt_destroy(void* t) {
    if (!t) return;
    FlintTensor* t_ = _as_tensor(t);
    if (t_->grad) free(t_->grad);
    // Exactly one of these is set, by dtype. free(NULL) is a no-op, so both
    // can be called unconditionally.
    free(t_->data);
    free(t_->data32);
    free(t_->shape);
    free(t_);
}

// Clone preserves the element type: copying an f32 tensor into f64 storage
// would silently quadruple its footprint and change what flt_is_f32 reports.
void* flt_clone(void* t) {
    FlintTensor* src = _as_tensor(t);
    FlintTensor* dst = _alloc(src->ndim, src->shape, src->dtype == FLT_F32);
    if (src->dtype == FLT_F32) memcpy(dst->data32, src->data32, (size_t)src->size * sizeof(float));
    else                    memcpy(dst->data, src->data, (size_t)src->size * sizeof(double));
    return dst;
}

void* flt_zeros_like(void* t) {
    FlintTensor* src = _as_tensor(t);
    return _alloc(src->ndim, src->shape, src->dtype == FLT_F32);
}

void* flt_zeros_like_f32(void* t) {
    FlintTensor* src = _as_tensor(t);
    return _alloc(src->ndim, src->shape, 1);
}

void* flt_clone_f32(void* t) {
    FlintTensor* src = _as_tensor(t);
    FlintTensor* dst = _alloc(src->ndim, src->shape, 1);
    if (src->dtype == FLT_F32) memcpy(dst->data32, src->data32, (size_t)src->size * sizeof(float));
    else                    for (int64_t i = 0; i < src->size; i++) dst->data32[i] = (float)src->data[i];
    return dst;
}

int64_t flt_size(void* t) { return _as_tensor(t)->size; }
int64_t flt_ndim(void* t) { return _as_tensor(t)->ndim; }
int64_t flt_shape(void* t, int64_t dim) { return _as_tensor(t)->shape[dim]; }

// Widening read: an f32 value returned as double is exact, so a caller that
// ignores flt_is_f32 still gets the right number for f32 tensors.
double flt_get(void* t, int64_t idx) {
    FlintTensor* t_ = _as_tensor(t);
    return (t_->dtype == FLT_F32) ? (double)t_->data32[idx] : t_->data[idx];
}

// The backing buffer. Layers need it to write into a tensor's storage
// directly -- an element-at-a-time flt_set loop is quadratic in overhead for
// anything the size of a convolution weight. Valid while the tensor lives;
// the layout is flat and row-major regardless of ndim. NULL for an f32
// tensor: use flt_data_f32 there, so a layer cannot write f32 through a
// double* and corrupt the buffer.
double* flt_data(void* t) {
    FlintTensor* t_ = _as_tensor(t);
    return (t_->dtype == FLT_F32) ? NULL : t_->data;
}
// Writes route by the DESTINATION's dtype: flt_set on an f32 tensor narrows,
// which is what the caller means by setting an f32 element.
void flt_set(void* t, int64_t idx, double v) {
    FlintTensor* t_ = _as_tensor(t);
    if (!t_) return;
    if (t_->dtype == FLT_F32) t_->data32[idx] = (float)v;
    else                    t_->data[idx] = v;
}
void flt_fill(void* t, double v) {
    FlintTensor* t_ = _as_tensor(t);
    if (!t_) return;
    if (t_->dtype == FLT_F32) { float fv = (float)v; for (int64_t i = 0; i < t_->size; i++) t_->data32[i] = fv; }
    else                    { for (int64_t i = 0; i < t_->size; i++) t_->data[i] = v; }
}
void flt_copy(void* dst, void* src) {
    FlintTensor* d = _as_tensor(dst);
    FlintTensor* s = _as_tensor(src);
    if (!d || !s) return;
    int64_t n = d->size < s->size ? d->size : s->size;
    // Convert element-wise when the widths differ; a raw memcpy between an
    // f32 and an f64 buffer would reinterpret every 4 bytes as 8.
    if (d->dtype == FLT_F32 && s->dtype == FLT_F32) {
        memcpy(d->data32, s->data32, (size_t)n * sizeof(float));
    } else if (d->dtype == FLT_F32) {
        for (int64_t i = 0; i < n; i++) d->data32[i] = (float)s->data[i];
    } else if (s->dtype == FLT_F32) {
        for (int64_t i = 0; i < n; i++) d->data[i] = (double)s->data32[i];
    } else {
        memcpy(d->data, s->data, (size_t)n * sizeof(double));
    }
}
void flt_randn(void* t) {
    FlintTensor* t_ = _as_tensor(t);
    if (!t_) return;
    for (int64_t i = 0; i < t_->size; i++) {
        double v = (double)rand() / RAND_MAX * 2.0 - 1.0;
        if (t_->dtype == FLT_F32) t_->data32[i] = (float)v; else t_->data[i] = v;
    }
}
void flt_rand(void* t) {
    FlintTensor* t_ = _as_tensor(t);
    for (int64_t i = 0; i < t_->size; i++)
        t_->data[i] = (double)rand() / RAND_MAX;
}
void flt_uniform(void* t, double lo, double hi) {
    FlintTensor* t_ = _as_tensor(t);
    for (int64_t i = 0; i < t_->size; i++)
        t_->data[i] = lo + (hi - lo) * (double)rand() / RAND_MAX;
}

// =========================================================================
// Element-wise unary ops (return new tensor)
// =========================================================================

static double _neg(double x) { return -x; }
static double _exp(double x) { return exp(x); }
static double _log(double x) { return log(x > 0 ? x : 1e-10); }
static double _sqrt(double x) { return sqrt(x > 0 ? x : 0); }
static double _relu(double x) { return x > 0 ? x : 0; }
static double _sigmoid(double x) { return 1.0 / (1.0 + exp(-x)); }
static double _tanh_(double x) { return tanh(x); }

// Elementwise ops, generated for both element types from one body.
//
// The f32 path computes in double and narrows on store: exp/tanh/sigmoid keep
// double precision where it matters (the final value) and there is still only
// one implementation of each math function to keep correct.
#define DEF_UNARY(NAME, FN)                                                   \
    static void* NAME(void* a) {                                              \
        FlintTensor* ta = _as_tensor(a);                                      \
        if (!ta) return NULL;                                                 \
        FlintTensor* r = _alloc(ta->ndim, ta->shape, ta->dtype == FLT_F32);     \
        if (ta->dtype == FLT_F32) {                                           \
            for (int64_t i = 0; i < ta->size; i++)                            \
                r->data32[i] = (float)FN((double)ta->data32[i]);               \
        } else {                                                              \
            for (int64_t i = 0; i < ta->size; i++)                            \
                r->data[i] = FN(ta->data[i]);                                 \
        }                                                                     \
        return r;                                                             \
    }

DEF_UNARY(flt_neg_impl, _neg)
DEF_UNARY(flt_exp_impl, _exp)
DEF_UNARY(flt_log_impl, _log)
DEF_UNARY(flt_sqrt_impl, _sqrt)
DEF_UNARY(flt_relu_impl, _relu)
DEF_UNARY(flt_sigmoid_impl, _sigmoid)
DEF_UNARY(flt_tanh_impl, _tanh_)

void* flt_neg(void* a)     { return flt_neg_impl(a); }
void* flt_exp(void* a)     { return flt_exp_impl(a); }
void* flt_log(void* a)     { return flt_log_impl(a); }
void* flt_sqrt(void* a)    { return flt_sqrt_impl(a); }
void* flt_relu(void* a)    { return flt_relu_impl(a); }
void* flt_sigmoid(void* a) { return flt_sigmoid_impl(a); }
void* flt_tanh(void* a)    { return flt_tanh_impl(a); }


void* flt_pow(void* a, double e) {
    FlintTensor* ta = _as_tensor(a);
    if (!ta) return NULL;
    FlintTensor* r = _alloc(ta->ndim, ta->shape, ta->dtype == FLT_F32);
    if (ta->dtype == FLT_F32) {
        for (int64_t i = 0; i < ta->size; i++)
            r->data32[i] = (float)pow((double)ta->data32[i], e);
    } else {
        for (int64_t i = 0; i < ta->size; i++) r->data[i] = pow(ta->data[i], e);
    }
    return r;
}

// =========================================================================
// In-place activations
// =========================================================================
// In-place: compute in double, narrow on store, matching the out-of-place ops.
void flt_relu_inplace(void* a) {
    FlintTensor* t = _as_tensor(a);
    if (!t) return;
    if (t->dtype == FLT_F32) {
        for (int64_t i = 0; i < t->size; i++)
            if (t->data32[i] < 0.0f) t->data32[i] = 0.0f;
    } else {
        for (int64_t i = 0; i < t->size; i++)
            if (t->data[i] < 0) t->data[i] = 0;
    }
}
void flt_sigmoid_inplace(void* a) {
    FlintTensor* t = _as_tensor(a);
    if (!t) return;
    if (t->dtype == FLT_F32) {
        for (int64_t i = 0; i < t->size; i++)
            t->data32[i] = (float)(1.0 / (1.0 + exp(-(double)t->data32[i])));
    } else {
        for (int64_t i = 0; i < t->size; i++)
            t->data[i] = 1.0 / (1.0 + exp(-t->data[i]));
    }
}
void flt_tanh_inplace(void* a) {
    FlintTensor* t = _as_tensor(a);
    if (!t) return;
    if (t->dtype == FLT_F32) {
        for (int64_t i = 0; i < t->size; i++) t->data32[i] = (float)tanh((double)t->data32[i]);
    } else {
        for (int64_t i = 0; i < t->size; i++) t->data[i] = tanh(t->data[i]);
    }
}

// =========================================================================
// Element-wise binary ops (broadcasting)
// =========================================================================
static void* _binary_op(void* a, void* b, double (*f)(double, double)) {
    FlintTensor* ta = _as_tensor(a);
    FlintTensor* tb = _as_tensor(b);
    int64_t out_ndim;
    int64_t* bs = _broadcast_shape(ta->ndim, ta->shape, tb->ndim, tb->shape, &out_ndim);
    if (!bs) { fprintf(stderr, "error: shapes not broadcastable\n"); return NULL; }
    // Type promotion: f32 only when BOTH sides are f32. Mixing widths widens,
    // which is what every numeric library does and what keeps a stray f32 from
    // silently truncating an f64 operand.
    int out_f32 = (ta->dtype == FLT_F32 && tb->dtype == FLT_F32);
    // _alloc COPIES the shape, so bs must still be live here -- freeing it
    // first gave every result tensor a freed/garbage shape (size 0).
    FlintTensor* r = _alloc(out_ndim, bs, out_f32);
    free(bs);
    if (!r) return NULL;
    for (int64_t i = 0; i < r->size; i++) {
        double av = (ta->dtype == FLT_F32) ? (double)ta->data32[i % ta->size] : ta->data[i % ta->size];
        double bv = (tb->dtype == FLT_F32) ? (double)tb->data32[i % tb->size] : tb->data[i % tb->size];
        double res = f(av, bv);
        if (out_f32) r->data32[i] = (float)res;
        else        r->data[i] = res;
    }
    return r;
}

static double _add(double a, double b) { return a + b; }
static double _sub(double a, double b) { return a - b; }
static double _mul(double a, double b) { return a * b; }
static double _div(double a, double b) { return b != 0 ? a / b : (a > 0 ? DBL_MAX : -DBL_MAX); }

void* flt_add_tt(void* a, void* b) { return _binary_op(a, b, _add); }
void* flt_sub_tt(void* a, void* b) { return _binary_op(a, b, _sub); }
void* flt_mul_tt(void* a, void* b) { return _binary_op(a, b, _mul); }
void* flt_div_tt(void* a, void* b) { return _binary_op(a, b, _div); }

// =========================================================================
// Matrix multiply
// =========================================================================
// Dispatches through flint_blas, which resolves OpenBLAS at first use via a
// weak symbol probe and otherwise uses the portable loop. This replaces an
// `#ifdef FLINT_USE_BLAS` that no build ever defined, which meant the BLAS
// branch was dead code and every matmul took the scalar triple loop.
void* flt_matmul(void* a, void* b) {
    FlintTensor* ta = _as_tensor(a);
    FlintTensor* tb = _as_tensor(b);
    if (!ta || !tb) { fprintf(stderr, "matmul: null tensor\n"); return NULL; }
    if (ta->ndim != 2 || tb->ndim != 2) { fprintf(stderr, "matmul: need 2D\n"); return NULL; }
    int64_t M = ta->shape[0], K = ta->shape[1], N = tb->shape[1];
    if (tb->shape[0] != K) { fprintf(stderr, "matmul: shape mismatch\n"); return NULL; }

    int64_t shape[2] = {M, N};
    // Mixed input promotes to f64 rather than truncating one operand.
    int out_f32 = (ta->dtype == FLT_F32 && tb->dtype == FLT_F32);
    FlintTensor* out = _alloc(2, shape, out_f32);
    if (!out) return NULL;
    if (out_f32) {
        // Convert into f32 buffers and run sgemm. Reusing the input buffers is
        // not safe (they are const and belong to the caller), so this
        // allocates two scratch copies -- matmul is dominated by the kernel
        // for any size worth calling it on.
        float* A = (float*)malloc((size_t)(M * K) * sizeof(float));
        float* B = (float*)malloc((size_t)(K * N) * sizeof(float));
        if (!A || !B) { free(A); free(B); return NULL; }
        for (int64_t i = 0; i < M * K; i++)
            A[i] = (ta->dtype == FLT_F32) ? ta->data32[i] : (float)ta->data[i];
        for (int64_t i = 0; i < K * N; i++)
            B[i] = (tb->dtype == FLT_F32) ? tb->data32[i] : (float)tb->data[i];
        flint_blas_matmul_f32(M, K, N, A, B, out->data32);
        free(A); free(B);
    } else {
        // Widen any f32 operand, then run dgemm.
        const double *Ad = ta->data, *Bd = tb->data;
        double* Aw = NULL; double* Bw = NULL;
        if (ta->dtype == FLT_F32) {
            Aw = (double*)malloc((size_t)(M * K) * sizeof(double));
            if (!Aw) return NULL;
            for (int64_t i = 0; i < M * K; i++) Aw[i] = (double)ta->data32[i];
            Ad = Aw;
        }
        if (tb->dtype == FLT_F32) {
            Bw = (double*)malloc((size_t)(K * N) * sizeof(double));
            if (!Bw) { free(Aw); return NULL; }
            for (int64_t i = 0; i < K * N; i++) Bw[i] = (double)tb->data32[i];
            Bd = Bw;
        }
        flint_blas_matmul(M, K, N, Ad, Bd, out->data);
        free(Aw); free(Bw);
    }
    return out;
}

// Explicit-BLAS matmul. Declared in the header since before the backend
// existed but never defined or registered -- calling it was a link error. It
// now forces the portable reference path, so it is the deterministic
// counterpart to flt_matmul (same answer, no BLAS variance) and doubles as the
// oracle the test gate compares the live path against.
void* flt_matmul_blas(void* a, void* b) {
    FlintTensor* ta = _as_tensor(a);
    FlintTensor* tb = _as_tensor(b);
    if (!ta || !tb) { fprintf(stderr, "matmul_blas: null tensor\n"); return NULL; }
    if (ta->ndim != 2 || tb->ndim != 2) { fprintf(stderr, "matmul_blas: need 2D\n"); return NULL; }
    int64_t M = ta->shape[0], K = ta->shape[1], N = tb->shape[1];
    if (tb->shape[0] != K) { fprintf(stderr, "matmul_blas: shape mismatch\n"); return NULL; }

    int64_t shape[2] = {M, N};
    int out_f32 = (ta->dtype == FLT_F32 && tb->dtype == FLT_F32);
    FlintTensor* out = _alloc(2, shape, out_f32);
    if (!out) return NULL;
    if (out_f32) {
        float* A = (float*)malloc((size_t)(M * K) * sizeof(float));
        float* B = (float*)malloc((size_t)(K * N) * sizeof(float));
        if (!A || !B) { free(A); free(B); return NULL; }
        for (int64_t i = 0; i < M * K; i++) A[i] = (float)(ta->dtype == FLT_F32 ? ta->data32[i] : ta->data[i]);
        for (int64_t i = 0; i < K * N; i++) B[i] = (float)(tb->dtype == FLT_F32 ? tb->data32[i] : tb->data[i]);
        flint_blas_matmul_f32_reference(M, K, N, A, B, out->data32);
        free(A); free(B);
    } else {
        const double *Ad = ta->data, *Bd = tb->data;
        double *Aw = NULL, *Bw = NULL;
        if (ta->dtype == FLT_F32) {
            Aw = (double*)malloc((size_t)(M * K) * sizeof(double));
            if (!Aw) return NULL;
            for (int64_t i = 0; i < M * K; i++) Aw[i] = (double)ta->data32[i];
            Ad = Aw;
        }
        if (tb->dtype == FLT_F32) {
            Bw = (double*)malloc((size_t)(K * N) * sizeof(double));
            if (!Bw) { free(Aw); return NULL; }
            for (int64_t i = 0; i < K * N; i++) Bw[i] = (double)tb->data32[i];
            Bd = Bw;
        }
        flint_blas_matmul_reference(M, K, N, Ad, Bd, out->data);
        free(Aw); free(Bw);
    }
    return out;
}

// =========================================================================
// Reductions
// =========================================================================
double flt_sum(void* a) {
    FlintTensor* ta = _as_tensor(a);
    if (!ta) return 0.0;
    // Accumulate in double for both element types: the sum is where f32
    // storage most often loses precision, and the accumulator is not the thing
    // being stored, so widening it is free.
    double s = 0.0;
    if (ta->dtype == FLT_F32) { for (int64_t i = 0; i < ta->size; i++) s += (double)ta->data32[i]; }
    else                    { for (int64_t i = 0; i < ta->size; i++) s += ta->data[i]; }
    return s;
}

double flt_mean(void* a) {
    FlintTensor* ta = _as_tensor(a);
    return ta->size > 0 ? flt_sum(a) / ta->size : 0;
}

// =========================================================================
// Reshape / Transpose
// =========================================================================
void* flt_reshape(void* a, int64_t ndim, int64_t* shape) {
    FlintTensor* ta = _as_tensor(a);
    int64_t sz = _calc_size(ndim, shape);
    if (sz != ta->size) { fprintf(stderr, "reshape: size mismatch\n"); return NULL; }
    FlintTensor* r = _alloc(ndim, shape, ta->dtype == FLT_F32);
    if (ta->dtype == FLT_F32) memcpy(r->data32, ta->data32, (size_t)ta->size * sizeof(float));
    else                    memcpy(r->data, ta->data, (size_t)ta->size * sizeof(double));
    return r;
}

void* flt_transpose(void* a, int64_t dim0, int64_t dim1) {
    FlintTensor* ta = _as_tensor(a);
    if (dim0 < 0 || dim0 >= ta->ndim || dim1 < 0 || dim1 >= ta->ndim)
        { fprintf(stderr, "transpose: invalid dims\n"); return NULL; }
    int64_t* new_shape = malloc(ta->ndim * sizeof(int64_t));
    memcpy(new_shape, ta->shape, ta->ndim * sizeof(int64_t));
    int64_t tmp = new_shape[dim0];
    new_shape[dim0] = new_shape[dim1];
    new_shape[dim1] = tmp;
    FlintTensor* r = _alloc(ta->ndim, new_shape, ta->dtype == FLT_F32);
    free(new_shape);
    // Copy with transposed indexing
    int64_t* src_strides = malloc(ta->ndim * sizeof(int64_t));
    int64_t* dst_strides = malloc(r->ndim * sizeof(int64_t));
    src_strides[ta->ndim-1] = 1;
    for (int64_t d = ta->ndim-2; d >= 0; d--)
        src_strides[d] = src_strides[d+1] * ta->shape[d+1];
    dst_strides[r->ndim-1] = 1;
    for (int64_t d = r->ndim-2; d >= 0; d--)
        dst_strides[d] = dst_strides[d+1] * r->shape[d+1];
    // Map flat indices through transposed shape
    int64_t* tmp_idx = calloc(ta->ndim, sizeof(int64_t));
    for (int64_t i = 0; i < ta->size; i++) {
        int64_t x = i;
        for (int64_t d = ta->ndim-1; d >= 0; d--) {
            tmp_idx[d] = x % ta->shape[d];
            x /= ta->shape[d];
        }
        int64_t tmp_v = tmp_idx[dim0];
        tmp_idx[dim0] = tmp_idx[dim1];
        tmp_idx[dim1] = tmp_v;
        int64_t dst_i = 0;
        for (int64_t d = 0; d < r->ndim; d++)
            dst_i += tmp_idx[d] * dst_strides[d];
        if (ta->dtype == FLT_F32) r->data32[dst_i] = ta->data32[i];
        else                    r->data[dst_i] = ta->data[i];
    }
    free(tmp_idx); free(src_strides); free(dst_strides);
    return r;
}

// =========================================================================
// Loss functions
// =========================================================================
double mse_loss(void* pred, void* target) {
    FlintTensor* p = _as_tensor(pred);
    FlintTensor* t = _as_tensor(target);
    int64_t n = p->size < t->size ? p->size : t->size;
    double s = 0;
    for (int64_t i = 0; i < n; i++) {
        double pv = (p->dtype == FLT_F32) ? (double)p->data32[i] : p->data[i];
        double tv = (t->dtype == FLT_F32) ? (double)t->data32[i] : t->data[i];
        double d = pv - tv; s += d * d;
    }
    return s / n;
}

void* mse_loss_grad(void* pred, void* target) {
    FlintTensor* p = _as_tensor(pred);
    FlintTensor* t = _as_tensor(target);
    int64_t n = p->size;
    double scale = 2.0 / n;
    // The gradient tensor matches the PREDICTION's dtype, so a chain ending in
    // an f32 forward pass produces f32 gradients all the way back.
    FlintTensor* g = _alloc(p->ndim, p->shape, p->dtype == FLT_F32);
    if (!g) return NULL;
    for (int64_t i = 0; i < n; i++) {
        double pv = (p->dtype == FLT_F32) ? (double)p->data32[i] : p->data[i];
        double tv = (t->dtype == FLT_F32) ? (double)t->data32[i] : t->data[i];
        double gv = (pv - tv) * scale;
        if (g->dtype == FLT_F32) g->data32[i] = (float)gv; else g->data[i] = gv;
    }
    return g;
}

// =========================================================================
// AUTOGRAH ENGINE
// =========================================================================
// Global autograd state
static AGNode* ag_graph = NULL;
static AGNode* ag_graph_last = NULL;
static int64_t ag_next_id = 0;
static int ag_initialized = 0;

// grad_id -> tensor registry.
//
// ag_backward accumulates into the tensor's own `grad` buffer, but the caller
// only holds the id that ag_track_tensor returned. Without a way to map the id
// back, ag_get_grad has nothing to resolve and gradients are unreachable --
// which is why it used to be a stub returning NULL. Ids are dense and start at
// 0, so a growable array indexed by id is enough; entries are cleared on
// cleanup so a stale id cannot hand back a freed tensor.
static FlintTensor** ag_reg = NULL;
static int64_t ag_reg_n = 0;

static void _ag_reg_set(int64_t id, FlintTensor* t) {
    if (id < 0) return;
    if (id >= ag_reg_n) {
        int64_t want = id + 1;
        int64_t cap = ag_reg_n ? ag_reg_n : 16;
        while (cap < want) cap *= 2;
        FlintTensor** nr = (FlintTensor**)realloc(ag_reg, (size_t)cap * sizeof(FlintTensor*));
        if (!nr) return;
        memset(nr + ag_reg_n, 0, (size_t)(cap - ag_reg_n) * sizeof(FlintTensor*));
        ag_reg = nr;
        ag_reg_n = cap;
    }
    ag_reg[id] = t;
}

static FlintTensor* _ag_reg_get(int64_t id) {
    if (id < 0 || id >= ag_reg_n) return NULL;
    return ag_reg[id];
}

// Track tensor: assign a grad_id and allocate gradient buffer
int64_t ag_track_tensor(void* tensor) {
    if (!ag_initialized) ag_init();
    FlintTensor* t = _as_tensor(tensor);
    t->grad_id = ag_next_id++;
    t->grad = calloc((size_t)t->size, sizeof(double));
    _ag_reg_set(t->grad_id, t);
    return t->grad_id;
}

void ag_init(void) {
    if (ag_initialized) return;
    ag_graph = NULL;
    ag_graph_last = NULL;
    ag_next_id = 0;
    ag_initialized = 1;
}

void ag_cleanup(void) {
    AGNode* n = ag_graph;
    while (n) { AGNode* next = n->next; free(n->input_ids); free(n->extra); free(n); n = next; }
    ag_graph = NULL;
    ag_graph_last = NULL;
    ag_next_id = 0;
}

// Snapshot the input tensors' data into the node.
//
// Backward is a function of the operands as well as grad_out: d(ab)/da needs b,
// and relu's derivative needs the sign of its input. The tape used to record
// ids only, so every rule degraded to a pass-through. The copies are owned by
// the node and freed in ag_cleanup.
static void _ag_capture(AGNode* n, void* const* tensors, int n_tensors) {
    int64_t total = 0, shape_total = 0;
    for (int i = 0; i < n_tensors; i++) {
        FlintTensor* t = (FlintTensor*)tensors[i];
        n->value_ndims[i] = t ? t->ndim : 0;
        total += t ? t->size : 0;
        shape_total += t ? t->ndim : 0;
    }
    n->values = total > 0 ? (double*)malloc((size_t)total * sizeof(double)) : NULL;
    n->value_shapes = shape_total > 0 ? (int64_t*)malloc((size_t)shape_total * sizeof(int64_t)) : NULL;
    int64_t off = 0, soff = 0;
    for (int i = 0; i < n_tensors; i++) {
        FlintTensor* t = (FlintTensor*)tensors[i];
        if (!t) continue;
        // The tape stays f64 whatever the tensor is: the snapshot is read by
        // the backward rules as doubles, and widening an f32 value is exact, so
        // this loses nothing while keeping one implementation of every rule.
        // A raw memcpy of an f32 buffer into this would reinterpret 4-byte
        // floats as 8-byte doubles and produce silent garbage.
        if (t->dtype == FLT_F32) {
            for (int64_t k = 0; k < t->size; k++) n->values[off + k] = (double)t->data32[k];
        } else {
            memcpy(n->values + off, t->data, (size_t)t->size * sizeof(double));
        }
        memcpy(n->value_shapes + soff, t->shape, (size_t)t->ndim * sizeof(int64_t));
        off += t->size;
        soff += t->ndim;
    }
}

void ag_record(int64_t op_type, int64_t output_id, int64_t* input_ids,
                int n_inputs, void* extra, int extra_int) {
    AGNode* n = calloc(1, sizeof(AGNode));
    n->op = (AGOpType)op_type;
    n->output_id = output_id;
    n->n_inputs = n_inputs;
    n->input_ids = malloc(n_inputs * sizeof(int64_t));
    memcpy(n->input_ids, input_ids, n_inputs * sizeof(int64_t));
    n->extra = extra;
    n->extra_int = extra_int;
    n->next = NULL;
    if (ag_graph_last) ag_graph_last->next = n;
    else ag_graph = n;
    ag_graph_last = n;
}

// Record an op together with a snapshot of its operands, which is what makes a
// real backward rule possible.
static void ag_record_values(int64_t op_type, int64_t output_id, int64_t* input_ids,
                             int n_inputs, void* extra, int extra_int,
                             void* const* tensors) {
    ag_record(op_type, output_id, input_ids, n_inputs, extra, extra_int);
    AGNode* n = ag_graph_last;
    n->value_ndims = (int64_t*)calloc((size_t)n_inputs, sizeof(int64_t));
    _ag_capture(n, tensors, n_inputs);
}

// Shape of operand i.
static const int64_t* _ag_shape_of(const AGNode* n, int i) {
    if (!n->value_shapes || !n->value_ndims) return NULL;
    int64_t soff = 0;
    for (int k = 0; k < i; k++) soff += n->value_ndims[k];
    return n->value_shapes + soff;
}

// Element count of operand i. Derived from its recorded shape so a 1-element
// shape gives 1 rather than the product of an empty array.
static int64_t _ag_value_size(const AGNode* n, int i) {
    int64_t ndim = n->value_ndims[i], sz = 1;
    const int64_t* shape = _ag_shape_of(n, i);
    for (int64_t d = 0; d < ndim; d++) sz *= shape[d];
    return sz;
}

// Resolve input i's snapshot into (values pointer, ndim, shape pointer).
static const double* _ag_operand(const AGNode* n, int i, int64_t* size_out,
                                 int64_t* ndim_out, const int64_t** shape_out) {
    *size_out = 0; *ndim_out = 0; *shape_out = NULL;
    if (!n->values || !n->value_ndims || !n->value_shapes) return NULL;
    int64_t voff = 0, soff = 0;
    for (int k = 0; k < i; k++) {
        voff += _ag_value_size(n, k);
        soff += n->value_ndims[k];
    }
    *size_out = _ag_value_size(n, i);
    *ndim_out = n->value_ndims[i];
    *shape_out = n->value_shapes + soff;
    return n->values + voff;
}

// Set the gradient that ag_backward differentiates from.
//
// mse_loss_grad returns a tensor of dL/dpred but does not put it on the tape,
// so ag_backward(from) would seed with ones and ignore it entirely. This
// records a node that passes that gradient through unchanged, so the real loss
// gradient reaches the parameters. Without it, training optimises a made-up
// objective.
// Record the gradient that the next ag_backward(output_id) must START from.
//
// The previous design appended a disconnected AG_EMPTY node holding the value.
// That node has no inputs, so nothing consumed it: the traversal still began at
// ones, every rule downstream saw a seed of 1, and training moved weights the
// WRONG WAY -- a scalar fit from 1.0 toward a target of 5 drove w down to 0.2.
// The loss gradient was computed and thrown away every step.
//
// Instead stash the tensor so ag_backward can seed the traversal with it. That
// is the one place the value can actually enter the graph.
static FlintTensor* g_seed_tensor = NULL;

void ag_seed(void* grad) {
    FlintTensor* g = _as_tensor(grad);
    if (!g || g->size <= 0) { g_seed_tensor = NULL; return; }
    g_seed_tensor = g;
}
static AGNode* _find_node(int64_t output_id) {
    AGNode* n = ag_graph;
    while (n) { if (n->output_id == output_id) return n; n = n->next; }
    return NULL;
}

static void _acc_grad(FlintTensor* t, double* grad, int64_t n) {
    if (!t->grad) {
        t->grad = calloc((size_t)t->size, sizeof(double));
        memcpy(t->grad, grad, (size_t)n * sizeof(double));
    } else {
        for (int64_t i = 0; i < n && i < t->size; i++) t->grad[i] += grad[i];
    }
}

void ag_backward(int64_t output_id) {
    AGNode* n = _find_node(output_id);
    if (!n) return;
    FlintTensor* out = NULL; // will be set during traversal

    // Traverse the DAG backwards breadth-first.
    //
    // The stack used to be two 65536-entry arrays declared here, i.e. 1 MB of
    // automatic storage. That overflows the default stack on a normal thread and
    // aborted inside ag_backward before doing any work -- the function was
    // uncallable on any real program. Heap-allocate it instead: a graph is
    // bounded by its node count, so 1024 entries handles any realistic tape and
    // growth is explicit if one ever needs more.
    #define AG_STACK_INIT 1024
    #define AG_STACK_MAX  (1 << 20)
    int stack_cap = AG_STACK_INIT;
    AGNode** stack = (AGNode**)malloc((size_t)stack_cap * sizeof(AGNode*));
    double** grad_stack = (double**)malloc((size_t)stack_cap * sizeof(double*));
    if (!stack || !grad_stack) { free(stack); free(grad_stack); return; }
    int stack_size = 0;

    stack[stack_size] = n;
    // Seed the output gradient. The old code seeded a single 1.0 regardless of
    // the output's rank, so every op read grad_out[0] and left the rest of the
    // buffer untouched -- ag_backward on a length-3 tensor produced zeros past
    // element 0. Seed one 1.0 per element, which is the correct seed for a
    // non-scalar output (d/dx sum(x) = 1) and reduces to the old behaviour for
    // a scalar.
    // Width of every gradient buffer in this traversal: the OUTPUT size of the
    // node we differentiate from. extra_int records the op's INPUT extent, so
    // for a reduction it is the wrong number -- sum/mean emit a 1-element
    // output. Using it made the seed look mismatched (silently falling back to
    // ones) and sized every buffer wrong.
    int64_t seed_n = 1;
    {
        FlintTensor* ot = _ag_reg_get(n->output_id);
        if (ot && ot->size > 0) seed_n = ot->size;
    }
    double* seed = (double*)malloc((size_t)seed_n * sizeof(double));
    if (g_seed_tensor && g_seed_tensor->size == seed_n) {
        // ag_seed supplied dL/doutput. This is the value that must enter the
        // graph; seeding ones instead silently trained a fabricated objective.
        memcpy(seed, g_seed_tensor->data, (size_t)seed_n * sizeof(double));
        g_seed_tensor = NULL;   // consumed: one seed per backward
    } else {
        g_seed_tensor = NULL;
        for (int64_t i = 0; i < seed_n; i++) seed[i] = 1.0;
    }
    grad_stack[stack_size] = seed;
    stack_size++;
    // Every gradient buffer in the traversal is n_seed wide; a node's total is
    // accumulated in a buffer of that size.
    const int64_t n_seed = seed_n;

    // Accumulated incoming gradient per node, keyed by the node itself.
    //
    // A node can be reached by more than one path -- ag_mul(d, d) contributes
    // two chains to the SAME node -- and each visit used to apply its own chain
    // independently, so the leaf accumulated the combined effect several times
    // (gradients came out 4x instead of 2x). Merging into one buffer per node,
    // and processing the node once its total is known, is both correct and
    // cheaper. Plain arrays keep this C99; the graph is small and short-lived.
    AGNode**   acc_nodes = (AGNode**)malloc(sizeof(AGNode*) * 256);
    double**   acc_grads = (double**)malloc(sizeof(double*) * 256);
    int        acc_cap = 256;
    int        acc_n = 0;

    while (stack_size > 0) {
        stack_size--;
        AGNode* node = stack[stack_size];
        double* grad_out = grad_stack[stack_size];

        if (!node) { free(grad_out); continue; }

        // Merge this arrival into the node's running total. A node already
        // expanded earlier in this pass must NOT be expanded again, so if it
        // has been seen the accumulated buffer is simply updated and the node
        // is not re-queued.
        {
            int slot = -1;
            for (int q = 0; q < acc_n; q++) if (acc_nodes[q] == node) { slot = q; break; }
            if (slot >= 0) {
                int64_t w = n_seed;
                {
                    FlintTensor* ot = _ag_reg_get(node->output_id);
                    if (ot && ot->size > 0) w = ot->size;
                }
                for (int64_t i = 0; i < w; i++) acc_grads[slot][i] += grad_out[i];
                free(grad_out);
                continue;   // already accounted for
            }
            if (acc_n == acc_cap) {
                int ncap = acc_cap * 2;
                acc_nodes = (AGNode**)realloc(acc_nodes, sizeof(AGNode*) * ncap);
                acc_grads = (double**)realloc(acc_grads, sizeof(double*) * ncap);
                acc_cap = ncap;
            }
            int64_t w = n_seed;
            {
                FlintTensor* ot = _ag_reg_get(node->output_id);
                if (ot && ot->size > 0) w = ot->size;
            }
            double* keep = (double*)malloc((size_t)w * sizeof(double));
            for (int64_t i = 0; i < w; i++) keep[i] = grad_out[i];
            acc_nodes[acc_n] = node;
            acc_grads[acc_n] = keep;
            acc_n++;
            free(grad_out);
            grad_out = keep;
        }

        // Find output tensor by grad_id (crude: iterate all tensors)
        // For now, compute gradients using op-specific math

        double* grad_inputs = NULL;
        int64_t n_grad = 0;
        // True when grad_inputs is a freshly allocated buffer this loop owns,
        // false when it aliases grad_out (which is freed separately below).
        // Without this the two frees at the end of the loop double-free whenever
        // an op passes its input gradient straight through -- which ag_mul does,
        // so ag_backward aborted on any program using it. ASan caught it.
        int grad_inputs_owned = 0;

        switch (node->op) {
            // Real rules. Each needs the operand values the node captured:
            // d(ab)/da = grad * b, relu's derivative needs the sign of its
            // input, and so on. Every one of these used to be a pass-through,
            // which is why ag_mul returned the seed instead of 2x.
            case AG_NEG: {
                n_grad = node->extra_int > 0 ? node->extra_int : 1;
                double* gi = (double*)malloc((size_t)n_grad * sizeof(double));
                grad_inputs_owned = 1;
                for (int64_t i = 0; i < n_grad; i++) gi[i] = -grad_out[i];
                grad_inputs = gi;
                break;
            }
            case AG_RELU: {
                n_grad = node->extra_int > 0 ? node->extra_int : 1;
                int64_t asz = 0, and = 0; const int64_t* ash = NULL;
                const double* av = _ag_operand(node, 0, &asz, &and, &ash);
                double* gi = (double*)malloc((size_t)n_grad * sizeof(double));
                grad_inputs_owned = 1;
                for (int64_t i = 0; i < n_grad; i++)
                    gi[i] = (av && i < asz && av[i] > 0.0) ? grad_out[i] : 0.0;
                grad_inputs = gi;
                break;
            }
            case AG_SIGMOID: {
                n_grad = node->extra_int > 0 ? node->extra_int : 1;
                int64_t osz = 0, ond = 0; const int64_t* osh = NULL;
                const double* ov = _ag_operand(node, 1 >= node->n_inputs ? 0 : 1, &osz, &ond, &osh);
                // s'(x) = s * (1 - s); ov is the OUTPUT (input i=0), so use the
                // output tensor's own value via the recorded operand when it is
                // the same size, else fall back to the seed.
                double* gi = (double*)malloc((size_t)n_grad * sizeof(double));
                grad_inputs_owned = 1;
                for (int64_t i = 0; i < n_grad; i++) {
                    double s = (ov && i < osz) ? 1.0 / (1.0 + exp(-ov[i])) : 1.0;
                    gi[i] = grad_out[i] * s * (1.0 - s);
                }
                grad_inputs = gi;
                break;
            }
            case AG_TANH: {
                n_grad = node->extra_int > 0 ? node->extra_int : 1;
                int64_t asz = 0, and = 0; const int64_t* ash = NULL;
                const double* av = _ag_operand(node, 0, &asz, &and, &ash);
                double* gi = (double*)malloc((size_t)n_grad * sizeof(double));
                grad_inputs_owned = 1;
                for (int64_t i = 0; i < n_grad; i++) {
                    double t = (av && i < asz) ? tanh(av[i]) : 0.0;
                    gi[i] = grad_out[i] * (1.0 - t * t);
                }
                grad_inputs = gi;
                break;
            }
            case AG_EXP: {
                n_grad = node->extra_int > 0 ? node->extra_int : 1;
                int64_t asz = 0, and = 0; const int64_t* ash = NULL;
                const double* av = _ag_operand(node, 0, &asz, &and, &ash);
                double* gi = (double*)malloc((size_t)n_grad * sizeof(double));
                grad_inputs_owned = 1;
                for (int64_t i = 0; i < n_grad; i++)
                    gi[i] = (av && i < asz) ? grad_out[i] * exp(av[i]) : grad_out[i];
                grad_inputs = gi;
                break;
            }
            case AG_LOG: {
                n_grad = node->extra_int > 0 ? node->extra_int : 1;
                int64_t asz = 0, and = 0; const int64_t* ash = NULL;
                const double* av = _ag_operand(node, 0, &asz, &and, &ash);
                double* gi = (double*)malloc((size_t)n_grad * sizeof(double));
                grad_inputs_owned = 1;
                for (int64_t i = 0; i < n_grad; i++) {
                    double x = (av && i < asz) ? av[i] : 1.0;
                    gi[i] = (x != 0.0) ? grad_out[i] / x : 0.0;
                }
                grad_inputs = gi;
                break;
            }
            case AG_SUB: {
                // d(a-b)/da = 1, d(a-b)/db = -1
                n_grad = node->extra_int > 0 ? node->extra_int : 1;
                double* gi = (double*)malloc((size_t)n_grad * sizeof(double));
                grad_inputs_owned = 1;
                memcpy(gi, grad_out, (size_t)n_grad * sizeof(double));
                grad_inputs = gi;
                break;
            }
            case AG_SUM: {
                // d(sum x)/dx_i = 1: every element gets the incoming gradient
                // unchanged, broadcast from the scalar output. n (the input
                // count) is what extra_int records; the incoming gradient is
                // one element wide.
                int64_t n = node->extra_int > 0 ? node->extra_int : 1;
                grad_inputs = (double*)malloc((size_t)n * sizeof(double));
                grad_inputs_owned = 1;
                for (int64_t i = 0; i < n; i++) grad_inputs[i] = grad_out[0];
                n_grad = n;
                // The chain handed downstream is n wide, which may exceed the
                // traversal width; the accumulator is sized to n_seed.
                break;
            }
            case AG_MEAN: {
                // d(mean x)/dx_i = 1/n, with n carried in extra.
                int64_t n = node->extra_int > 0 ? node->extra_int : 1;
                double scale = 1.0;
                if (node->extra) scale = ((double*)node->extra)[0];
                grad_inputs = (double*)malloc((size_t)n * sizeof(double));
                grad_inputs_owned = 1;
                for (int64_t i = 0; i < n; i++) grad_inputs[i] = grad_out[0] * scale;
                n_grad = n;
                break;
            }
            case AG_ADD: {
                // d(a+b)/da = d(a+b)/db = 1
                n_grad = node->extra_int > 0 ? node->extra_int : 1;
                double* gi = (double*)malloc((size_t)n_grad * sizeof(double));
                grad_inputs_owned = 1;
                memcpy(gi, grad_out, (size_t)n_grad * sizeof(double));
                grad_inputs = gi;
                break;
            }
            case AG_MUL: {
                // d(a*b)/da = grad * b and d(a*b)/db = grad * a. The node
                // records both operands, so both chains are available; the
                // per-input split happens in the propagation loop below.
                n_grad = node->extra_int > 0 ? node->extra_int : 1;
                int64_t bsz = 0, bnd = 0; const int64_t* bsh = NULL;
                const double* bv = _ag_operand(node, 1, &bsz, &bnd, &bsh);
                double* gi = (double*)malloc((size_t)n_grad * sizeof(double));
                grad_inputs_owned = 1;
                for (int64_t i = 0; i < n_grad; i++)
                    gi[i] = (bv && i < bsz) ? grad_out[i] * bv[i] : grad_out[i];
                grad_inputs = gi;
                break;
            }
            case AG_MATMUL: {
                // C = A(MxK) @ B(KxN). dA = grad(MxN) @ B^T (MxN * NxK = MxK),
                // dB = A^T (KxM) @ grad (KxM * MxN = KxN). Row-major.
                int64_t* shp = (int64_t*)node->extra;
                int64_t M = shp ? shp[0] : 0, K = shp ? shp[1] : 0, N = shp ? shp[2] : 0;
                int64_t asz = 0, and_ = 0; const int64_t* ash = NULL;
                int64_t bsz = 0, bnd = 0; const int64_t* bsh = NULL;
                const double* av = _ag_operand(node, 0, &asz, &and_, &ash);
                const double* bv = _ag_operand(node, 1, &bsz, &bnd, &bsh);
                if (M > 0 && K > 0 && N > 0 && av && bv) {
                    int64_t da_sz = 0, da_nd = 0; const int64_t* da_sh = NULL;
                    const double* da = _ag_operand(node, 0, &da_sz, &da_nd, &da_sh);
                    int64_t out_sz = M * N;
                    // dA into grad_inputs, dB recomputed in the split below.
                    grad_inputs = (double*)calloc((size_t)(M * K), sizeof(double));
                    grad_inputs_owned = 1;
                    for (int64_t i = 0; i < M; i++)
                        for (int64_t k = 0; k < K; k++) {
                            double s = 0.0;
                            for (int64_t j = 0; j < N; j++) s += grad_out[i * N + j] * bv[k * N + j];
                            grad_inputs[i * K + k] = s;
                        }
                    n_grad = M * K;
                    (void)da; (void)out_sz; (void)and_;
                } else {
                    n_grad = node->extra_int > 0 ? node->extra_int : 1;
                    grad_inputs = (double*)malloc((size_t)n_grad * sizeof(double));
                    grad_inputs_owned = 1;
                    memcpy(grad_inputs, grad_out, (size_t)n_grad * sizeof(double));
                }
                break;
            }
            case AG_EMPTY: {
                // Seed node: the gradient IS the value captured by ag_seed
                // (dL/dpred), not the incoming grad_out, which the traversal
                // sets to ones. Reading grad_out here would discard the loss
                // gradient and train against a made-up objective.
                n_grad = node->extra_int > 0 ? node->extra_int : 1;
                double* gi = (double*)malloc((size_t)n_grad * sizeof(double));
                grad_inputs_owned = 1;
                if (node->values) memcpy(gi, node->values, (size_t)n_grad * sizeof(double));
                else memcpy(gi, grad_out, (size_t)n_grad * sizeof(double));
                grad_inputs = gi;
                break;
            }
            default: {
                n_grad = node->extra_int > 0 ? node->extra_int : 1;
                double* gi = (double*)malloc((size_t)n_grad * sizeof(double));
                grad_inputs_owned = 1;
                memcpy(gi, grad_out, (size_t)n_grad * sizeof(double));
                grad_inputs = gi;
                break;
            }
        }

        // Propagate to inputs. An input with no creator node is a leaf -- a
        // tensor the caller tracked -- so its gradient accumulates into its own
        // buffer. Without this the whole pass computed gradients and discarded
        // them: _acc_grad existed but had no callers, so ag_backward left every
        // tracked tensor's grad at zero.
        //
        // A repeated input (ag_mul(x, x) records x's id twice) must accumulate
        // once. It did, but the loop then freed grad_inputs once per input, so
        // the second pass double-freed -- ASan caught it, and without ASan it
        // aborted the process inside ag_backward. Accumulate per unique input
        // and free the buffer once after the loop.
        for (int k = 0; k < node->n_inputs; k++) {
            AGNode* input_node = _find_node(node->input_ids[k]);
            // Per-input gradient. `grad_inputs` is input 0's chain; a binary op
            // needs a different chain per input (d(ab)/da uses b, d(ab)/db uses
            // a), so input 1 gets its own buffer here rather than a copy of
            // input 0's.
            double* gk = grad_inputs;
            int64_t nk = n_grad;
            int gk_owned = 0;
            if (k == 1 && node->n_inputs == 2) {
                if (node->op == AG_SUB) {
                    // d(a-b)/db = -1, so the second chain negates.
                    gk = (double*)malloc((size_t)n_grad * sizeof(double));
                    if (!gk) { gk = grad_inputs; }
                    else {
                        gk_owned = 1;
                        for (int64_t i = 0; i < n_grad; i++) gk[i] = -grad_inputs[i];
                    }
                }
                if (node->op == AG_MUL) {
                    int64_t asz = 0, and_ = 0; const int64_t* ash = NULL;
                    const double* av = _ag_operand(node, 0, &asz, &and_, &ash);
                    gk = (double*)malloc((size_t)n_grad * sizeof(double));
                    if (!gk) { gk = grad_inputs; }
                    else {
                        gk_owned = 1;
                        for (int64_t i = 0; i < n_grad; i++)
                            gk[i] = (av && i < asz) ? grad_out[i] * av[i] : grad_out[i];
                    }
                } else if (node->op == AG_MATMUL) {
                    int64_t* shp = (int64_t*)node->extra;
                    int64_t M = shp ? shp[0] : 0, K = shp ? shp[1] : 0, N = shp ? shp[2] : 0;
                    int64_t asz = 0, and_ = 0; const int64_t* ash = NULL;
                    const double* av = _ag_operand(node, 0, &asz, &and_, &ash);
                    if (M > 0 && K > 0 && N > 0 && av) {
                        nk = K * N;
                        gk = (double*)calloc((size_t)nk, sizeof(double));
                        if (gk) {
                            gk_owned = 1;
                            for (int64_t kk = 0; kk < K; kk++)
                                for (int64_t j = 0; j < N; j++) {
                                    double s = 0.0;
                                    for (int64_t i2 = 0; i2 < M; i2++) s += av[i2 * K + kk] * grad_out[i2 * N + j];
                                    gk[kk * N + j] = s;
                                }
                        } else { gk = grad_inputs; nk = n_grad; }
                    } else { gk = grad_inputs; nk = n_grad; }
                }
            }
            if (!input_node) {
                FlintTensor* leaf = _ag_reg_get(node->input_ids[k]);
                if (leaf) {
                    // Reduce to the operand's shape before accumulating.
                    // A broadcast operand (a bias row added across every row of
                    // an (H x N) tensor) receives the same gradient once per
                    // position it was broadcast to, so summing is required --
                    // without it every bias gradient came out divided by N,
                    // which is enough to make training diverge.
                    // Compare against the LEAF's own shape rather than the
                    // recorded operand snapshot: when an operand was broadcast,
                    // the snapshot already has the expanded size, so it cannot
                    // distinguish "same shape" from "was broadcast".
                    int64_t lsz = leaf->size;
                    int64_t lnd = leaf->ndim;
                    const int64_t* lsh = leaf->shape;
                    if (lsz > 0 && lsz < nk) {
                        // The operand's elements are repeated `reps` times each
                        // in row-major order, so reps = nk / lsz. Reduce by
                        // summing each run of reps -- which is exactly the
                        // sum-over-broadcast-positions a gradient needs.
                        int64_t reps = nk / lsz;
                        if (reps > 1) {
                            double* red = (double*)calloc((size_t)lsz, sizeof(double));
                            if (red) {
                                for (int64_t r = 0; r < lsz; r++)
                                    for (int64_t q = 0; q < reps; q++)
                                        red[r] += gk[r * reps + q];
                                _acc_grad(leaf, red, lsz);
                                free(red);
                                if (gk_owned) free(gk);
                                continue;
                            }
                        }
                    }
                    _acc_grad(leaf, gk, nk);
                }
                if (gk_owned) free(gk);
                continue;
            }
            // A repeated input (ag_mul(d, d)) must contribute BOTH chains, but
            // it must be traversed ONCE. Pushing the node twice made it pop
            // twice, so _acc_grad ran twice on the same leaf and the gradient
            // came out at 4x instead of 2x (caught by the finite-difference
            // check). When this input was already scheduled, merge the chain
            // into that pending buffer instead of adding a second work item.
            int dup = -1;
            for (int j = 0; j < stack_size; j++) {
                if (stack[j] == input_node) { dup = j; break; }
            }
            if (dup >= 0) {
                // The pending entry has the length its own chain needs; merge
                // element-wise over the shorter length, which is what the
                // same-input case always is (both chains have equal length).
                double* pend = grad_stack[dup];
                for (int64_t i = 0; i < n_grad && i < nk; i++) pend[i] += gk[i];
                if (gk_owned) free(gk);
                continue;
            }
            if (stack_size == stack_cap && stack_cap < AG_STACK_MAX) {
                int ncap = stack_cap * 2;
                AGNode** ns = (AGNode**)realloc(stack, (size_t)ncap * sizeof(AGNode*));
                double** ng = (double**)realloc(grad_stack, (size_t)ncap * sizeof(double*));
                if (ns) stack = ns;
                if (ng) grad_stack = ng;
                if (ns && ng) stack_cap = ncap;
            }
            if (stack_size < stack_cap) {
                stack[stack_size] = input_node;
                double* g = malloc((size_t)nk * sizeof(double));
                if (!g) { if (gk_owned) free(gk); break; }
                memcpy(g, gk, (size_t)nk * sizeof(double));
                grad_stack[stack_size] = g;
                stack_size++;
            }
            if (gk_owned) free(gk);
        }

        free(grad_out);
        if (grad_inputs && grad_inputs_owned) free(grad_inputs);
    }

    free(stack);
    free(grad_stack);
    #undef AG_STACK_INIT
    #undef AG_STACK_MAX
}

double* ag_get_grad(int64_t grad_id) {
    FlintTensor* t = _ag_reg_get(grad_id);
    return t ? t->grad : NULL;
}

// The grad_id an ag_* op assigned to its output.
//
// ag_matmul and friends set grad_id on the tensor they return, but the caller
// has no way to read it back: re-tracking with ag_track_tensor allocates a
// SECOND id and orphans the recorded node, so ag_backward finds nothing and the
// gradient stays zero. Callers need this to name the node they want to
// differentiate from.
// Read one gradient element.
//
// The `double*` ag_get_grad returns cannot be indexed from Flint: `ptr[i]` is
// defined as string indexing (an i8 load guarded by flint_str_length), so
// `g[0]` reads the gradient buffer as if it were a NUL-terminated string and
// traps on the length check. Exposing the element read as a call keeps the
// language unchanged and makes gradients actually reachable.
double ag_grad_at(int64_t grad_id, int64_t idx) {
    FlintTensor* t = _ag_reg_get(grad_id);
    if (!t || !t->grad) return 0.0;
    if (idx < 0 || idx >= t->size) {
        fprintf(stderr, "ag_grad_at: index %lld out of bounds for length %lld\n",
                (long long)idx, (long long)t->size);
        return 0.0;
    }
    return t->grad[idx];
}

// Elements in a tracked tensor's gradient buffer.
int64_t ag_grad_len(int64_t grad_id) {
    FlintTensor* t = _ag_reg_get(grad_id);
    return (t && t->grad) ? t->size : 0;
}

int64_t ag_grad_id(void* tensor) {
    if (!tensor) return -1;
    FlintTensor* t = _as_tensor(tensor);
    return t->grad_id;
}

void ag_zero_grad(int64_t grad_id) {
    FlintTensor* t = _ag_reg_get(grad_id);
    if (t && t->grad)
        memset(t->grad, 0, (size_t)t->size * sizeof(double));
}

// =========================================================================
// High-level autograd ops (for Flint wrapper)
// =========================================================================
void* ag_matmul(void* a, void* b) {
    FlintTensor* ta = _as_tensor(a);
    FlintTensor* tb = _as_tensor(b);
    void* out = flt_matmul(a, b);
    if (ta->grad_id >= 0 || tb->grad_id >= 0) {
        int64_t ids[2] = {ta->grad_id, tb->grad_id};
        FlintTensor* tout = _as_tensor(out);
        tout->grad_id = ag_next_id++;
        _ag_reg_set(tout->grad_id, tout);
        // Store shapes for backward
        int64_t* shapes = malloc(4 * sizeof(int64_t));
        shapes[0] = ta->shape[0]; shapes[1] = ta->shape[1];
        shapes[2] = tb->shape[1]; shapes[3] = 0;
        void* t0[2] = { ta, tb };
        ag_record_values(AG_MATMUL, tout->grad_id, ids, 2, shapes, (int)tout->size, t0);
    }
    return out;
}

void* ag_add(void* a, void* b) {
    FlintTensor* ta = _as_tensor(a);
    FlintTensor* tb = _as_tensor(b);
    void* out = flt_add_tt(a, b);
    if (ta->grad_id >= 0 || tb->grad_id >= 0) {
        int64_t ids[2] = {ta->grad_id, tb->grad_id};
        FlintTensor* tout = _as_tensor(out);
        tout->grad_id = ag_next_id++;
        _ag_reg_set(tout->grad_id, tout);
        void* t1[2] = { ta, tb };
        ag_record_values(AG_ADD, tout->grad_id, ids, 2, NULL, (int)tout->size, t1);
    }
    return out;
}

// d(a - b)/da = 1, d(a - b)/db = -1
void* ag_sub(void* a, void* b) {
    FlintTensor* ta = _as_tensor(a);
    FlintTensor* tb = _as_tensor(b);
    void* out = flt_sub_tt(a, b);
    if (ta->grad_id >= 0 || tb->grad_id >= 0) {
        int64_t ids[2] = {ta->grad_id, tb->grad_id};
        FlintTensor* tout = _as_tensor(out);
        tout->grad_id = ag_next_id++;
        _ag_reg_set(tout->grad_id, tout);
        void* t[2] = { ta, tb };
        ag_record_values(AG_SUB, tout->grad_id, ids, 2, NULL, (int)tout->size, t);
    }
    return out;
}

// Sum over every element, producing a 1-element tensor so the result can stay
// on the tape. d(sum)/dx_i = 1 for all i, so backward is a broadcast of the
// incoming gradient -- the reduction the tape could not previously express.
void* ag_sum(void* a) {
    FlintTensor* ta = _as_tensor(a);
    int64_t sz = ta->size;
    void* out = flt_create_1d(1);
    FlintTensor* to = _as_tensor(out);
    // Read through flt_get: ta->data is NULL for an f32 tensor, and
    // accumulating through the widening accessor is exact.
    double total = 0.0;
    for (int64_t i = 0; i < sz; i++) total += flt_get(ta, i);
    to->data[0] = total;
    if (ta->grad_id >= 0) {
        to->grad_id = ag_next_id++;
        _ag_reg_set(to->grad_id, to);
        int64_t ids[1] = { ta->grad_id };
        void* t[1] = { ta };
        ag_record_values(AG_SUM, to->grad_id, ids, 1, NULL, (int)sz, t);
    }
    return out;
}

// Mean over every element. Same chain as sum, scaled by 1/n -- the scale is
// carried in extra so backward applies it without needing a divide node.
void* ag_mean(void* a) {
    FlintTensor* ta = _as_tensor(a);
    int64_t sz = ta->size > 0 ? ta->size : 1;
    void* out = flt_create_1d(1);
    FlintTensor* to = _as_tensor(out);
    double total = 0.0;
    for (int64_t i = 0; i < ta->size; i++) total += flt_get(ta, i);
    to->data[0] = total / (double)sz;
    if (ta->grad_id >= 0) {
        to->grad_id = ag_next_id++;
        _ag_reg_set(to->grad_id, to);
        int64_t ids[1] = { ta->grad_id };
        double* extra = (double*)malloc(sizeof(double));
        extra[0] = 1.0 / (double)sz;
        void* t[1] = { ta };
        ag_record_values(AG_MEAN, to->grad_id, ids, 1, extra, (int)sz, t);
    }
    return out;
}

// Seed the traversal with a uniform scalar. The common case is seeding a scalar
// loss with 1.0; ag_seed takes a whole tensor, which is awkward there.
void ag_seed_scalar(int64_t output_id, double v) {
    void* t = flt_create_1d(1);
    flt_set(t, 0, v);
    _ag_reg_set(output_id, _as_tensor(t));
    g_seed_tensor = _as_tensor(t);
    // The seed tensor must survive until backward consumes it.
    _ag_reg_set(output_id, _as_tensor(t));
}

void* ag_mul(void* a, void* b) {
    FlintTensor* ta = _as_tensor(a);
    FlintTensor* tb = _as_tensor(b);
    void* out = flt_mul_tt(a, b);
    if (ta->grad_id >= 0 || tb->grad_id >= 0) {
        int64_t ids[2] = {ta->grad_id, tb->grad_id};
        FlintTensor* tout = _as_tensor(out);
        tout->grad_id = ag_next_id++;
        _ag_reg_set(tout->grad_id, tout);
        void* t2[2] = { ta, tb };
        ag_record_values(AG_MUL, tout->grad_id, ids, 2, NULL, (int)tout->size, t2);
    }
    return out;
}

void* ag_relu(void* a) {
    FlintTensor* ta = _as_tensor(a);
    void* out = flt_relu(a);
    if (ta->grad_id >= 0) {
        int64_t ids[1] = {ta->grad_id};
        FlintTensor* tout = _as_tensor(out);
        tout->grad_id = ag_next_id++;
        _ag_reg_set(tout->grad_id, tout);
        void* t_ag_relu[1] = { ta };
        ag_record_values(AG_RELU, tout->grad_id, ids, 1, NULL, (int)tout->size, t_ag_relu);
    }
    return out;
}

void* ag_tanh(void* a) {
    FlintTensor* ta = _as_tensor(a);
    void* out = flt_tanh(a);
    if (ta->grad_id >= 0) {
        int64_t ids[1] = {ta->grad_id};
        FlintTensor* tout = _as_tensor(out);
        tout->grad_id = ag_next_id++;
        _ag_reg_set(tout->grad_id, tout);
        void* t_tanh[1] = { ta };
        ag_record_values(AG_TANH, tout->grad_id, ids, 1, NULL, (int)tout->size, t_tanh);
    }
    return out;
}

void* ag_log(void* a) {
    FlintTensor* ta = _as_tensor(a);
    void* out = flt_log(a);
    if (ta->grad_id >= 0) {
        int64_t ids[1] = {ta->grad_id};
        FlintTensor* tout = _as_tensor(out);
        tout->grad_id = ag_next_id++;
        _ag_reg_set(tout->grad_id, tout);
        void* t_log[1] = { ta };
        ag_record_values(AG_LOG, tout->grad_id, ids, 1, NULL, (int)tout->size, t_log);
    }
    return out;
}

void* ag_sigmoid(void* a) {
    FlintTensor* ta = _as_tensor(a);
    void* out = flt_sigmoid(a);
    if (ta->grad_id >= 0) {
        int64_t ids[1] = {ta->grad_id};
        FlintTensor* tout = _as_tensor(out);
        tout->grad_id = ag_next_id++;
        _ag_reg_set(tout->grad_id, tout);
        void* t_ag_sigmoid[1] = { ta };
        ag_record_values(AG_SIGMOID, tout->grad_id, ids, 1, NULL, (int)tout->size, t_ag_sigmoid);
    }
    return out;
}

void* ag_exp(void* a) {
    FlintTensor* ta = _as_tensor(a);
    void* out = flt_exp(a);
    if (ta->grad_id >= 0) {
        int64_t ids[1] = {ta->grad_id};
        FlintTensor* tout = _as_tensor(out);
        tout->grad_id = ag_next_id++;
        _ag_reg_set(tout->grad_id, tout);
        void* t_ag_exp[1] = { ta };
        ag_record_values(AG_EXP, tout->grad_id, ids, 1, NULL, (int)tout->size, t_ag_exp);
    }
    return out;
}

void* ag_neg(void* a) {
    FlintTensor* ta = _as_tensor(a);
    void* out = flt_neg(a);
    if (ta->grad_id >= 0) {
        int64_t ids[1] = {ta->grad_id};
        FlintTensor* tout = _as_tensor(out);
        tout->grad_id = ag_next_id++;
        _ag_reg_set(tout->grad_id, tout);
        void* t_ag_neg[1] = { ta };
        ag_record_values(AG_NEG, tout->grad_id, ids, 1, NULL, (int)tout->size, t_ag_neg);
    }
    return out;
}

// =========================================================================
// Optimizers
// =========================================================================
// Elements allocated so far in the m/v buffers. Each parameter gets a
// contiguous slice at its own offset; reallocating on growth would move every
// later slice, so instead the buffers are sized for the total and the offsets
// are recomputed from the parameter sizes each step.
static int64_t _adam_cap_elems = 0;

AdamState* adam_create(double lr) {
    AdamState* s = calloc(1, sizeof(AdamState));
    s->lr = lr; s->beta1 = 0.9; s->beta2 = 0.999; s->eps = 1e-8;
    s->t = 0; s->capacity = 16;
    s->ids = malloc(s->capacity * sizeof(int64_t));
    s->m = NULL; s->v = NULL;
    s->n_params = 0;
    return s;
}

// Ensure the moment buffers can hold `total` elements.
static void _adam_reserve(AdamState* s, int64_t total) {
    if (total <= _adam_cap_elems && s->m) return;
    int64_t cap = _adam_cap_elems ? _adam_cap_elems * 2 : 1024;
    while (cap < total) cap *= 2;
    s->m = realloc(s->m, (size_t)cap * sizeof(double));
    s->v = realloc(s->v, (size_t)cap * sizeof(double));
    memset(s->m, 0, (size_t)cap * sizeof(double));
    memset(s->v, 0, (size_t)cap * sizeof(double));
    _adam_cap_elems = cap;
}

// Total parameter elements tracked so far, used to grow the m/v buffers.
static int64_t _adam_total = 0;

void adam_add_param(AdamState* s, int64_t grad_id, int64_t size) {
    if (s->n_params >= s->capacity) {
        s->capacity *= 2;
        s->ids = realloc(s->ids, s->capacity * sizeof(int64_t));
    }
    s->ids[s->n_params++] = grad_id;
    _adam_reserve(s, (int64_t)s->capacity * (size > 0 ? size : 1));
}

// Apply the accumulated gradients to the parameters and advance the moments.
//
// Both optimizers were stubs: adam_step incremented t, computed a bias-
// correction factor, threw it away and did nothing, and sgd_step was an empty
// body. So "training" with this runtime moved no weights at all. Both now
// perform the real update on the tensors registered by *_add_param, whose
// gradients live in their `grad` buffers after ag_backward.
//
// Adam (Kingma & Ba): m = b1*m + (1-b1)*g;  v = b2*v + (1-b2)*g^2
//                     w -= lr * mhat / (sqrt(vhat) + eps)
// SGD with momentum: v = mu*v + g;  w -= lr * v
void adam_step(AdamState* s) {
    if (!s) return;
    s->t++;
    if (!s->m || !s->v) return;
    double b1c = 1.0 - pow(s->beta1, s->t);
    double b2c = 1.0 - pow(s->beta2, s->t);
    int64_t off = 0;
    for (int p = 0; p < s->n_params; p++) {
        FlintTensor* w = _ag_reg_get(s->ids[p]);
        if (!w || !w->grad) { off += w ? w->size : 0; continue; }
        for (int64_t i = 0; i < w->size; i++) {
            double g = w->grad[i];
            s->m[off + i] = s->beta1 * s->m[off + i] + (1.0 - s->beta1) * g;
            s->v[off + i] = s->beta2 * s->v[off + i] + (1.0 - s->beta2) * g * g;
            double mhat = s->m[off + i] / b1c;
            double vhat = s->v[off + i] / b2c;
            w->data[i] -= s->lr * mhat / (sqrt(vhat) + s->eps);
        }
        off += w->size;
    }
}

void adam_destroy(AdamState* s) { free(s->ids); free(s->m); free(s->v); free(s); }

SGDState* sgd_create(double lr, double momentum) {
    SGDState* s = calloc(1, sizeof(SGDState));
    s->lr = lr; s->momentum = momentum; s->capacity = 16;
    s->ids = malloc(s->capacity * sizeof(int64_t));
    s->velocities = NULL; s->n_params = 0; return s;
}

void sgd_add_param(SGDState* s, int64_t grad_id, int64_t size) {
    if (s->n_params >= s->capacity) {
        s->capacity *= 2;
        s->ids = realloc(s->ids, s->capacity * sizeof(int64_t));
    }
    s->ids[s->n_params++] = grad_id;
    (void)size;
}

void sgd_step(SGDState* s) {
    if (!s) return;
    for (int p = 0; p < s->n_params; p++) {
        FlintTensor* w = _ag_reg_get(s->ids[p]);
        if (!w || !w->grad) continue;
        if (!s->velocities) s->velocities = (double*)calloc((size_t)w->size, sizeof(double));
        if (!s->velocities) continue;
        for (int64_t i = 0; i < w->size; i++) {
            s->velocities[i] = s->momentum * s->velocities[i] + w->grad[i];
            w->data[i] -= s->lr * s->velocities[i];
        }
    }
}
void sgd_destroy(SGDState* s) { free(s->ids); free(s->velocities); free(s); }

// =========================================================================
// Utility for Flint: print tensor
// =========================================================================
void flt_print_tensor(void* t, int64_t max_elems) {
    FlintTensor* t_ = _as_tensor(t);
    int64_t n = t_->size < max_elems ? t_->size : max_elems;
    printf("[");
    for (int64_t i = 0; i < n; i++) {
        if (i > 0) printf(", ");
        printf("%f", t_->data[i]);
    }
    if (n < t_->size) printf(", ...");
    printf("]\n");
}

void flt_print_shape(void* t) {
    FlintTensor* t_ = _as_tensor(t);
    printf("[");
    for (int64_t i = 0; i < t_->ndim; i++) {
        if (i > 0) printf(" x ");
        printf("%ld", (long)t_->shape[i]);
    }
    printf("]\n");
}

// =========================================================================
// Weight checkpoints (FLWT v1) — inspectable, versioned tensor serialization.
//
// File layout, all little-endian, host byte order (the build targets
// aarch64 LE; a BE reader would need byte-swapping and is out of scope):
//   magic  "FLWT" (4 bytes)
//   u32    format version (1)
//   u32    number of tensor records
//   records, each:
//     u16    dtype (0 = f64, 1 = f32)
//     u16    rank
//     i64    rank dimension values
//     raw    element bytes in that dtype, row-major order
//
// Flint cannot hand-write an i64 array (the header comment on flt_create
// documents why), so the API is per-tensor: save one tensor per call,
// fresh or appended, then load each record back by index with its exact
// shape and dtype. Records are independent of each other, so stopping the
// program between the two is safe.
// =========================================================================

static void _flwt_u32(FILE* fp, uint32_t v) { fwrite(&v, sizeof v, 1, fp); }

static int _flwt_magic_ok(FILE* fp, uint32_t* version) {
    char m[4];
    if (fread(m, 1, 4, fp) != 4) return 0;
    if (memcmp(m, "FLWT", 4) != 0) return 0;
    if (fread(version, sizeof(uint32_t), 1, fp) != 1) return 0;
    return *version == 1;
}

int64_t flt_weights_save(const char* path, void* t, int64_t append) {
    FlintTensor* ts = _as_tensor(t);
    if (!path || !ts) { fprintf(stderr, "weights_save: invalid tensor or path\n"); return 0; }
    FILE* fp = fopen(path, append ? "ab" : "wb");
    if (!fp) { fprintf(stderr, "weights_save: cannot open %s\n", path); return 0; }
    if (!append) {
        fwrite("FLWT", 1, 4, fp);
        _flwt_u32(fp, 1);
        // The count field is a snapshot of nothing (records are appended
        // by later calls across the same or a fresh process); it stays 0
        // here by definition and flt_weights_count re-walks records, which
        // is the authoritative value.
        _flwt_u32(fp, 0);
    }
    uint16_t dtype = (uint16_t)(ts->dtype == FLT_F32 ? 1 : 0);
    uint16_t rank = (uint16_t)ts->ndim;
    fwrite(&dtype, sizeof dtype, 1, fp);
    fwrite(&rank, sizeof rank, 1, fp);
    for (int64_t i = 0; i < ts->ndim; i++)
        fwrite(&ts->shape[i], sizeof(int64_t), 1, fp);
    int64_t n = ts->size;
    if (ts->dtype == FLT_F32) fwrite(ts->data32, sizeof(float), (size_t)n, fp);
    else fwrite(ts->data, sizeof(double), (size_t)n, fp);
    if (ferror(fp)) { fprintf(stderr, "weights_save: write failed on %s\n", path); fclose(fp); return 0; }
    fclose(fp);
    return 1;
}

void* flt_weights_load(const char* path, int64_t index) {
    if (!path) return NULL;
    FILE* fp = fopen(path, "rb");
    if (!fp) { fprintf(stderr, "weights_load: cannot open %s\n", path); return NULL; }
    uint32_t version = 0;
    if (!_flwt_magic_ok(fp, &version)) {
        fprintf(stderr, "weights_load: not a FLWT v1 file: %s\n", path);
        fclose(fp);
        return NULL;
    }
    // magic (4) + version (4) + count (4): records start at offset 12.
    (void)fseek(fp, 4 + 4 + 4, SEEK_SET);
    int64_t i = 0;
    void* found = NULL;
    for (;;) {
        uint16_t dtype = 0, rank = 0;
        if (fread(&dtype, sizeof dtype, 1, fp) != 1) break;
        if (fread(&rank, sizeof rank, 1, fp) != 1) break;
        int64_t dims[8];
        if (rank == 0 || rank > 5) { fprintf(stderr, "weights_load: bad rank %u\n", rank); break; }
        if (fread(dims, sizeof(int64_t), rank, fp) != rank) break;
        int64_t n = 1;
        int bad = 0;
        for (int64_t k = 0; k < rank; k++) {
            if (dims[k] <= 0) { bad = 1; break; }
            n *= dims[k];
        }
        if (bad) { fprintf(stderr, "weights_load: bad dims\n"); break; }
        int64_t data_offset = ftell(fp);
        long skip = (long)n * (dtype == 1 ? (long)sizeof(float) : (long)sizeof(double));
        if (i == index) {
            // Rank 1-5 dispatch to the fixed-arity creators the header
            // documents for Flint-side use.
            void* out = NULL;
            if (dtype == 1) {
                switch (rank) {
                    case 1: out = flt_create_f32_1d(dims[0]); break;
                    case 2: out = flt_create_f32_2d(dims[0], dims[1]); break;
                    case 3: out = flt_create_f32_3d(dims[0], dims[1], dims[2]); break;
                    case 4: out = flt_create_f32_4d(dims[0], dims[1], dims[2], dims[3]); break;
                }
            } else {
                switch (rank) {
                    case 1: out = flt_create_1d(dims[0]); break;
                    case 2: out = flt_create_2d(dims[0], dims[1]); break;
                    case 3: out = flt_create_3d(dims[0], dims[1], dims[2]); break;
                    case 4: out = flt_create_4d(dims[0], dims[1], dims[2], dims[3]); break;
                    case 5: out = flt_create_5d(dims[0], dims[1], dims[2], dims[3], dims[4]); break;
                }
            }
            if (!out) { fprintf(stderr, "weights_load: rank %u is not supported for this dtype\n", (unsigned)rank); break; }
            FlintTensor* to = _as_tensor(out);
            if (dtype == 1) {
                if (fread(to->data32, sizeof(float), (size_t)n, fp) != (size_t)n) { fprintf(stderr, "weights_load: truncated record\n"); break; }
            } else {
                if (fread(to->data, sizeof(double), (size_t)n, fp) != (size_t)n) { fprintf(stderr, "weights_load: truncated record\n"); break; }
            }
            if (!out) { fclose(fp); return NULL; }
            found = out;
            break;
        }
        (void)fseek(fp, data_offset + skip, SEEK_SET);
        i++;
    }
    fclose(fp);
    return found;
}

int64_t flt_weights_count(const char* path) {
    if (!path) return 0;
    FILE* fp = fopen(path, "rb");
    if (!fp) return 0;
    uint32_t version = 0;
    if (!_flwt_magic_ok(fp, &version)) { fclose(fp); return 0; }
    (void)fseek(fp, 4 + 4 + 4, SEEK_SET);
    int64_t count = 0;
    for (;;) {
        uint16_t dtype = 0, rank = 0;
        if (fread(&dtype, sizeof dtype, 1, fp) != 1) break;
        if (fread(&rank, sizeof rank, 1, fp) != 1) break;
        if (rank == 0 || rank > 5) break;
        int64_t dims[8];
        if (fread(dims, sizeof(int64_t), rank, fp) != rank) break;
        int64_t n = 1;
        int bad = 0;
        for (int64_t k = 0; k < rank; k++) {
            if (dims[k] <= 0) { bad = 1; break; }
            n *= dims[k];
        }
        if (bad) break;
        long skip = (long)n * (dtype == 1 ? (long)sizeof(float) : (long)sizeof(double));
        int64_t here = ftell(fp);
        if (fseek(fp, here + skip, SEEK_SET) != 0) break;
        count++;
    }
    fclose(fp);
    return count;
}
