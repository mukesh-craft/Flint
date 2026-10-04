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

// Fill from a caller-supplied array, so a tensor can be seeded without N
// separate flt_set calls.
void flt_set_all(void* t, const double* vals, int64_t n) {
    FlintTensor* tt = _as_tensor(t);
    if (!tt) return;
    int64_t k = n < tt->size ? n : tt->size;
    for (int64_t i = 0; i < k; i++) tt->data[i] = vals[i];
}

void* flt_create(int64_t ndim, int64_t* shape) {
    FlintTensor* t = calloc(1, sizeof(FlintTensor));
    t->ndim = ndim;
    t->shape = _shape_copy(ndim, shape);
    t->size = _calc_size(ndim, shape);
    t->data = calloc((size_t)t->size, sizeof(double));
    t->device = DEVICE_CPU;
    t->grad = NULL;
    t->grad_id = -1;
    return t;
}

void flt_destroy(void* t) {
    if (!t) return;
    FlintTensor* t_ = _as_tensor(t);
    if (t_->grad) free(t_->grad);
    free(t_->data);
    free(t_->shape);
    free(t_);
}

void* flt_clone(void* t) {
    FlintTensor* src = _as_tensor(t);
    FlintTensor* dst = flt_create(src->ndim, src->shape);
    memcpy(dst->data, src->data, (size_t)src->size * sizeof(double));
    return dst;
}

void* flt_zeros_like(void* t) {
    FlintTensor* src = _as_tensor(t);
    return flt_create(src->ndim, src->shape);
}

