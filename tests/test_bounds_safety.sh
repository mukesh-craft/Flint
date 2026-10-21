#!/usr/bin/env bash
# Gate: the known-length fact must not weaken the bounds check.
#
# The compiler knows a literal array's exact length, so it can prove that a
# literal index is in range and drop the check. The DANGEROUS direction is
# the opposite: an index that is NOT proven must still be checked, and an
# out-of-range access must still trap.
#
# This is a runtime assertion: the program is expected to ABORT.
#
# Usage: bash tests/test_bounds_safety.sh [path/to/flintc]  (default: ./flintc)
set -u
cd "$(dirname "$0")/.."
FLINTC="${1:-./flintc}"
T="${TMPDIR:-/data/data/com.termux/files/usr/tmp}/bounds_safety"
mkdir -p "$T"
pass=0; fail=0

# 1. An out-of-range LITERAL index must trap (the proof must not fire).
if ! "$FLINTC" tests_manual/t_bounds_oob.fl -o "$T/oob.bin" >/dev/null 2>&1; then
    echo "FAIL t_bounds_oob.fl did not compile"; fail=$((fail+1))
else
    out=$("$T/oob.bin" 2>&1); rc=$?
    if [ "$rc" -eq 0 ]; then
        echo "FAIL t_bounds_oob.fl: the out-of-range access did NOT trap"
        fail=$((fail+1))
    elif printf '%s\n' "$out" | grep -qi "out of bounds"; then
        pass=$((pass+1))
    else
        echo "FAIL t_bounds_oob.fl: aborted, but not with a bounds diagnostic"
        printf '%s\n' "$out" | head -3
        fail=$((fail+1))
    fi
fi

# 2. In UNSAFE mode the check is gone on purpose -- the build is stamped
#    as unsafe, so the program must NOT be expected to trap. This checks
#    that the escape hatch is real and that the safe build is not.
if "$FLINTC" --unsafe tests_manual/t_bounds_oob.fl -o "$T/oob_unsafe.bin" >/dev/null 2>&1; then
    if strings "$T/oob_unsafe.bin" 2>/dev/null | grep -q "flint:safety=unsafe"; then
        pass=$((pass+1))
    else
        echo "FAIL the unsafe build is not stamped unsafe"
        fail=$((fail+1))
    fi
else
    echo "FAIL the unsafe build did not compile"
    fail=$((fail+1))
fi

# 3. The loop-range analysis (Phase 2 / 2C) must never excuse a REAL
#    out-of-bounds access: a loop shaped `while i <= limit` proves
#    `i <= limit`, which does NOT make `i < len` true when len == limit.
cat > "$T/range_oob.fl" <<'EOF'
fn main() -> i64 {
    limit = 3
    arr = flint_array_alloc(limit)
    mut i: i64 = 0
    while i <= limit {
        flint_array_set(arr, i, i * 2)
        i = i + 1
    }
    print(flint_array_get(arr, limit))
    return 0
}
EOF
if "$FLINTC" "$T/range_oob.fl" -o "$T/range_oob.bin" >/dev/null 2>&1; then
    out=$("$T/range_oob.bin" 2>&1); rc=$?
    if [ "$rc" -eq 0 ]; then
        echo "FAIL the loop-range analysis removed a real bounds check (no trap)"
        fail=$((fail+1))
    elif printf '%s
' "$out" | grep -qi "out of bounds"; then
        pass=$((pass+1))
    else
        echo "FAIL the program aborted, but not with a bounds diagnostic"
        fail=$((fail+1))
    fi
else
    echo "FAIL the range-oob program did not compile"
    fail=$((fail+1))
fi

# 4. A literal index that IS in range must work (the proof fires, and the
#    program is correct).
cat > "$T/inrange.fl" <<'EOF'
fn main() -> i64 {
    a = [10, 20, 30]
    print(a[1])
    return 0
}
EOF
if "$FLINTC" "$T/inrange.fl" -o "$T/inrange.bin" >/dev/null 2>&1; then
    out=$("$T/inrange.bin" 2>&1)
    if [ "$out" = "20" ]; then pass=$((pass+1)); else
        echo "FAIL in-range literal index printed '$out', expected 20"
        fail=$((fail+1))
    fi
else
    echo "FAIL the in-range program did not compile"
    fail=$((fail+1))
fi

# 5. The safety POLICY must agree with the IR it produced (Phase 2 / 2B).
#    The policy counts the checks it inserted; the IR must contain exactly
#    that many. A mismatch means a check was emitted without the policy
#    knowing -- the failure mode where the safety posture and the emitted
#    code disagree.
cat > "$T/policy.fl" <<'EOF'
fn main() -> i64 {
    a = [1, 2, 3]
    mut s: i64 = 0
    mut i: i64 = 0
    while i < 3 { s = s + a[i]
        i = i + 1 }
    print(s)
    return 0
}
EOF
if "$FLINTC" --check-bce "$T/policy.fl" -o "$T/policy.bin" > "$T/policy.txt" 2>&1 \
   && "$FLINTC" --check-bce "$T/policy.fl" --emit-llvm -o "$T/policy.ll" >/dev/null 2>&1; then
    # The policy's own counts (exact, pre-optimization) against the
    # compiler's pre-optimization IR count ("checks emitted"). The
    # optimized IR is NOT the right comparison: the optimizer legitimately
    # removes checks the policy inserted.
    said=$(awk '/inserted by policy/ {n = 0; for (i = 1; i <= NF; i++) {
                        if ($i == "bounds") n += $(i + 1) + 0;
                        if ($i == "overflow") n += $(i + 1) + 0 }
                    gsub(/[^0-9]/, "", n); print n; exit }' "$T/policy.txt")
    have=$(awk '/checks emitted/ {print $2; exit}' "$T/policy.txt")
    if [ -n "$said" ] && [ -n "$have" ] && [ "$said" = "$have" ]; then
        pass=$((pass+1))
    else
        echo "FAIL the policy inserted $said checks, the IR has $have before optimization"
        fail=$((fail+1))
    fi
else
    echo "FAIL the policy program did not compile"
    fail=$((fail+1))
fi

echo "bounds-safety: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
