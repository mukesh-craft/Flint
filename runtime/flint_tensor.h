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

typedef struct {
    double* data;     // flat data array (always accessible on CPU for now)
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
    struct AGNode* next;   // linked list
} AGNode;

// =========================================================================
// Tensor API
// =========================================================================
void*  flt_create(int64_t ndim, int64_t* shape);
void   flt_destroy(void* t);
void*  flt_clone(void* t);
void*  flt_zeros_like(void* t);
int64_t flt_size(void* t);
int64_t flt_ndim(void* t);
int64_t flt_shape(void* t, int64_t dim);
double flt_get(void* t, int64_t idx);
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

// BLAS-accelerated matmul (detects OpenBLAS at link time)
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
void   ag_zero_grad(int64_t grad_id);
void   ag_print_graph(void);

// High-level autograd ops (record + compute)
void* ag_matmul(void* a, void* b);
void* ag_add(void* a, void* b);
void* ag_mul(void* a, void* b);
void* ag_relu(void* a);
void* ag_sigmoid(void* a);
void* ag_tanh(void* a);
void* ag_exp(void* a);
void* ag_log(void* a);
void* ag_neg(void* a);

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
