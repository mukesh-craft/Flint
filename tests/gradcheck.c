// Gradient checker: compare the autograd tape against central finite differences.
//
//   analytic = d loss / d w                    (what ag_backward computes)
//   numeric  = (loss(w+h) - loss(w-h)) / 2h    (h = 1e-6)
//
// This is the standard verification (DeepLearning.AI's gradient-checking
// notebook, Cranmer's autodiff tutorial, demofox's dual-numbers post). An
// analytical gradient can pass every shape and type check and still be the
// wrong number; only a numerical comparison catches that.
//
// The loss is rebuilt from scratch for each perturbation. Reusing one tape
// across forward passes double-counts, which is the usual reason a check like
// this "doesn't work".

#include "flint_tensor.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static int failures = 0;
static int checks = 0;

// Loss for a 1-D affine map: L = sum((w*x + b - y)^2) / n.
// Small enough that every rule in the tape participates in one forward pass:
//   x, y are fixed; w and b are the parameters under test.
#define NIN 4
static const double XS[NIN] = {0.0, 0.0, 1.0, 1.0};
static const double YS[NIN] = {0.0, 1.0, 1.0, 0.0};   // XOR targets

static double loss_arr(const double* w, double b) {
    double s = 0.0;
    for (int i = 0; i < NIN; i++) {
        double p = w[i] * XS[i] + b;
        double d = p - YS[i];
        s += d * d;
    }
    return s / NIN;
}

// Run the same loss through the tape and read back d/dw and d/db.
static void tape_grads(double* dw, double* db) {
    ag_init();

    void* x = flt_create_1d(NIN);
    for (int i = 0; i < NIN; i++) flt_set(x, i, XS[i]);
    void* y = flt_create_1d(NIN);
    for (int i = 0; i < NIN; i++) flt_set(y, i, YS[i]);

    void* w = flt_create_1d(NIN);
    for (int i = 0; i < NIN; i++) flt_set(w, i, 0.3 + 0.1 * i);
    void* b = flt_create_1d(1);
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

static void compare(const char* label, double analytic, double numeric) {
    double denom = fabs(analytic) > fabs(numeric) ? fabs(analytic) : fabs(numeric);
    if (denom < 1e-9) denom = 1.0;
    double rel = fabs(analytic - numeric) / denom;
    checks++;
    if (rel > 1e-5) {
        printf("FAIL %-8s analytic %14.8f  numeric %14.8f  rel %.2e\n",
               label, analytic, numeric, rel);
        failures++;
    } else {
        printf("PASS %-8s analytic %14.8f  numeric %14.8f\n", label, analytic, numeric);
    }
}

int main(void) {
    double w[NIN], b = -0.2;
    double dw[NIN], db[1];

    for (int i = 0; i < NIN; i++) w[i] = 0.3 + 0.1 * i;

    tape_grads(dw, db);

    const double h = 1e-6;
    for (int i = 0; i < NIN; i++) {
        char lbl[16];
        snprintf(lbl, sizeof lbl, "d/dw[%d]", i);
        w[i] = 0.3 + 0.1 * i;
        double wp[NIN], wm[NIN];
        memcpy(wp, w, sizeof w); memcpy(wm, w, sizeof w);
        wp[i] += h; wm[i] -= h;
        compare(lbl, dw[i], (loss_arr(wp, b) - loss_arr(wm, b)) / (2 * h));
    }
    {
        compare("d/db", db[0], (loss_arr(w, b + h) - loss_arr(w, b - h)) / (2 * h));
    }

    printf("gradcheck: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}