// flint_nn.h — neural network layers.
//
// The tensor library had primitives (matmul, elementwise, activations, a tape,
// Adam) but no layers, so a model had to be hand-assembled from matmuls and
// there was no way to say "convolution" at all. These are the standard layers
// in f64, row-major, NCHW for convolutions.
//
// Each layer is a pure function on tensors and a no-op for gradients: they are
// meant to sit inside an ag_* forward pass, and the tape records the primitive
// ops it decomposes into. flt_conv2d therefore returns a new tensor and the
// caller is responsible for wiring it into the graph with ag_* where the
// gradient matters.

#ifndef FLINT_NN_H
#define FLINT_NN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- convolution --------------------------------------------------------
// out[n][oc][oh][ow] = bias[oc] + sum_ic sum_kh sum_kw
//                                 in[n][ic][ih][iw] * w[oc][ic][kh][kw]
//
// in  : (N, IC, IH, IW)
// w   : (OC, IC, KH, KW)
// bias: (OC), or NULL for no bias
// stride and padding apply to the spatial dims only.
void* flt_conv2d(void* in, void* w, void* bias, int64_t stride, int64_t padding);

// 2x2 max or average pooling over the spatial dims.
void* flt_maxpool2d(void* in, int64_t k, int64_t stride);
void* flt_avgpool2d(void* in, int64_t k, int64_t stride);

// Global average pooling over H,W -> (N, C). Very common before a dense head.
void* flt_global_avgpool(void* in);

// ---- normalisation ------------------------------------------------------
// Per-channel (batch-statistics) like BatchNorm at inference.
void* flt_batchnorm(void* in, void* gamma, void* beta, double eps);
// Per-sample, per-channel mean/variance computed from this tensor.
void* flt_layer_norm(void* in, void* gamma, void* beta, double eps);
// Fixed, learned scale/shift with no statistics: the simplest normaliser and
// what most transformers use.
void* flt_rms_norm(void* in, void* gamma, double eps);

// ---- activations and regularisation -------------------------------------
void* flt_softmax(void* in);          // per row (last dim)
void* flt_dropout(void* in, double p); // scales survivors by 1/(1-p)
void* flt_dropout_inplace(void* in, double p);
double flt_l2_reg(void* w);           // sum of squares, for a penalty term

// ---- shape plumbing -----------------------------------------------------
// (N,C,H,W) -> (N, C*H*W), for the dense head after a conv stack.
void* flt_flatten(void* in);

// ---- element access ------------------------------------------------------
// Writing a tensor element-by-element through a pointer needs flint_array_get
// on a vec handle, which does not apply to a tensor. These take the tensor
// directly, so ML code can fill one without a parallel literal.
//
//   flt_set_at(t, idx, v)     write element idx (bounds-checked)
//   flt_get_at(t, idx) -> f64  read element idx
//
// An out-of-range index panics rather than corrupting memory, matching the
// rest of the library.
void  flt_set_at(void* t, int64_t idx, double v);
double flt_get_at(void* t, int64_t idx);

// ---- parameter init -----------------------------------------------------
// Xavier/Glorot uniform, scaled for tanh; He for ReLU. Random weights are the
// difference between a network that trains and one that does not.
void* flt_xavier_uniform(void* t, double scale);
void* flt_he_uniform(void* t, double scale);

#ifdef __cplusplus
}
#endif

#endif // FLINT_NN_H