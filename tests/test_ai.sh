#!/usr/bin/env bash
# Standard library + AI gate.
#
# The tensor/autograd/optimizer runtime is 2,900 lines of C that nothing
# exercised before -- there was no gate, so a change there shipped completely
# unverified. The gate has since grown to cover the maths, statistics,
# collection and formatting libraries for the same reason.
#
# This checks, in order:
#
#   1. blas_check: the live matmul path agrees with the portable reference on
#      non-square shapes (catches a wrong leading dimension, which returns
#      plausible garbage rather than failing)
#   2. tensor: flt_matmul from Flint produces the exact expected values
#   3. autograd: ag_* is reachable from Flint and a gradient comes back
#   4. optimizers: Adam and SGD are callable and step without crashing
#
# Both BLAS and portable paths are checked when OpenBLAS is installed, since
# they are different code.
# Usage: bash tests/test_ai.sh [path/to/flintc]   (default: ./flintc)
set -u
FLINTC="${1:-./flintc}"
_TMPBASE="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
TMPD="$_TMPBASE/slipstream/ai"
mkdir -p "$TMPD"
pass=0; fail=0

CC_BIN="${CC:-clang}"
INC="-I$PREFIX/include -Iruntime"
LIB="-L$PREFIX/lib"

# ---- 1. BLAS correctness (C level: needs the objects, not the compiler) ----
if [ -f flint_blas.o ]; then
  for mode in openblas portable; do
    if [ "$mode" = openblas ]; then
      $CC_BIN -O2 tests/blas_check.c $INC $LIB flint_blas.o -lopenblas -lm \
        -o "$TMPD/blas_$mode" 2>/dev/null || { echo "SKIP blas($mode): compile failed"; continue; }
    else
      $CC_BIN -O2 tests/blas_check.c $INC $LIB flint_blas.o -lm \
        -o "$TMPD/blas_$mode" 2>/dev/null || { echo "SKIP blas($mode): compile failed"; continue; }
    fi
    if "$TMPD/blas_$mode" > "$TMPD/blas_$mode.out" 2>&1; then
      echo "PASS blas/$mode"
      pass=$((pass+1))
    else
      echo "FAIL blas/$mode"
      command grep -E "^FAIL" "$TMPD/blas_$mode.out" | head -4
      fail=$((fail+1))
    fi
  done
else
  echo "SKIP blas: flint_blas.o not built"
fi

# ---- 1b. Gradient check: analytic vs central finite differences ----
# The standard verification for an autograd tape. Every rule can pass a shape
# and type check and still be numerically wrong; only a finite-difference
# comparison catches that. It found a 2x error in mul, a 4x double-count from
# processing a repeated input twice, and a seed that was silently discarded.
if [ -f flint_tensor.o ]; then
  if $CC_BIN -O2 tests/gradcheck.c $INC $LIB flint_tensor.o flint_blas.o \
       ${FLINT_BLAS_EXTRA:-} -lm -o "$TMPD/gradcheck" 2>/dev/null; then
    if "$TMPD/gradcheck" > "$TMPD/gradcheck.out" 2>&1; then
      echo "PASS gradcheck"
      pass=$((pass+1))
    else
      echo "FAIL gradcheck"
      command grep -E "^FAIL" "$TMPD/gradcheck.out" | head -6
      fail=$((fail+1))
    fi
  else
    echo "SKIP gradcheck: compile failed"
  fi
else
  echo "SKIP gradcheck: flint_tensor.o not built"
fi

# ---- 2-4. Flint-level AI tests ----
# run <name> <expected-space-separated> <source-file>
run() {
  name="$1"; want="$2"; src="$3"
  if [ ! -f "$src" ]; then echo "SKIP $name: $src missing"; return; fi
  if ! "$FLINTC" "$src" --no-run -o "$TMPD/$name.bin" > "$TMPD/$name.err" 2>&1; then
    echo "FAIL $name (build): $(head -n1 "$TMPD/$name.err")"
    fail=$((fail+1)); return
  fi
  got="$("$TMPD/$name.bin" 2>&1 | tr '\n' ' ' | tr -s ' ' | sed 's/ *$//')"
  if [ "$got" = "$want" ]; then
    echo "PASS $name"
    pass=$((pass+1))
  else
    echo "FAIL $name"
    echo "  want: $want"
    echo "  got:  $got"
    fail=$((fail+1))
  fi
}

# 2x3 @ 3x2 = [[58,64],[139,154]], sum 415, blas available=1, then Phase-2
# tensor literals: rank-1 (size/get), rank-2 (size/shape/sum), int
# promotion, negation, and literal-vs-handbuilt agreement (sum 10).
run tensor "4 58 64 139 154 58 64 139 154 415 1 2 1 2 4 2 2 10 1 1 10" tests/t_ai_tensor.fl

