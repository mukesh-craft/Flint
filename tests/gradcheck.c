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

// ---------------------------------------------------------------------------
// Widened coverage (2026-10-17): matmul first per the ledger order, then every
// unary op the tape header exposes (sigmoid/exp/tanh/log/relu/neg) plus mean
// as its own pass (the sum pass reused it only trivially). The relu offset is
// chosen so one operand set straddles the kink (-0.15 -> dead, dead, alive,
// alive): the dead-branch gradient (exactly 0) is as load-bearing as the
// alive one, and the numeric reference sees the same mixed table.
// ----------------------------------------------------------------------------

static double W2[4] = {0.3, -0.2, 0.5, 0.4};
static double X2[4] = {1.0, 2.0, 0.5, -1.0};   // exact in f32

static double loss_mmul(const double* w) {
    double s = 0.0;
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2; j++) {
            double acc = 0.0;
            for (int k = 0; k < 2; k++) acc += w[i*2+k] * X2[k*2+j];
            s += acc;
        }
    return s;
}

static double loss_mmul_f32(const double* w) {
    float wf[4], xf[4];
    for (int i = 0; i < 4; i++) { wf[i] = (float)w[i]; xf[i] = (float)X2[i]; }
    double s = 0.0;
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2; j++) {
            float acc = 0.0f;
            for (int k = 0; k < 2; k++) acc += wf[i*2+k] * xf[k*2+j];
            s += (double)acc;
        }
    return s;
}

static void tape_mmul_grads(int is_f32, double* dw) {
    ag_init();
    void* wt = is_f32 ? flt_create_f32_2d(2, 2) : flt_create_2d(2, 2);
    void* xt = is_f32 ? flt_create_f32_2d(2, 2) : flt_create_2d(2, 2);
    for (int i = 0; i < 4; i++) { flt_set(wt, i, W2[i]); flt_set(xt, i, X2[i]); }
    int64_t wid = ag_track_tensor(wt);
    void* m = ag_matmul(wt, xt);
    void* t = ag_sum(m);
    ag_seed_scalar(ag_grad_id(t), 1.0);
    ag_backward(ag_grad_id(t));
    for (int i = 0; i < 4; i++) dw[i] = ag_grad_at(wid, i);
    ag_cleanup();
    flt_destroy(wt); flt_destroy(xt); flt_destroy(m); flt_destroy(t);
}

// One generic pass: z_i = w_i * XS_i (+ b where noted); a unary chain maps z
// to the loss mean. kind 0 sigmoid(+b), 1 exp, 2 tanh, 3 log(+2.0), 4
// relu(+b=-0.15 kink), 5 neg, 6 plain mean. b is tracked when the pass asks
// for a db gradient (kinds 0 and 6).
static double loss_unary(int kind, const double* w, double b) {
    double s = 0.0;
    for (int i = 0; i < NIN; i++) {
        double a = w[i] * XS[i] + b;
        double v;
        switch (kind) {
            case 0: v = 1.0 / (1.0 + exp(-a)); break;
            case 1: v = exp(a); break;
            case 2: v = tanh(a); break;
            case 3: v = log(a); break;
            case 4: v = a > 0.0 ? a : 0.0; break;
            case 5: v = -a; break;
            default: v = a; break;
        }
        s += v;
    }
    return s / NIN;
}

static double loss_unary_f32(int kind, const double* w, double b) {
    double s = 0.0;
    for (int i = 0; i < NIN; i++) {
        float a = (float)w[i] * (float)XS[i] + (float)b;
        float v;
        switch (kind) {
            case 0: v = 1.0f / (1.0f + expf(-a)); break;
            case 1: v = expf(a); break;
            case 2: v = tanhf(a); break;
            case 3: v = logf(a); break;
            case 4: v = a > 0.0f ? a : 0.0f; break;
            case 5: v = -a; break;
            default: v = a; break;
        }
        s += (double)v;
    }
    return s / NIN;
}