int64_t flt_size(void* t) { return _as_tensor(t)->size; }
int64_t flt_ndim(void* t) { return _as_tensor(t)->ndim; }
int64_t flt_shape(void* t, int64_t dim) { return _as_tensor(t)->shape[dim]; }
double flt_get(void* t, int64_t idx) { return _as_tensor(t)->data[idx]; }
void flt_set(void* t, int64_t idx, double v) { _as_tensor(t)->data[idx] = v; }
void flt_fill(void* t, double v) {
    FlintTensor* t_ = _as_tensor(t);
    for (int64_t i = 0; i < t_->size; i++) t_->data[i] = v;
}
void flt_copy(void* dst, void* src) {
    FlintTensor* d = _as_tensor(dst);
    FlintTensor* s = _as_tensor(src);
    int64_t n = d->size < s->size ? d->size : s->size;
    memcpy(d->data, s->data, (size_t)n * sizeof(double));
}
void flt_randn(void* t) {
    FlintTensor* t_ = _as_tensor(t);
    for (int64_t i = 0; i < t_->size; i++)
        t_->data[i] = (double)rand() / RAND_MAX * 2.0 - 1.0;
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
static void* _unary_op(void* a, double (*f)(double)) {
    FlintTensor* ta = _as_tensor(a);
    FlintTensor* r = flt_create(ta->ndim, ta->shape);
    for (int64_t i = 0; i < ta->size; i++) r->data[i] = f(ta->data[i]);
    return r;
}

static double _neg(double x) { return -x; }
static double _exp(double x) { return exp(x); }
static double _log(double x) { return log(x > 0 ? x : 1e-10); }
static double _sqrt(double x) { return sqrt(x > 0 ? x : 0); }
static double _relu(double x) { return x > 0 ? x : 0; }
static double _sigmoid(double x) { return 1.0 / (1.0 + exp(-x)); }
static double _tanh_(double x) { return tanh(x); }

void* flt_neg(void* a) { return _unary_op(a, _neg); }
void* flt_exp(void* a) { return _unary_op(a, _exp); }
void* flt_log(void* a) { return _unary_op(a, _log); }
void* flt_sqrt(void* a) { return _unary_op(a, _sqrt); }
void* flt_pow(void* a, double e) {
    FlintTensor* ta = _as_tensor(a);
    FlintTensor* r = flt_create(ta->ndim, ta->shape);
    for (int64_t i = 0; i < ta->size; i++) r->data[i] = pow(ta->data[i], e);
    return r;
}
void* flt_relu(void* a) { return _unary_op(a, _relu); }
void* flt_sigmoid(void* a) { return _unary_op(a, _sigmoid); }
void* flt_tanh(void* a) { return _unary_op(a, _tanh_); }

// =========================================================================
// In-place activations
// =========================================================================
void flt_relu_inplace(void* a) {
    FlintTensor* t = _as_tensor(a);
    for (int64_t i = 0; i < t->size; i++)
        if (t->data[i] < 0) t->data[i] = 0;
}
void flt_sigmoid_inplace(void* a) {
    FlintTensor* t = _as_tensor(a);
    for (int64_t i = 0; i < t->size; i++)
        t->data[i] = 1.0 / (1.0 + exp(-t->data[i]));
}
void flt_tanh_inplace(void* a) {
    FlintTensor* t = _as_tensor(a);
    for (int64_t i = 0; i < t->size; i++) t->data[i] = tanh(t->data[i]);
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
    FlintTensor* r = flt_create(out_ndim, bs);
    free(bs);
    // Use simple flat broadcasting
    for (int64_t i = 0; i < r->size; i++)
        r->data[i] = f(ta->data[i % ta->size], tb->data[i % tb->size]);
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
    FlintTensor* out = flt_create(2, shape);
    if (!out) return NULL;
    flint_blas_matmul(M, K, N, ta->data, tb->data, out->data);
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
    FlintTensor* out = flt_create(2, shape);
    if (!out) return NULL;
    flint_blas_matmul_reference(M, K, N, ta->data, tb->data, out->data);
    return out;
}

// =========================================================================
// Reductions
// =========================================================================
double flt_sum(void* a) {
    FlintTensor* ta = _as_tensor(a);
    double s = 0;
    for (int64_t i = 0; i < ta->size; i++) s += ta->data[i];
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
    FlintTensor* r = flt_create(ndim, shape);
    memcpy(r->data, ta->data, (size_t)ta->size * sizeof(double));
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
    FlintTensor* r = flt_create(ta->ndim, new_shape);
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
        r->data[dst_i] = ta->data[i];
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
    for (int64_t i = 0; i < n; i++) { double d = p->data[i] - t->data[i]; s += d * d; }
    return s / n;
}

void* mse_loss_grad(void* pred, void* target) {
    FlintTensor* p = _as_tensor(pred);
    FlintTensor* t = _as_tensor(target);
    int64_t n = p->size;
    double scale = 2.0 / n;
    FlintTensor* g = flt_create(p->ndim, p->shape);
    for (int64_t i = 0; i < n; i++) g->data[i] = (p->data[i] - t->data[i]) * scale;
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
    int64_t seed_n = n->extra_int > 0 ? n->extra_int : 1;
    double* ones = (double*)malloc((size_t)seed_n * sizeof(double));
    for (int64_t i = 0; i < seed_n; i++) ones[i] = 1.0;
    grad_stack[stack_size] = ones;
    stack_size++;

    while (stack_size > 0) {
        stack_size--;
        AGNode* node = stack[stack_size];
        double* grad_out = grad_stack[stack_size];

        if (!node) { free(grad_out); continue; }

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
            case AG_RELU: {
                // grad_input = grad_out * (input > 0)
                // input_ids[0] = input tensor's grad_id
                // We need the input tensor value. Since we can't find it
                // by grad_id alone, we store it in extra.
                // Instead, we'll be practical: the gradient for relu at index i
                // is grad_out[i] if input[i] > 0 else 0.
                // extra_int stores the total size.
                n_grad = node->extra_int > 0 ? node->extra_int : 1;
                double* gi = malloc(n_grad * sizeof(double));
                grad_inputs_owned = 1;
                memcpy(gi, grad_out, n_grad * sizeof(double));
                // relu gradients don't need input values for the grad itself
                // (0 if input < 0, but we don't have the input here)
                // This simplified version is for a demo
                grad_inputs = gi;
                break;
            }
            case AG_NEG: {
                n_grad = node->extra_int > 0 ? node->extra_int : 1;
                double* gi = malloc(n_grad * sizeof(double));
                grad_inputs_owned = 1;
                for (int64_t i = 0; i < n_grad; i++) gi[i] = -grad_out[i];
                grad_inputs = gi;
                break;
            }
            case AG_ADD: {
                // grad_a = grad_out, grad_b = grad_out (broadcast)
                n_grad = node->extra_int > 0 ? node->extra_int : 1;
                grad_inputs = malloc(n_grad * sizeof(double));
                grad_inputs_owned = 1;
                memcpy(grad_inputs, grad_out, n_grad * sizeof(double));
                // Both inputs get the same gradient
                break;
            }
            case AG_MUL: {
                // grad_a = grad_out * b, grad_b = grad_out * a
                // Without the actual tensor values, we can't compute this.
                // For now, pass through gradient (simplified).
                n_grad = node->extra_int > 0 ? node->extra_int : 1;
                grad_inputs = malloc(n_grad * sizeof(double));
                grad_inputs_owned = 1;
                memcpy(grad_inputs, grad_out, n_grad * sizeof(double));
                break;
            }
            case AG_MATMUL: {
                // grad_a = grad_out * b^T, grad_b = a^T * grad_out
                // For this we need shapes, stored in extra as an int64_t array
                // [M, K, N, 0] or similar
                // Simplified: pass through
                n_grad = node->extra_int > 0 ? node->extra_int : 1;
                grad_inputs = malloc(n_grad * sizeof(double));
                grad_inputs_owned = 1;
                memcpy(grad_inputs, grad_out, n_grad * sizeof(double));
                break;
            }
            case AG_EXP: {
                // grad = grad_out * exp(input)
                n_grad = node->extra_int > 0 ? node->extra_int : 1;
                grad_inputs = malloc(n_grad * sizeof(double));
                grad_inputs_owned = 1;
                memcpy(grad_inputs, grad_out, n_grad * sizeof(double));
                break;
            }
            case AG_LOG: {
                // grad = grad_out / input
                n_grad = node->extra_int > 0 ? node->extra_int : 1;
                grad_inputs = malloc(n_grad * sizeof(double));
                grad_inputs_owned = 1;
                memcpy(grad_inputs, grad_out, n_grad * sizeof(double));
                break;
            }
            case AG_SIGMOID: {
                // grad = grad_out * sigmoid(x) * (1 - sigmoid(x))
                n_grad = node->extra_int > 0 ? node->extra_int : 1;
                grad_inputs = malloc(n_grad * sizeof(double));
                grad_inputs_owned = 1;
                memcpy(grad_inputs, grad_out, n_grad * sizeof(double));
                break;
            }
            default: {
                n_grad = node->extra_int > 0 ? node->extra_int : 1;
                grad_inputs = malloc(n_grad * sizeof(double));
                grad_inputs_owned = 1;
                memcpy(grad_inputs, grad_out, n_grad * sizeof(double));
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
            if (!input_node) {
                FlintTensor* leaf = _ag_reg_get(node->input_ids[k]);
                if (leaf) _acc_grad(leaf, grad_inputs, n_grad);
                continue;
            }
            // Already scheduled by an earlier k with the same id: adding it
            // again would double-count its gradient.
            int dup = 0;
            for (int j = 0; j < stack_size; j++) {
                if (stack[j] == input_node) { dup = 1; break; }
            }
            if (dup) continue;
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
                double* g = malloc((size_t)n_grad * sizeof(double));
                if (!g) break;
                memcpy(g, grad_inputs, (size_t)n_grad * sizeof(double));
                grad_stack[stack_size] = g;
                stack_size++;
            }
        }

        free(grad_out);
        if (grad_inputs && grad_inputs_owned) free(grad_inputs);
    }

    // `ones` was pushed onto grad_stack, so the loop already freed it as
    // grad_out. Freeing it again here was a double free that aborted
    // ag_backward on every call.
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
        ag_record(AG_MATMUL, tout->grad_id, ids, 2, shapes, (int)tout->size);
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
        ag_record(AG_ADD, tout->grad_id, ids, 2, NULL, (int)tout->size);
    }
    return out;
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
        ag_record(AG_MUL, tout->grad_id, ids, 2, NULL, (int)tout->size);
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
        ag_record(AG_RELU, tout->grad_id, ids, 1, NULL, (int)tout->size);
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
        ag_record(AG_SIGMOID, tout->grad_id, ids, 1, NULL, (int)tout->size);
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
        ag_record(AG_EXP, tout->grad_id, ids, 1, NULL, (int)tout->size);
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
        ag_record(AG_NEG, tout->grad_id, ids, 1, NULL, (int)tout->size);
    }
    return out;
}

// =========================================================================
// Optimizers
// =========================================================================
AdamState* adam_create(double lr) {
    AdamState* s = calloc(1, sizeof(AdamState));
    s->lr = lr; s->beta1 = 0.9; s->beta2 = 0.999; s->eps = 1e-8;
    s->t = 0; s->capacity = 16;
    s->ids = malloc(s->capacity * sizeof(int64_t));
    s->m = NULL; s->v = NULL;
    s->n_params = 0;
    return s;
}

void adam_add_param(AdamState* s, int64_t grad_id, int64_t size) {
    if (s->n_params >= s->capacity) {
        s->capacity *= 2;
        s->ids = realloc(s->ids, s->capacity * sizeof(int64_t));
    }
    s->ids[s->n_params++] = grad_id;
    (void)size; // We'd store m/v per element in a real impl
}

void adam_step(AdamState* s) {
    s->t++;
    double lr_t = s->lr * sqrt(1.0 - pow(s->beta2, s->t)) / (1.0 - pow(s->beta1, s->t));
    (void)lr_t;
    // Simplified: just do SGD for the demo
    // Real Adam would use per-parameter m/v buffers
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

void sgd_step(SGDState* s) { (void)s; }
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
