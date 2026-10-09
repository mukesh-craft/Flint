#ifndef FLINT_TENSOR_H
#define FLINT_TENSOR_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

// =========================================================================
// Tensor struct
// =========================================================================
typedef enum { DEVICE_CPU = 0, DEVICE_CUDA = 1, DEVICE_ROCM = 2 } DeviceType;

// Element type. FLT_F64 is the default and keeps every existing program
// working unchanged; FLT_F32 halves memory and halves the bytes moved by
// matmul, which is what a real model wants.
//
// f32 tensors compute in double and narrow on store. That costs one convert
// per element but gives transcendentals (exp, tanh, sigmoid) double-precision
// accuracy in the last place it matters -- after the value is already final --
// and keeps a single implementation of every math function instead of two.
typedef enum { FLT_F64 = 0, FLT_F32 = 1 } FlintDType;

typedef struct {
    // Exactly one of these is non-NULL, selected by dtype. An op handed the
    // wrong one returns NULL with a diagnostic rather than reinterpreting
    // memory, so a dtype mix-up is a message, not a segfault.
    double* data;     // FLT_F64 storage (NULL for an f32 tensor)
    float*  data32;   // FLT_F32 storage (NULL for an f64 tensor)
    FlintDType dtype;
    int64_t* shape;   // shape dimensions
    int64_t ndim;     // number of dimensions
    int64_t size;     // total elements
    DeviceType device;
    // Gradient (for autograd)
    double* grad;     // gradient buffer (NULL if no gradient needed)
    int64_t grad_id;  // unique ID for gradient tracking (-1 if not tracked)
} FlintTensor;

// =========================================================================
// Operation types for autograd
// =========================================================================
typedef enum {
    AG_MATMUL, AG_ADD, AG_SUB, AG_MUL, AG_DIV,
    AG_NEG, AG_EXP, AG_LOG, AG_SQRT, AG_POW,
    AG_RELU, AG_SIGMOID, AG_TANH,
    AG_SUM, AG_MEAN,
    AG_RESHAPE, AG_TRANSPOSE,
    AG_EMPTY  // marker
} AGOpType;

// =========================================================================
// Autograd node
// =========================================================================
typedef struct AGNode {
    AGOpType op;
    int n_inputs;
    int64_t* input_ids;    // grad_id of each input
    int64_t output_id;     // grad_id of output
    void* extra;           // op-specific extra data
    int extra_int;
    // Copies of the input tensors' DATA, captured when the op was recorded.
    // Backward needs them: d(ab)/da = grad * b, so without the operand values
    // the tape can only pass the gradient through, which is what it did -- and
    // why ag_mul returned the seed instead of the real derivative. `values` is
    // n_inputs tensors laid out contiguously (each with its own shape), so the
    // shapes are recorded alongside in value_ndims.
    double* values;
    int64_t* value_ndims;
    int64_t* value_shapes; // n_inputs * value_ndims[i] entries
    struct AGNode* next;   // linked list
} AGNode;

// =========================================================================
// Tensor API
// =========================================================================
void*  flt_create(int64_t ndim, int64_t* shape);

// ---- f32 tensors -------------------------------------------------------
// Same API, half the memory and half the matmul bandwidth. Constructors
// mirror the f64 ones; every op below dispatches on the tensor's dtype, so a
// program mixes f32 and f64 freely and the binary ops promote to f64 when the
// two sides differ.
void*  flt_empty(int64_t ndim, int64_t* shape, int32_t is_f32);
void*  flt_create_f32_1d(int64_t n0);
void*  flt_create_f32_2d(int64_t n0, int64_t n1);
void*  flt_create_f32_3d(int64_t n0, int64_t n1, int64_t n2);
void*  flt_create_f32_4d(int64_t n0, int64_t n1, int64_t n2, int64_t n3);
void*  flt_zeros_like_f32(void* t);      // same shape and dtype, zeroed
void*  flt_clone_f32(void* t);           // same shape and dtype, copied
int64_t flt_is_f32(void* t);             // 1 for f32, 0 for f64
float*  flt_data_f32(void* t);           // backing buffer; NULL if the tensor is f64
int64_t flt_bytes(void* t);              // payload size, for comparing footprint

