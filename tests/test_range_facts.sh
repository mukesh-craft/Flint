#!/usr/bin/env bash
# Gate: bounded-range facts (Phase 3 / P1).
#
# A fact proves that every dynamic evaluation of an expression yields an i64
# that fits, so the checked add/sub/mul can become a plain one. That is what
# lets LLVM turn `fib(n-1) + fib(n-2)` back into an accumulator loop; it is
# also the one place where a wrong analysis would silently remove a trap.
#
# The gate checks BOTH directions:
#   * a closed-world program with constant roots gets facts (and the check
#     really is gone from the linked fib);
#   * an unknown root, an overflowing root, a global store, or a name-based
#     dispatch keeps every check, and an actual overflow still panics;
#   * emit-only output (.ll) keeps the check even for a program whose
#     executable would elide it (an artifact can be called from outside).
#
# Usage: bash tests/test_range_facts.sh [path/to/flintc]  (default: ./flintc)
set -u
cd "$(dirname "$0")/.."
FLINTC="${1:-./flintc}"
T="${TMPDIR:-/data/data/com.termux/files/usr/tmp}/range_facts"
mkdir -p "$T"
pass=0; fail=0

ok()  { pass=$((pass+1)); }
bad() { echo "FAIL $*"; fail=$((fail+1)); }

# The third `bce:` line reports what the analysis WOULD prove. It is printed
# with the facts disabled, so it never changes the artifact being measured.
facts() { # $1 = source file
    "$FLINTC" --check-bce "$1" -o "$T/probe.bin" 2>&1 |
        awk '/bounded-range facts prove/ {print $5; exit}'
}

# 1. fib: the two decreasing arguments and the sum are all provable.
cat > "$T/fib.fl" <<'EOF'
fn fib(n: i64) -> i64 {
    if n <= 1 { n } else { fib(n - 1) + fib(n - 2) }
}

fn main() -> i64 {
    flint_println(flint_i64_to_string(fib(30)))
    0
}
EOF
n=$(facts "$T/fib.fl")
[ "$n" = "3" ] && ok || bad "fib: expected 3 facts, got '${n:-none}'"

# 2. A two-operation recursion: `n - 1` and `n + sum_to(n - 1)`.
cat > "$T/sum_to.fl" <<'EOF'
fn sum_to(n: i64) -> i64 {
    if n <= 0 { 0 } else { n + sum_to(n - 1) }
}

fn main() -> i64 {
    flint_println(flint_i64_to_string(sum_to(100)))
    0
}
EOF
n=$(facts "$T/sum_to.fl")
[ "$n" = "2" ] && ok || bad "sum_to: expected 2 facts, got '${n:-none}'"

# 3. A provable constant root: `inc(42)` cannot overflow, the result is 43.
cat > "$T/inc_small.fl" <<'EOF'
fn inc(n: i64) -> i64 { n + 1 }

fn main() -> i64 {
    flint_println(flint_i64_to_string(inc(42)))
    0
}
EOF
n=$(facts "$T/inc_small.fl")
[ "$n" = "1" ] && ok || bad "inc(42): expected 1 fact, got '${n:-none}'"
if "$FLINTC" "$T/inc_small.fl" -o "$T/inc_small.bin" >/dev/null 2>&1; then
    out=$("$T/inc_small.bin" 2>&1)
    [ "$out" = "43" ] && ok || bad "inc(42): expected 43, got '$out'"
else
    bad "inc(42) did not compile"
fi

# 4. A constant root that overflows must NOT get a fact, and must trap.
cat > "$T/inc_overflow.fl" <<'EOF'
fn inc(n: i64) -> i64 { n + 1 }