# y = x*x on [1,2,3] -> [1,4,9]; grad len 3, pass-through grad [2,2,2]
run autograd "1 4 9 3 2 4 6" tests/t_ai_autograd.fl

# Optimizers: construct, register a param, step, destroy.
run optimizers "loss-before loss-after adam-sgd-ok" tests/t_ai_optim.fl

# Gradient correctness against analytically-known derivatives. The tape used to
# return the seed (1) for every rule, so these all read 1.0.
run gradients "6 0 1 0.25 2.71828 0.25 1 12 1" tests/t_ai_gradients.fl

# A 2-layer network trained on f(x) = 2x + 1. After 500 epochs it must predict
# 3, 5, 7, 9 -- an exact fit, so the run is verifiable by hand rather than
# "the loss went down". The first two lines are the pre-training loss (8.0656)
# and prediction (0.16) at x=1.
run nn "8.0656 0.16 predictions 3 5 7 9" tests/nn_two_layer.fl

# XOR: the standard check that a network is more than a perceptron. No linear
# model separates it, so solving it exercises the hidden layer, the
# nonlinearity and the backward pass together. Loss must collapse from 0.148 to
# ~0 and every output must be on the correct side of 0.5.
run xor "initial-loss 0.14804 final-loss 1.04316e-25 outputs 2.35756e-13 1 1 3.22242e-13" \
    tests/xor_mlp.fl

# ---- standard library: maths, statistics, collections, formatting ----
# These were the largest gaps in the library: no format function at all, no
# set/priority-queue/deque, and a maths module missing everything except the
# transcendentals.
run stdlib \
  "3628800 6 12 7 10 832040 180 3 5 0.842701 24 0 1 2 2 1 4 1.66667 1.29099 3.7 5 2.13809 2 1 1 1 3 0 3 0 3" \
  tests/t_stdlib.fl

run format "n=42 x=3.50 sum=99 name=flint n=7" tests/t_format.fl

# The NN layers, against hand-computed values: identity-kernel conv2d over
# 1..9, 2x2 avg/max pooling, global pooling, softmax, layer norm, flatten and
# Glorot init. Convolution could not be expressed at all before these existed.
run nn_layers "2 2 6 8 12 14 3.5 5.5 11.5 13.5 6 8 14 16 8.5 0.333333 -1.34164 9 1 1 1" \
    tests/t_nn_layers.fl

# The utility layer: sorting (including the pathological inputs), binary
# search, atomics, mutex/rwlock/condvar, the CSPRNG, and CLI parsing. C level,
# because these are runtime primitives rather than codegen behaviour.
if [ -f flint_util.o ]; then
  if $CC_BIN -O2 tests/util_check.c $INC $LIB flint_util.o runtime.o flint_aegis.o \
       -lm -o "$TMPD/util_check" 2>/dev/null; then
    if "$TMPD/util_check" > "$TMPD/util_check.out" 2>&1; then
      echo "PASS util_check"
      pass=$((pass+1))
    else
      echo "FAIL util_check"
      command grep -E "^FAIL" "$TMPD/util_check.out" | head -6
      fail=$((fail+1))
    fi
  else
    echo "SKIP util_check: compile failed"
  fi
else
  echo "SKIP util_check: flint_util.o not built"
fi

# f32 tensors: half the memory and ~2x the matmul speed. Values are small
# integers so every product is exact in single precision and the expected
# output is checkable by hand rather than against a tolerance.
run f32 "1 24 1 2 2 58 64 139 154 58 154 1 2 12 1 0.731059 21 3.5 0 6 1 6 0 4000000 8000000" \
    tests/t_f32.fl

# Phase-2 negative cases: malformed shapes and unsupported dtypes must fail
# loudly (nonzero exit + message), never silently miscompile. Sources live
# in $TMPD (not the repo) so the parse-gate/fixpoint corpus stays green.
run_fail() { # $1=name $2=want-substring $3=source-file
  name="$1"; want="$2"; src="$3"
  if "$FLINTC" "$src" --no-run -o "$TMPD/$name.bin" > "$TMPD/$name.err" 2>&1; then
    echo "FAIL $name (built, must reject)"
    fail=$((fail+1)); return
  fi
  if command grep -qF -- "$want" "$TMPD/$name.err"; then
    echo "PASS $name"
    pass=$((pass+1))
  else
    echo "FAIL $name (missing '$want')"
    command head -n 3 "$TMPD/$name.err"
    fail=$((fail+1))
  fi
}
cat > "$TMPD/reject_ragged.fl" <<'EOF'
fn main() -> i64 {
    t = [[1.0], [2.0, 3.0]]
    0
}
EOF
run_fail reject_ragged "ragged tensor literal" "$TMPD/reject_ragged.fl"
cat > "$TMPD/reject_rank3.fl" <<'EOF'
fn main() -> i64 {
    t = [[[1.0]]]
    0
}
EOF
run_fail reject_rank3 "rank 1 and 2" "$TMPD/reject_rank3.fl"
cat > "$TMPD/reject_suffix.fl" <<'EOF'
fn main() -> i64 {
    t = [1.0f32, 2.0]
    0
}
EOF
run_fail reject_suffix "not yet supported" "$TMPD/reject_suffix.fl"
cat > "$TMPD/reject_cast_str.fl" <<'EOF'
fn main() -> i64 {
    x = 1 as str
    0
}
EOF
run_fail reject_cast_str "only 'i64' and 'f64'" "$TMPD/reject_cast_str.fl"
cat > "$TMPD/reject_cast_f32.fl" <<'EOF'
fn main() -> i64 {
    x = 1 as f32
    0
}
EOF
run_fail reject_cast_f32 "not yet supported" "$TMPD/reject_cast_f32.fl"
cat > "$TMPD/reject_cast_val.fl" <<'EOF'
fn main() -> i64 {
    s = "hi"
    y = s as i64
    0
}
EOF
run_fail reject_cast_val "needs an i64 or f64 value" "$TMPD/reject_cast_val.fl"

