#!/usr/bin/env bash
# Gate: the middle-end runs with a TargetMachine (Phase 3 / P1).
#
# LLVM's loop vectorizer asks TargetTransformInfo for the vector-register
# width and the cost model. A PassBuilder built without a TargetMachine
# reports "no vector registers" and silently declines every loop -- the
# same module vectorizes under `opt -O2`, which always has a TM. The bug
# was invisible because the IR was still correct, just scalar.
#
# This gate compiles a check-free loop with `--unsafe` and requires the
# loop vectorizer's own metadata in the optimized IR. It also pins the
# safety side: the default build must still carry the accumulator's
# overflow check (the TTI fix must not turn a checked add into a plain
# one), and the unsafe result must be numerically right.
#
# Usage: bash tests/test_vectorize.sh [path/to/flintc]  (default: ./flintc)
set -u
cd "$(dirname "$0")/.."
FLINTC="${1:-./flintc}"
T="${TMPDIR:-/data/data/com.termux/files/usr/tmp}/vectorize"
mkdir -p "$T"
pass=0; fail=0
ok()  { pass=$((pass+1)); }
bad() { echo "FAIL $*"; fail=$((fail+1)); }

cat > "$T/vec.fl" <<'EOF'
fn main() -> i64 {
    n = 1000
    arr = flint_array_alloc(n)

    mut i: i64 = 0
    while i < n {
        flint_array_set(arr, i, i * 2)
        i = i + 1
    }

    mut sum: i64 = 0
    i = 0
    while i < n {
        sum = sum + flint_array_get(arr, i)
        i = i + 1
    }
    flint_println(flint_i64_to_string(sum))
    flint_array_free(arr)
    0
}
EOF

# 1. A loop with no checks must be vectorized. `llvm.loop.isvectorized`
#    is emitted by the loop vectorizer itself, so it is stronger than
#    looking for a particular vector width (which is target-dependent).
if "$FLINTC" --unsafe "$T/vec.fl" -o "$T/vec.ll" >/dev/null 2>&1; then
    if grep -q 'llvm.loop.isvectorized' "$T/vec.ll"; then
        ok
    else
        bad "check-free loop was not vectorized (no llvm.loop.isvectorized)"
    fi
else
    bad "vectorize probe did not compile"
fi

# 2. The same program built with the default safety policy keeps the
#    accumulator's overflow check. The vectorizer must not be allowed to
#    trade the trap for speed.
if "$FLINTC" "$T/vec.fl" -o "$T/safe.ll" >/dev/null 2>&1; then
    if grep -q 'sadd.with.overflow' "$T/safe.ll"; then
        ok
    else
        bad "safe build lost the accumulator overflow check"
    fi
else
    bad "safe probe did not compile"
fi

# 3. The unsafe binary's value is still exact: sum(2*i, i=0..999) = 999000.
if "$FLINTC" --unsafe "$T/vec.fl" --no-run -o "$T/vec.bin" >/dev/null 2>&1; then
    out=$("$T/vec.bin" 2>&1)
    [ "$out" = "999000" ] && ok || bad "unsafe sum: expected 999000, got '$out'"
else
    bad "unsafe binary did not compile"
fi

echo "vectorize: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