fn main() -> i64 {
    flint_println(flint_i64_to_string(inc(9223372036854775807)))
    0
}
EOF
n=$(facts "$T/inc_overflow.fl")
[ "$n" = "0" ] && ok || bad "inc(INT64_MAX): expected 0 facts, got '${n:-none}'"
if "$FLINTC" "$T/inc_overflow.fl" -o "$T/inc_overflow.bin" >/dev/null 2>&1; then
    out=$("$T/inc_overflow.bin" 2>&1); rc=$?
    if [ "$rc" -ne 0 ] && printf '%s\n' "$out" | grep -qi "integer overflow"; then
        ok
    else
        bad "inc(INT64_MAX) did not panic (rc=$rc, out='$out')"
    fi
else
    bad "inc(INT64_MAX) did not compile"
fi

# 5. A runtime root: the same function, called with a value the analysis
#    cannot see, must keep its check and trap at runtime.
cat > "$T/inc_dynamic.fl" <<'EOF'
fn inc(n: i64) -> i64 { n + 1 }

fn main() -> i64 {
    # flint_arg_count() includes argv[0], so with no extra arguments this
    # is INT64_MAX -- but only at runtime, never to the analysis.
    base = 9223372036854775807 - flint_arg_count() + 1
    flint_println(flint_i64_to_string(inc(base)))
    0
}
EOF
n=$(facts "$T/inc_dynamic.fl")
[ "$n" = "0" ] && ok || bad "inc(dynamic): expected 0 facts, got '${n:-none}'"
if "$FLINTC" "$T/inc_dynamic.fl" -o "$T/inc_dynamic.bin" >/dev/null 2>&1; then
    out=$("$T/inc_dynamic.bin" 2>&1); rc=$?
    if [ "$rc" -ne 0 ] && printf '%s\n' "$out" | grep -qi "integer overflow"; then
        ok
    else
        bad "inc(dynamic INT64_MAX) did not panic (rc=$rc, out='$out')"
    fi
else
    bad "inc(dynamic) did not compile"
fi

# 6. A global store inside the function: a self-call could mutate it, so no
#    facts.
cat > "$T/global_store.fl" <<'EOF'
mut g: i64 = 0

fn addg(n: i64) -> i64 {
    g = n + 1
    n + 2
}

fn main() -> i64 {
    flint_println(flint_i64_to_string(addg(3)))
    0
}
EOF
n=$(facts "$T/global_store.fl")
[ "$n" = "0" ] && ok || bad "global store: expected 0 facts, got '${n:-none}'"

# 7. A branch assignment must reach the statements after the branch. This
#    was a real bug: the analysis copied the environment at a block
#    boundary, so `if n > 0 { x = MAX }` was ignored, `x + n` was "proved"
#    [1,1], and the executable wrapped instead of trapping.
cat > "$T/branch_assign.fl" <<'EOF'
fn f(n: i64) -> i64 {
    mut x = 0
    if n > 0 { x = 9223372036854775807 }
    x + n
}

fn main() -> i64 {
    flint_println(flint_i64_to_string(f(1)))
    0
}
EOF
n=$(facts "$T/branch_assign.fl")
[ "$n" = "0" ] && ok || bad "branch assign: expected 0 facts, got '${n:-none}'"
if "$FLINTC" "$T/branch_assign.fl" -o "$T/branch_assign.bin" >/dev/null 2>&1; then
    out=$("$T/branch_assign.bin" 2>&1); rc=$?
    if [ "$rc" -ne 0 ] && printf '%s\n' "$out" | grep -qi "integer overflow"; then
        ok
    else
        bad "branch assign overflow did not panic (rc=$rc, out='$out')"
    fi
else
    bad "branch assign did not compile"
fi

# 8. An explicit `return` inside a self-call must not truncate the caller's
#    evaluation. `returned`/`returnVal` are per-body state; leaking them
#    memoized the callee's value as the caller's, so the outer sum below was
#    "proved" to fit and the executable wrapped instead of trapping.
cat > "$T/return_leak.fl" <<'EOF'
fn f(n: i64) -> i64 {
    if n <= 0 { return 0 }
    (f(n - 1) + 1) + 9223372036854775806
}

