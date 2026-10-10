// Gradient checker: compare the autograd tape against central finite differences,
// for BOTH element types.
//
//   analytic = d loss / d w                    (what ag_backward computes)
//   numeric  = (loss(w+h) - loss(w-h)) / 2h
//
// This is the standard verification (DeepLearning.AI's gradient-checking
// notebook, Cranmer's autodiff tutorial, demofox's dual-numbers post). An
// analytical gradient can pass every shape and type check and still be the
// wrong number; only a numerical comparison catches that.
//
// Why f32 needs its OWN check rather than reusing the f64 one: f32 has ~7
// decimal digits, so the analytic gradient it returns differs from the exact
// derivative by rounding. Comparing against an f64 finite difference at a 1e-5
// tolerance would fail on arithmetic that is correct to the last bit f32 can
// represent. So the f32 pass uses a tolerance sized to the format (~2e-3
// relative) and a larger perturbation step, since h must stay well above the
// f32 noise floor or the difference is dominated by rounding rather than by
// the derivative.
//
// The loss is rebuilt from scratch for each perturbation. Reusing one tape
// across forward passes double-counts, which is the usual reason a check like
// this "doesn't work".

#include "flint_tensor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int failures = 0;
static int checks = 0;

// Loss for a 1-D affine map: L = sum((w*x + b - y)^2) / n.
// Small enough that every rule in the tape participates in one forward pass:
//   x, y are fixed; w and b are the parameters under test.
#define NIN 4
static const double XS[NIN] = {0.0, 0.0, 1.0, 1.0};
static const double YS[NIN] = {0.0, 1.0, 1.0, 0.0};   // XOR targets

// Evaluate the loss in f32 by rounding every input to float first, so the
// reference has the same rounding the tape's forward pass had. Comparing an f32
// gradient against an f64 reference would measure the format change instead of
// the gradient.
static double loss_f32(const float* w, float b) {
    double s = 0.0;
    for (int i = 0; i < NIN; i++) {
        float p = (float)(w[i] * (float)XS[i] + b);
        float d = (float)(p - (float)YS[i]);
        s += (double)(d * d);
    }
    return s / NIN;
}

static double loss_arr(const double* w, double b) {
    double s = 0.0;
    for (int i = 0; i < NIN; i++) {
        double p = w[i] * XS[i] + b;
        double d = p - YS[i];
        s += d * d;
    }
    return s / NIN;
}

// The same loss through the tape, with every tensor in the requested element
// type. Reads back d/dw and d/db.
static void tape_grads(int is_f32, double* dw, double* db) {
    ag_init();

    void* x = is_f32 ? flt_create_f32_1d(NIN) : flt_create_1d(NIN);
    for (int i = 0; i < NIN; i++) flt_set(x, i, XS[i]);
    void* y = is_f32 ? flt_create_f32_1d(NIN) : flt_create_1d(NIN);
    for (int i = 0; i < NIN; i++) flt_set(y, i, YS[i]);

    void* w = is_f32 ? flt_create_f32_1d(NIN) : flt_create_1d(NIN);
    for (int i = 0; i < NIN; i++) flt_set(w, i, 0.3 + 0.1 * i);
    void* b = is_f32 ? flt_create_f32_1d(1) : flt_create_1d(1);
    flt_set(b, 0, -0.2);

    int64_t wid = ag_track_tensor(w);
    int64_t bid = ag_track_tensor(b);

    void* wx = ag_mul(w, x);
    void* p  = ag_add(wx, b);
    void* d  = ag_sub(p, y);
    void* sq = ag_mul(d, d);
    void* tot = ag_sum(sq);

    // d/d(sum) = 1, so the loss is sum/N.
    ag_seed_scalar(ag_grad_id(tot), 1.0 / NIN);
    ag_backward(ag_grad_id(tot));

    for (int i = 0; i < NIN; i++) dw[i] = ag_grad_at(wid, i);
    db[0] = ag_grad_at(bid, 0);

    ag_cleanup();
    flt_destroy(w); flt_destroy(b); flt_destroy(x); flt_destroy(y);
    flt_destroy(wx); flt_destroy(p); flt_destroy(d);
    flt_destroy(sq); flt_destroy(tot);
}

static void compare(const char* label, double analytic, double numeric, double tol) {
    double denom = fabs(analytic) > fabs(numeric) ? fabs(analytic) : fabs(numeric);
    if (denom < 1e-9) denom = 1.0;
    double rel = fabs(analytic - numeric) / denom;
    checks++;
    if (rel > tol) {
        printf("FAIL %-12s analytic %14.8f  numeric %14.8f  rel %.2e > %.0e\n",
               label, analytic, numeric, rel, tol);
        failures++;
    } else {
        printf("PASS %-12s analytic %14.8f  numeric %14.8f  rel %.2e\n",
               label, analytic, numeric, rel);
    }
}

// One full pass. `tol` and `h` differ per element type: f32 needs a looser
// tolerance and a bigger step, for the reasons in the header comment.
static int run_pass(const char* name, int is_f32, double h, double tol) {
    printf("--- %s ---\n", name);
    double w[NIN], b = -0.2;
    double dw[NIN], db[1];

    for (int i = 0; i < NIN; i++) w[i] = 0.3 + 0.1 * i;

    tape_grads(is_f32, dw, db);

    for (int i = 0; i < NIN; i++) {
        char lbl[24];
        snprintf(lbl, sizeof lbl, "%s d/dw[%d]", name, i);
        double wp[NIN], wm[NIN];
        memcpy(wp, w, sizeof w); memcpy(wm, w, sizeof w);
        wp[i] += h; wm[i] -= h;
        // The reference loss must be evaluated in the SAME precision as the
        // forward pass, or the comparison measures the format change rather
        // than the gradient.
        double lp, lm;
        if (is_f32) {
            float a1[NIN], a2[NIN];
            for (int k = 0; k < NIN; k++) { a1[k] = (float)wp[k]; a2[k] = (float)wm[k]; }
            lp = loss_f32(a1, (float)b);      // b held FIXED: only w[i] moves
            lm = loss_f32(a2, (float)b);
        } else {
            lp = loss_arr(wp, b);
            lm = loss_arr(wm, b);
        }
        compare(lbl, dw[i], (lp - lm) / (2 * h), tol);
    }
    {
        char lbl[24];
        snprintf(lbl, sizeof lbl, "%s d/db", name);
        double lp, lm;
        if (is_f32) {
            float a1[NIN], a2[NIN];
            for (int k = 0; k < NIN; k++) { a1[k] = (float)w[k]; a2[k] = (float)w[k]; }
            lp = loss_f32(a1, (float)(b + h));
            lm = loss_f32(a2, (float)(b - h));
        } else {
            lp = loss_arr(w, b + h);
            lm = loss_arr(w, b - h);
        }
        compare(lbl, db[0], (lp - lm) / (2 * h), tol);
    }
    return failures;
}

int main(void) {
    // f64: h well inside double precision, tight tolerance.
    run_pass("f64", 0, 1e-6, 1e-5);

    // f32: h must clear the single-precision noise floor (1e-7 relative) by
    // a wide margin or the difference is pure rounding, and the tolerance is
    // sized to what f32 can represent (~1e-7) with room for accumulated
    // error through six ops.
    run_pass("f32", 1, 1e-2, 2e-3);

    printf("gradcheck: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}