// Casts. Both allocate a new tensor.
void*  flt_to_f32(void* t);
void*  flt_to_f64(void* t);
// Rank-specific constructors. A Flint array is a vec handle, not an i64
// buffer, so a caller in Flint cannot supply the `int64_t*` that flt_create
// wants -- these take the dimensions directly.
void*  flt_create_1d(int64_t n0);
void*  flt_create_2d(int64_t n0, int64_t n1);
void*  flt_create_3d(int64_t n0, int64_t n1, int64_t n2);
void*  flt_create_4d(int64_t n0, int64_t n1, int64_t n2, int64_t n3);
void*  flt_create_5d(int64_t n0, int64_t n1, int64_t n2, int64_t n3, int64_t n4);
void   flt_set_all(void* t, const double* vals, int64_t n);
void   flt_destroy(void* t);
void*  flt_clone(void* t);
void*  flt_zeros_like(void* t);
int64_t flt_size(void* t);
int64_t flt_ndim(void* t);
int64_t flt_shape(void* t, int64_t dim);
double flt_get(void* t, int64_t idx);
// Backing buffer, row-major and flat regardless of ndim. Valid while the
// tensor lives. Layers write through this instead of looping flt_set.
double* flt_data(void* t);
void   flt_set(void* t, int64_t idx, double v);
void   flt_fill(void* t, double v);
void   flt_copy(void* dst, void* src);
void   flt_randn(void* t);
void   flt_rand(void* t);
void   flt_uniform(void* t, double lo, double hi);

// Math ops (returns new tensor)
void* flt_neg(void* a);
void* flt_exp(void* a);
void* flt_log(void* a);
void* flt_sqrt(void* a);
void* flt_pow(void* a, double e);
void* flt_relu(void* a);
void* flt_sigmoid(void* a);
void* flt_tanh(void* a);
void* flt_add_tt(void* a, void* b);
void* flt_sub_tt(void* a, void* b);
void* flt_mul_tt(void* a, void* b);
void* flt_div_tt(void* a, void* b);
void* flt_matmul(void* a, void* b);
double flt_sum(void* a);
double flt_mean(void* a);
void*  flt_reshape(void* a, int64_t ndim, int64_t* shape);
void*  flt_transpose(void* a, int64_t dim0, int64_t dim1);

// Deterministic matmul: always the portable reference loop, never BLAS.
// Same result as flt_matmul, so it is the oracle the test gate compares the
// live (possibly BLAS-accelerated) path against.
void* flt_matmul_blas(void* a, void* b);

// In-place ops
void flt_relu_inplace(void* a);
void flt_sigmoid_inplace(void* a);
void flt_tanh_inplace(void* a);

// =========================================================================
// Autograd API
// =========================================================================
void   ag_init(void);
void   ag_cleanup(void);
int64_t ag_track_tensor(void* tensor);
void   ag_record(int64_t op_type, int64_t output_id, int64_t* input_ids,
                  int n_inputs, void* extra, int extra_int);
void   ag_backward(int64_t output_id);
double* ag_get_grad(int64_t grad_id);
// The grad_id an ag_* op assigned to its output tensor. Needed because the op
// sets grad_id itself: re-tracking with ag_track_tensor allocates a second id
// and orphans the recorded node, so ag_backward would find nothing.
int64_t ag_grad_id(void* tensor);
// Read gradient element `idx` of the tensor tracked as `grad_id`. Provided
// because `double*` cannot be indexed from Flint -- `ptr[i]` is string
// indexing, so `g[0]` on a gradient buffer traps on the length check.
double ag_grad_at(int64_t grad_id, int64_t idx);
int64_t ag_grad_len(int64_t grad_id);
void   ag_zero_grad(int64_t grad_id);
void   ag_print_graph(void);

// High-level autograd ops (record + compute)
void* ag_matmul(void* a, void* b);
void* ag_add(void* a, void* b);
void* ag_mul(void* a, void* b);
void* ag_relu(void* a);
void* ag_sigmoid(void* a);
void* ag_tanh(void* a);
void* ag_log(void* a);
void* ag_exp(void* a);
void* ag_log(void* a);
void* ag_neg(void* a);
void* ag_sub(void* a, void* b);
void* ag_sum(void* a);
void* ag_mean(void* a);
void  ag_seed_scalar(int64_t output_id, double v);

// =========================================================================
// Optimizer API
// =========================================================================
typedef struct {
    double lr;
    double beta1, beta2;
    double eps;
    int t;
    // Per-parameter state stored as parallel arrays indexed by grad_id
    double* m;       // first moment
    double* v;       // second moment
    int64_t* ids;    // parameter grad_ids
    int n_params;
    int capacity;
} AdamState;

AdamState* adam_create(double lr);
void adam_add_param(AdamState* s, int64_t grad_id, int64_t size);
void adam_step(AdamState* s);
void adam_destroy(AdamState* s);

// SGD
typedef struct {
    double lr;
    double momentum;
    int64_t* ids;
    double* velocities;
    int n_params;
    int capacity;
} SGDState;

SGDState* sgd_create(double lr, double momentum);
void sgd_add_param(SGDState* s, int64_t grad_id, int64_t size);
void sgd_step(SGDState* s);
void sgd_destroy(SGDState* s);

// =========================================================================
// Loss functions
// =========================================================================
double mse_loss(void* pred, void* target);
void*  mse_loss_grad(void* pred, void* target);

#ifdef __cplusplus
}
#endif
#endif // FLINT_TENSOR_H
