// flint_nn.c — neural network layers. Per flint_nn.h.

#include "flint_nn.h"
#include "flint_tensor.h"

// The runtime RNG, so a seeded program reproduces exactly.
extern double flint_rand_f64(void);

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

extern void flint_panic(const char*);

// Tensors are opaque handles here; a 4-D view is unpacked from the shape.
static int64_t sh(void* t, int64_t d) { return flt_shape(t, d); }
static double* dat(void* t) { return (double*)flt_data(t); }

// ------------------------------------------------------------------ conv2d
void* flt_conv2d(void* in, void* w, void* bias, int64_t stride, int64_t padding) {
    if (!in || !w) return NULL;
    int64_t N = sh(in, 0), IC = sh(in, 1), IH = sh(in, 2), IW = sh(in, 3);
    int64_t OC = sh(w, 0), KH = sh(w, 2), KW = sh(w, 3);
    if (sh(in, 0) != N) return NULL;

    int64_t OH = (IH + 2 * padding - KH) / stride + 1;
    int64_t OW = (IW + 2 * padding - KW) / stride + 1;
    if (OH <= 0 || OW <= 0) return NULL;

    int64_t oshape[4] = { N, OC, OH, OW };
    void* out = flt_create(4, oshape);
    if (!out) return NULL;

    const double* I = dat(in);
    const double* W = dat(w);
    const double* B = bias ? dat(bias) : NULL;
    double* O = dat(out);

    // Direct convolution. Correct and clear; the common case is small enough
    // (channel counts in the tens) that a naive loop still beats a
    // cache-blocked im2col here, and a blocked version is the obvious next
    // optimisation if a large conv shows up in a benchmark.
    for (int64_t n = 0; n < N; n++) {
        for (int64_t oc = 0; oc < OC; oc++) {
            for (int64_t oh = 0; oh < OH; oh++) {
                for (int64_t ow = 0; ow < OW; ow++) {
                    double acc = B ? B[oc] : 0.0;
                    for (int64_t ic = 0; ic < IC; ic++) {
                        for (int64_t kh = 0; kh < KH; kh++) {
                            int64_t ih = oh * stride + kh - padding;
                            if (ih < 0 || ih >= IH) continue;   // zero pad
                            for (int64_t kw = 0; kw < KW; kw++) {
                                int64_t iw = ow * stride + kw - padding;
                                if (iw < 0 || iw >= IW) continue;
                                acc += I[((n * IC + ic) * IH + ih) * IW + iw]
                                     * W[((oc * IC + ic) * KH + kh) * KW + kw];
                            }
                        }
                    }
                    O[((n * OC + oc) * OH + oh) * OW + ow] = acc;
                }
            }
        }
    }
    return out;
}

// ------------------------------------------------------------------ pooling
// Shared walk for max and average pooling over the last two dims.
static void* pool2d(void* in, int64_t k, int64_t stride, int want_max) {
    if (!in || k <= 0) return NULL;
    int64_t N = sh(in, 0), C = sh(in, 1), H = sh(in, 2), W = sh(in, 3);
    int64_t OH = (H - k) / stride + 1;
    int64_t OW = (W - k) / stride + 1;
    if (OH <= 0 || OW <= 0) return NULL;

    int64_t oshape[4] = { N, C, OH, OW };
    void* out = flt_create(4, oshape);
    if (!out) return NULL;
    const double* I = dat(in);
    double* O = dat(out);

    for (int64_t n = 0; n < N; n++)
        for (int64_t c = 0; c < C; c++)
            for (int64_t oh = 0; oh < OH; oh++)
                for (int64_t ow = 0; ow < OW; ow++) {
                    double best = want_max ? -INFINITY : 0.0;
                    double sum = 0.0;
                    for (int64_t kh = 0; kh < k; kh++)
                        for (int64_t kw = 0; kw < k; kw++) {
                            double v = I[((n * C + c) * H + oh * stride + kh) * W + ow * stride + kw];
                            sum += v;
                            if (want_max && v > best) best = v;
                        }
                    O[((n * C + c) * OH + oh) * OW + ow] = want_max ? best : sum / (double)(k * k);
                }
    return out;
}

void* flt_maxpool2d(void* in, int64_t k, int64_t stride) { return pool2d(in, k, stride, 1); }
void* flt_avgpool2d(void* in, int64_t k, int64_t stride) { return pool2d(in, k, stride, 0); }