fn main() -> i64 {
    flint_println(flint_i64_to_string(f(2)))
    0
}
EOF
n=$(facts "$T/return_leak.fl")
# Only the `n - 1` argument subtraction is provable; both sums overflow for
# n = 2 (the buggy analysis also "proved" them, so this count is 3).
[ "$n" = "1" ] && ok || bad "return leak: expected 1 fact, got '${n:-none}'"
if "$FLINTC" "$T/return_leak.fl" -o "$T/return_leak.bin" >/dev/null 2>&1; then
    out=$("$T/return_leak.bin" 2>&1); rc=$?
    if [ "$rc" -ne 0 ] && printf '%s\n' "$out" | grep -qi "integer overflow"; then
        ok
    else
        bad "return leak overflow did not panic (rc=$rc, out='$out')"
    fi
else
    bad "return leak did not compile"
fi

# 9. A call hidden in a map literal is still a call site. Missing it would
#    leave `f(MAX)` unanalyzed while `f(1)` supplied a narrow fact, so the
#    check would be removed from the overflowing call too.
cat > "$T/map_root.fl" <<'EOF'
fn f(n: i64) -> i64 { n + 1 }

fn main() -> i64 {
    m = map {"a": f(9223372036854775807)}
    flint_println(flint_i64_to_string(f(1)))
    0
}
EOF
n=$(facts "$T/map_root.fl")
[ "$n" = "0" ] && ok || bad "map-literal root: expected 0 facts, got '${n:-none}'"
if "$FLINTC" "$T/map_root.fl" -o "$T/map_root.bin" >/dev/null 2>&1; then
    out=$("$T/map_root.bin" 2>&1); rc=$?
    if [ "$rc" -ne 0 ] && printf '%s\n' "$out" | grep -qi "integer overflow"; then
        ok
    else
        bad "map-literal overflow did not panic (rc=$rc, out='$out')"
    fi
else
    bad "map-literal root did not compile"
fi

# 10. A string literal naming the function: `flint_thread_spawn("addg", ...)`
#     is a name-based dispatcher, so a literal use is refused.
cat > "$T/named.fl" <<'EOF'
fn addg(n: i64) -> i64 { n + 2 }

fn main() -> i64 {
    flint_println("addg")
    flint_println(flint_i64_to_string(addg(3)))
    0
}
EOF
n=$(facts "$T/named.fl")
[ "$n" = "0" ] && ok || bad "name-based dispatch: expected 0 facts, got '${n:-none}'"

# 11. Emit-only output keeps the check, even for a program whose executable
#     would elide it. An artifact can be linked and called from outside the
#     analyzed call sites.
if "$FLINTC" "$T/fib.fl" -o "$T/fib.ll" >/dev/null 2>&1; then
    calls=$(grep -c 'call.*@llvm\.sadd\.with\.overflow' "$T/fib.ll")
    [ "$calls" = "1" ] && ok || bad "emit .ll: expected 1 checked add, got $calls"
else
    bad "emit .ll failed"
fi

# 12. The linked executable really has no overflow branch left in fib, and
#     the value is still right. (llvm-objdump ships with the LLVM the
#     compiler already requires; skip the disassembly check if absent.)
if "$FLINTC" "$T/fib.fl" -o "$T/fib.bin" >/dev/null 2>&1; then
    out=$("$T/fib.bin" 2>&1)
    [ "$out" = "832040" ] && ok || bad "fib(30): expected 832040, got '$out'"
    if command -v llvm-objdump >/dev/null 2>&1; then
        if llvm-objdump -d --disassemble-symbols=fib "$T/fib.bin" 2>/dev/null |
               sed -n '/<fib>:/,/^$/p' | grep -q 'flint_panic'; then
            bad "linked fib still contains a panic branch"
        else
            ok
        fi
    fi
else
    bad "fib executable did not compile"
fi

echo "range-facts: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