# The TOML parser, against every construct a config file uses plus five error
# cases. C level: a parse bug should be reported as a parse failure, not as a
# confusing value-model result.
if [ -f flint_toml.o ]; then
  if $CC_BIN -O1 tests/toml_check.c $INC $LIB flint_value.o flint_toml.o \
       -lm -o "$TMPD/toml_check" 2>/dev/null; then
    if "$TMPD/toml_check" > "$TMPD/toml_check.out" 2>&1; then
      echo "PASS toml_check"
      pass=$((pass+1))
    else
      echo "FAIL toml_check"
      command grep -E "^FAIL" "$TMPD/toml_check.out" | head -6
      fail=$((fail+1))
    fi
  else
    echo "SKIP toml_check: compile failed"
  fi
else
  echo "SKIP toml_check: flint_toml.o not built"
fi

# The YAML parser: block mappings and sequences, the same-indent rule, flow
# collections, block scalars, quoting, typing, and the error cases.
if [ -f flint_yaml.o ]; then
  if $CC_BIN -O1 tests/yaml_check.c $INC $LIB flint_value.o flint_yaml.o \
       -lm -o "$TMPD/yaml_check" 2>/dev/null; then
    if "$TMPD/yaml_check" > "$TMPD/yaml_check.out" 2>&1; then
      echo "PASS yaml_check"
      pass=$((pass+1))
    else
      echo "FAIL yaml_check"
      command grep -E "^FAIL" "$TMPD/yaml_check.out" | head -6
      fail=$((fail+1))
    fi
  else
    echo "SKIP yaml_check: compile failed"
  fi
else
  echo "SKIP yaml_check: flint_yaml.o not built"
fi

# The civil-time/timezone library: verified against the libc oracle (POSIX
# TZ rules as an independent implementation of the same IANA rules) over
# weekly sweeps plus exact transition edges. C level: a wrong UTC offset
# returns plausible garbage rather than failing.
if [ -f flint_time.o ]; then
  if $CC_BIN -O1 tests/time_check.c $INC $LIB flint_time.o \
       -lm -o "$TMPD/time_check" 2>/dev/null; then
    if "$TMPD/time_check" > "$TMPD/time_check.out" 2>&1; then
      echo "PASS time_check"
      pass=$((pass+1))
    else
      echo "FAIL time_check"
      command grep -E "^FAIL" "$TMPD/time_check.out" | head -6
      fail=$((fail+1))
    fi
  else
    echo "SKIP time_check: compile failed"
  fi
else
  echo "SKIP time_check: flint_time.o not built"
fi

# The time library reached from Flint: calendar, DST edges, %Z abbrevs,
# zone-aware parsing, durations, zone abbrev/DST accessors.
run time "1 0 0 1 29 28 0 0 19800 32400 0 -18000 -14400 39600 36000 -25200 -25200 9 1 2026-10-07T09:30:15Z Wednesday October 07 2026 0 1 1 1 1 1d01:01:01 -0d01:01:01 60 -18000 -14400 -14400 -18000 39600 36000 -21600 EDT 1 1 EST 0 EDT 1 UTC 0" \
    tests/t_time.fl

# The utility layer reached from Flint, not just from C: sorting, binary
# search, atomics, a mutex, an rwlock, the CSPRNG and CLI parsing.
run util "1 9 2 -1 15 12 1 99 1 rwlock-ok 1 1 rng-ok 1 0 cli-ok" \
    tests/t_util.fl

# Weight checkpoints: FLWT v1 save/append/count/load round-trip, exact f64
# values and f32 dtype chosen live, and out-of-range reads fail the null way.
run chkpt "1 1 1 1 1 1 1 1 1 1" \
    tests/t_chkpt.fl

echo "stdlib-ai: $pass passed, $fail failed"
[ "$fail" -eq 0 ]