void* flt_global_avgpool(void* in) {
    if (!in) return NULL;
    int64_t N = sh(in, 0), C = sh(in, 1), H = sh(in, 2), W = sh(in, 3);
    if (H <= 0 || W <= 0) return NULL;
    int64_t oshape[2] = { N, C };
    void* out = flt_create(2, oshape);
    if (!out) return NULL;
    const double* I = dat(in);
    double* O = dat(out);
    double scale = 1.0 / (double)(H * W);
    for (int64_t n = 0; n < N; n++)
        for (int64_t c = 0; c < C; c++) {
            double s = 0.0;
            for (int64_t i = 0; i < H * W; i++) s += I[(n * C + c) * H * W + i];
            O[n * C + c] = s * scale;
        }
    return out;
}

// ---------------------------------------------------------- normalisation
// Scale and shift by per-channel gamma/beta. Statistics are assumed already
// normalised (the inference-time form of BatchNorm); flt_batchnorm_infer
// computes them, this is the pure affine part.
static void* affine_channel(void* in, void* gamma, void* beta, double eps) {
    if (!in) return NULL;
    int64_t nd = flt_ndim(in);
    if (nd < 2) return NULL;
    int64_t C = sh(in, 1);
    int64_t total = flt_size(in);
    int64_t plane = total / (C > 0 ? C : 1);
    void* out = flt_clone(in);
    if (!out) return NULL;
    double* O = dat(out);
    const double* G = gamma ? dat(gamma) : NULL;
    const double* B = beta ? dat(beta) : NULL;
    for (int64_t c = 0; c < C; c++) {
        double g = G ? G[c] : 1.0;
        double b = B ? B[c] : 0.0;
        for (int64_t p = 0; p < plane; p++) {
            int64_t i = c * plane + p;
            O[i] = O[i] * g + b;
        }
    }
    (void)eps;
    return out;
}

void* flt_batchnorm(void* in, void* gamma, void* beta, double eps) {
    return affine_channel(in, gamma, beta, eps);
}

// LayerNorm: normalise each sample's channel vector to zero mean / unit var,
// then apply gamma and beta. Statistics come from this tensor, so it works
// without a training-mode flag.
void* flt_layer_norm(void* in, void* gamma, void* beta, double eps) {
    if (!in) return NULL;
    int64_t C = sh(in, 1);
    int64_t total = flt_size(in);
    if (C <= 0) return NULL;
    // Normalise ACROSS the channel axis: each group of C consecutive values is
    // one sample's feature vector. plane is that width (C), NOT total/C --
    // which for a (1, C) tensor is 1, and would normalise one value at a time
    // so every output was its own mean, i.e. zero.
    int64_t plane = C;
    void* out = flt_clone(in);
    if (!out) return NULL;
    const double* I = dat(in);
    double* O = dat(out);
    const double* G = gamma ? dat(gamma) : NULL;
    const double* B = beta ? dat(beta) : NULL;
    for (int64_t start = 0; start + plane <= total; start += plane) {
        double mean = 0.0;
        for (int64_t p = 0; p < plane; p++) mean += I[start + p];
        mean /= (double)plane;
        double var = 0.0;
        for (int64_t p = 0; p < plane; p++) {
            double d = I[start + p] - mean;
            var += d * d;
        }
        var /= (double)plane;
        double inv = 1.0 / sqrt(var + eps);
        for (int64_t p = 0; p < plane; p++) {
            int64_t c = start + p;
            double g = G ? G[p] : 1.0;
            double b = B ? B[p] : 0.0;
            O[c] = (I[c] - mean) * inv * g + b;
        }
    }
    return out;
}

// RMSNorm: no mean subtraction, just rescale by root mean square. The
// pre-norm of most transformers, and cheaper than LayerNorm.
void* flt_rms_norm(void* in, void* gamma, double eps) {
    if (!in) return NULL;
    int64_t C = sh(in, 1);
    int64_t total = flt_size(in);
    if (C <= 0) return NULL;
    // Same axis as LayerNorm: normalise across C features per sample.
    int64_t plane = C;
    void* out = flt_clone(in);
    if (!out) return NULL;
    const double* I = dat(in);
    double* O = dat(out);
    const double* G = gamma ? dat(gamma) : NULL;
    for (int64_t start = 0; start + plane <= total; start += plane) {
        double ss = 0.0;
        for (int64_t p = 0; p < plane; p++) ss += I[start + p] * I[start + p];
        double inv = 1.0 / sqrt(ss / (double)plane + eps);
        for (int64_t p = 0; p < plane; p++) {
            double g = G ? G[p] : 1.0;
            O[start + p] = I[start + p] * inv * g;
        }
    }
    return out;
}