static void tape_unary_grads(int kind, int is_f32, double* dw, double* db) {
    ag_init();
    void* x = is_f32 ? flt_create_f32_1d(NIN) : flt_create_1d(NIN);
    void* w = is_f32 ? flt_create_f32_1d(NIN) : flt_create_1d(NIN);
    for (int i = 0; i < NIN; i++) { flt_set(x, i, XS[i]); flt_set(w, i, 0.3 + 0.1 * i); }
    int64_t wid = ag_track_tensor(w);
    double b = -0.2;
    void* bx = NULL;
    void* mulnode = ag_mul(w, x);
    void* added = NULL;
    void* cur = mulnode;
    switch (kind) {
        case 0:
            bx = is_f32 ? flt_create_f32_1d(1) : flt_create_1d(1);
            flt_set(bx, 0, b);
            ag_track_tensor(bx);
            added = ag_add(cur, bx);
            cur = ag_sigmoid(added);
            break;
        case 1: cur = ag_exp(cur); break;
        case 2: cur = ag_tanh(cur); break;
        case 3:
            bx = is_f32 ? flt_create_f32_1d(1) : flt_create_1d(1);
            flt_set(bx, 0, 2.0);
            ag_track_tensor(bx);
            added = ag_add(cur, bx);
            cur = ag_log(added);
            break;
        case 4:
            bx = is_f32 ? flt_create_f32_1d(1) : flt_create_1d(1);
            flt_set(bx, 0, -0.15);
            ag_track_tensor(bx);
            added = ag_add(cur, bx);
            cur = ag_relu(added);
            break;
        case 5: cur = ag_neg(cur); break;
        default:
            bx = is_f32 ? flt_create_f32_1d(1) : flt_create_1d(1);
            flt_set(bx, 0, b);
            ag_track_tensor(bx);
            added = ag_add(cur, bx);
            cur = added;
            break;
    }
    void* tot = ag_mean(cur);
    ag_seed_scalar(ag_grad_id(tot), 1.0);
    ag_backward(ag_grad_id(tot));
    for (int i = 0; i < NIN; i++) dw[i] = ag_grad_at(wid, i);
    if (db) db[0] = ag_grad_at(ag_grad_id(bx), 0);
    ag_cleanup();
    // Each handle corresponds to exactly one node: mulnode (w*x), added (a+b,
    // for the chains that have one), cur (the unary or, in the default arm,
    // the same node as added), tot (mean). Destroy each distinct handle once.
    flt_destroy(x); flt_destroy(w); flt_destroy(mulnode); flt_destroy(tot);
    if (added && added != cur) flt_destroy(added);
    if (cur && cur != mulnode) flt_destroy(cur);
    if (bx) flt_destroy(bx);
}

static int run_mmul(const char* name, int is_f32, double h, double tol) {
    printf("--- mmul %s ---\n", name);
    double dw[4];
    tape_mmul_grads(is_f32, dw);
    for (int i = 0; i < 4; i++) {
        double wp[4], wm[4];
        memcpy(wp, W2, sizeof W2); memcpy(wm, W2, sizeof W2);
        wp[i] += h; wm[i] -= h;
        double lp = is_f32 ? loss_mmul_f32(wp) : loss_mmul(wp);
        double lm = is_f32 ? loss_mmul_f32(wm) : loss_mmul(wm);
        char lbl[24];
        snprintf(lbl, sizeof lbl, "%s dw[%d]", name, i);
        compare(lbl, dw[i], (lp - lm) / (2 * h), tol);
    }
    return failures;
}

static int run_unary(int kind, const char* name, int is_f32, double h, double tol, double b, int want_db) {
    printf("--- %s ---\n", name);
    double w[NIN], dw[NIN], dbv[1];
    for (int i = 0; i < NIN; i++) w[i] = 0.3 + 0.1 * i;
    tape_unary_grads(kind, is_f32, dw, want_db ? dbv : NULL);
    for (int i = 0; i < NIN; i++) {
        double wp[NIN], wm[NIN];
        memcpy(wp, w, sizeof w); memcpy(wm, w, sizeof w);
        wp[i] += h; wm[i] -= h;
        double lp = is_f32 ? loss_unary_f32(kind, wp, b) : loss_unary(kind, wp, b);
        double lm = is_f32 ? loss_unary_f32(kind, wm, b) : loss_unary(kind, wm, b);
        char lbl[32];
        snprintf(lbl, sizeof lbl, "%s dw[%d]", name, i);
        compare(lbl, dw[i], (lp - lm) / (2 * h), tol);
    }
    if (want_db) {
        double lp = is_f32 ? loss_unary_f32(kind, w, b + h) : loss_unary(kind, w, b + h);
        double lm = is_f32 ? loss_unary_f32(kind, w, b - h) : loss_unary(kind, w, b - h);
        char lbl[32];
        snprintf(lbl, sizeof lbl, "%s db", name);
        compare(lbl, dbv[0], (lp - lm) / (2 * h), tol);
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

    // Widened rules. matmul first (ledger order); f64 tight, f32 loose.
    run_mmul("f64mm", 0, 1e-6, 1e-5);
    run_mmul("f32mm", 1, 1e-2, 2e-3);

    // Unaries + mean over f64 at the same tight settings.
    static const struct { int kind; const char* nm; double b; int want_db; } P[] = {
        {0, "f64 sigmoid", -0.2, 1}, {1, "f64 exp", 0.0, 0}, {2, "f64 tanh", 0.0, 0},
        {3, "f64 log", 2.0, 0}, {4, "f64 relu", -0.15, 0}, {5, "f64 neg", 0.0, 0},
        {6, "f64 mean", -0.2, 1},
    };
    for (size_t p = 0; p < sizeof P / sizeof P[0]; p++)
        run_unary(P[p].kind, P[p].nm, 0, 1e-6, 1e-5, P[p].b, P[p].want_db);

    // f32 composite through the SAME kind-0 chain (sigmoid of w*x+b, mean) --
    // verifies the f32 narrowing happens at the same rounding points the
    // numeric reference modeled above.
    run_unary(0, "f32 sigmoid", 1, 1e-2, 2e-3, -0.2, 1);

    printf("gradcheck: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}