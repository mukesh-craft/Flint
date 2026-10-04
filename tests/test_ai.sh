#!/usr/bin/env bash
# AI gate: the tensor/autograd/optimizer runtime is 2,900 lines of C that
# nothing exercised before -- there was no AI gate, so a change there shipped
# completely unverified. This checks, in order:
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

# ---- 2-4. Flint-level AI tests ----
# run <name> <expected-space-separated> <source-file>
run() {
  name="$1"; want="$2"; src="$3"
  if [ ! -f "$src" ]; then echo "SKIP $name: $src missing"; return; fi
  if ! "$FLINTC" "$src" -o "$TMPD/$name.bin" > "$TMPD/$name.err" 2>&1; then
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

# 2x3 @ 3x2 = [[58,64],[139,154]], sum 415, blas available=1
run tensor "4 58 64 139 154 58 64 139 154 415 1" tests/t_ai_tensor.fl

# y = x*x on [1,2,3] -> [1,4,9]; grad len 3, pass-through grad [2,2,2]
run autograd "1 4 9 3 2 2 2" tests/t_ai_autograd.fl

# Optimizers: construct, register a param, step, destroy.
run optimizers "loss-before loss-after adam-sgd-ok" tests/t_ai_optim.fl

echo "ai: $pass passed, $fail failed"
[ "$fail" -eq 0 ]