// ------------------------------------------------------------- activations
void* flt_softmax(void* in) {
    if (!in) return NULL;
    int64_t nd = flt_ndim(in);
    if (nd < 1) return NULL;
    int64_t last = sh(in, nd - 1);
    if (last <= 0) return NULL;
    int64_t rows = flt_size(in) / last;
    void* out = flt_clone(in);
    if (!out) return NULL;
    double* O = dat(out);
    for (int64_t r = 0; r < rows; r++) {
        double* row = O + r * last;
        double mx = row[0];
        for (int64_t i = 1; i < last; i++) if (row[i] > mx) mx = row[i];
        double sum = 0.0;
        for (int64_t i = 0; i < last; i++) { row[i] = exp(row[i] - mx); sum += row[i]; }
        // Guard a fully-masked row rather than dividing by zero.
        if (sum <= 0.0) { for (int64_t i = 0; i < last; i++) row[i] = 1.0 / (double)last; continue; }
        for (int64_t i = 0; i < last; i++) row[i] /= sum;
    }
    return out;
}

void* flt_dropout_inplace(void* in, double p) {
    if (!in || p <= 0.0) return in;
    if (p >= 1.0) { flt_fill(in, 0.0); return in; }
    int64_t n = flt_size(in);
    double* d = dat(in);
    double keep = 1.0 / (1.0 - p);
    for (int64_t i = 0; i < n; i++) {
        // flint_rand_f64 is the runtime RNG, so a seeded program is reproducible.
        if (flint_rand_f64() < p) d[i] = 0.0;
        else d[i] *= keep;
    }
    return in;
}

void* flt_dropout(void* in, double p) {
    if (!in) return NULL;
    void* out = flt_clone(in);
    return out ? flt_dropout_inplace(out, p) : NULL;
}

double flt_l2_reg(void* w) {
    if (!w) return 0.0;
    int64_t n = flt_size(w);
    const double* d = dat(w);
    double s = 0.0;
    for (int64_t i = 0; i < n; i++) s += d[i] * d[i];
    return s;
}

// -------------------------------------------------------------- shape help
void* flt_flatten(void* in) {
    if (!in) return NULL;
    int64_t nd = flt_ndim(in);
    if (nd < 2) return NULL;
    int64_t lead = sh(in, 0);
    int64_t rest = flt_size(in) / (lead > 0 ? lead : 1);
    int64_t oshape[2] = { lead, rest };
    return flt_create(2, oshape);
}

// ------------------------------------------------------ element access
void flt_set_at(void* t, int64_t idx, double v) {
    if (!t) { flint_panic("flt_set_at: null tensor"); return; }
    int64_t n = flt_size(t);
    if (idx < 0 || idx >= n) {
        char msg[96];
        snprintf(msg, sizeof msg, "flt_set_at: index %lld out of bounds for length %lld",
                 (long long)idx, (long long)n);
        flint_panic(msg);
        return;
    }
    dat(t)[idx] = v;
}

double flt_get_at(void* t, int64_t idx) {
    if (!t) { flint_panic("flt_get_at: null tensor"); return 0.0; }
    int64_t n = flt_size(t);
    if (idx < 0 || idx >= n) {
        char msg[96];
        snprintf(msg, sizeof msg, "flt_get_at: index %lld out of bounds for length %lld",
                 (long long)idx, (long long)n);
        flint_panic(msg);
        return 0.0;
    }
    return dat(t)[idx];
}

// --------------------------------------------------------------- init
// Uniform in [-a, a] from the runtime RNG (flint_rand_f64 in [0,1)).
static void fill_uniform(void* t, double a) {
    int64_t n = flt_size(t);
    double* d = dat(t);
    for (int64_t i = 0; i < n; i++) d[i] = (flint_rand_f64() * 2.0 - 1.0) * a;
}

void* flt_xavier_uniform(void* t, double scale) {
    if (!t) return NULL;
    // Glorot bound from the tensor's own shape; `scale` multiplies it so a
    // caller can shrink or enlarge without recomputing.
    int64_t fan_in = 1, fan_out = 1, nd = flt_ndim(t);
    if (nd >= 1) fan_in = sh(t, 0);
    if (nd >= 2) fan_out = sh(t, 1);
    double denom = (double)(fan_in + fan_out);
    if (denom <= 0.0) denom = 2.0;
    fill_uniform(t, scale * sqrt(6.0 / denom));
    return t;
}

void* flt_he_uniform(void* t, double scale) {
    if (!t) return NULL;
    int64_t fan_in = flt_ndim(t) >= 1 ? sh(t, 0) : 1;
    if (fan_in <= 0) fan_in = 1;
    fill_uniform(t, scale * sqrt(6.0 / (double)fan_in));
    return t;
}