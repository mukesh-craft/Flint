#!/usr/bin/env bash
# Gate: the scoped `bytes` type (P1, 2026-10-26).
#
# Contract under test:
#   * `bytes` is a DISTINCT type from `[i64]` even though both use the
#     runtime {ptr, i64} slice;
#   * b[i] reads one byte and zero-extends (0..255);
#   * b[i] = v stores the low 8 bits (documented truncation);
#   * indexing is bounds-checked by default (positive and negative) and
#     the check disappears under --unsafe;
#   * len(b), flint_bytes_len, flint_bytes_data and flint_bytes_free work;
#   * bytes and [i64] cannot be mixed silently (the checker rejects it);
#   * the one-command JIT path and the AOT artifact agree.
#
# Usage: bash tests/test_bytes.sh [path/to/flintc]   (default: ./flintc)
set -u
FLINTC="${1:-./flintc}"
_TMPBASE="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
T="$(mktemp -d "$_TMPBASE/flint-bytes.XXXXXX" 2>/dev/null || echo "$_TMPBASE/flint-bytes.$$")"
trap 'rm -rf "$T"' EXIT
pass=0; fail=0

check() { # $1=description $2=expected $3=actual
    if [ "$2" = "$3" ]; then
        pass=$((pass+1))
    else
        fail=$((fail+1))
        echo "FAIL: $1"
        echo "  expected: $(printf '%s' "$2" | tr '\n' '|')"
        echo "  actual:   $(printf '%s' "$3" | tr '\n' '|')"
    fi
}

check_yes() { # $1=description $2=condition-result
    if [ "$2" = "yes" ]; then pass=$((pass+1));
    else fail=$((fail+1)); echo "FAIL: $1"; fi
}

cat > "$T/bytes.fl" <<'EOF'
fn fill(b: bytes) -> i64 {
    mut i: i64 = 0
    while i < len(b) {
        b[i] = i * 2
        i = i + 1
    }
    0
}

fn sum(b: bytes) -> i64 {
    mut s: i64 = 0
    mut i: i64 = 0
    while i < len(b) {
        s = s + b[i]
        i = i + 1
    }
    s
}

fn main() -> i64 {
    b: bytes = flint_bytes_alloc(8)
    print(len(b))
    print(flint_bytes_len(b))
    print(b[0])                 # zero-initialised
    b[0] = 7
    b[1] = 255
    b[2] = 300                  # low 8 bits -> 44
    print(b[0])
    print(b[1])
    print(b[2])
    print(flint_ptr_to_int(flint_bytes_data(b)) != 0)
    fill(b)
    print(sum(b))               # 0+2+4+6+8+10+12+14
    flint_bytes_free(b)
    0
}
EOF

want="8
8
0
7
255
44
1
56"

# 1. One-command AOT: -o builds, runs and keeps the executable.
if ! out="$(timeout -s KILL 300 "$FLINTC" "$T/bytes.fl" -o "$T/bytes.bin" 2>"$T/build.log")"; then
    echo "FAIL: bytes fixture did not compile"; cat "$T/build.log"; exit 1
fi
check "AOT bytes output (-o runs it)" "$want" "$out"
check "AOT bytes executable remains" "yes" "$(test -x "$T/bytes.bin" && echo yes || echo no)"
out="$("$T/bytes.bin" 2>"$T/run.err")"
check "AOT bytes output (rerun)" "$want" "$out"

# 2. The one-command JIT path produces the same answer.
out="$(timeout -s KILL 300 "$FLINTC" "$T/bytes.fl" 2>"$T/jit.err")"
check "JIT bytes output" "$want" "$out"

# 3. Positive out-of-bounds traps in the safe build.
cat > "$T/oob.fl" <<'EOF'
fn main() -> i64 {
    b: bytes = flint_bytes_alloc(64)
    print(b[64])
    0
}
EOF
timeout -s KILL 300 "$FLINTC" "$T/oob.fl" -o "$T/oob.bin" --no-run >/dev/null 2>&1
( "$T/oob.bin" >"$T/oob.out" 2>"$T/oob.err" ) 2>/dev/null
rc=$?
check_yes "safe b[len] exits nonzero" "$(test "$rc" -ne 0 && echo yes || echo no)"
check_yes "safe b[len] reports out of bounds" \
    "$(grep -q 'out of bounds for length 64' "$T/oob.err" && echo yes || echo no)"

# 4. A negative index traps too (unsigned compare catches it).
cat > "$T/neg.fl" <<'EOF'
fn main() -> i64 {
    b: bytes = flint_bytes_alloc(64)
    mut i: i64 = 0 - 1
    print(b[i])
    0
}
EOF
timeout -s KILL 300 "$FLINTC" "$T/neg.fl" -o "$T/neg.bin" --no-run >/dev/null 2>&1
( "$T/neg.bin" >"$T/neg.out" 2>"$T/neg.err" ) 2>/dev/null
rc=$?
check_yes "safe b[-1] exits nonzero" "$(test "$rc" -ne 0 && echo yes || echo no)"
check_yes "safe b[-1] reports out of bounds" \
    "$(grep -q 'out of bounds' "$T/neg.err" && echo yes || echo no)"

# 5. --unsafe removes the check: the same one-past-the-end read returns.
timeout -s KILL 300 "$FLINTC" --unsafe "$T/oob.fl" -o "$T/oob_unsafe.bin" --no-run >/dev/null 2>&1
"$T/oob_unsafe.bin" >"$T/oob_unsafe.out" 2>"$T/oob_unsafe.err"
rc=$?
check_yes "--unsafe b[len] exits 0" "$(test "$rc" -eq 0 && echo yes || echo no)"
check_yes "--unsafe b[len] prints a value" \
    "$(test -n "$(cat "$T/oob_unsafe.out")" && echo yes || echo no)"

# 6. The policy counter sees the bytes accesses, and --unsafe removes them.
safe_checks=$(timeout -s KILL 300 "$FLINTC" --check-bce "$T/oob.fl" -o "$T/c1.bin" --no-run 2>&1 |
    awk '/inserted by policy -- bounds/ {print $7+0; exit}')
unsafe_checks=$(timeout -s KILL 300 "$FLINTC" --unsafe --check-bce "$T/oob.fl" -o "$T/c2.bin" --no-run 2>&1 |
    awk '/inserted by policy -- bounds/ {print $7+0; exit}')
check_yes "bytes indexing emits a bounds check" \
    "$(test -n "$safe_checks" && test "$safe_checks" -gt 0 && echo yes || echo no)"
check_yes "--unsafe removes the bytes bounds check" \
    "$(test -n "$unsafe_checks" && test "$unsafe_checks" -eq 0 && echo yes || echo no)"

# 7. The checker refuses to mix bytes and [i64] in either direction.
cat > "$T/bad_bytes.fl" <<'EOF'
fn main() -> i64 {
    b: bytes = flint_array_alloc(4)
    0
}
EOF
out=$(timeout -s KILL 300 "$FLINTC" "$T/bad_bytes.fl" -o "$T/bad1.bin" 2>&1)
rc=$?
check_yes "bytes <- array is rejected" "$(test "$rc" -ne 0 && echo yes || echo no)"
check_yes "bytes <- array reports E0290" "$(printf '%s' "$out" | grep -q E0290 && echo yes || echo no)"

cat > "$T/bad_array.fl" <<'EOF'
fn main() -> i64 {
    a: [i64] = flint_bytes_alloc(4)
    0
}
EOF
out=$(timeout -s KILL 300 "$FLINTC" "$T/bad_array.fl" -o "$T/bad2.bin" 2>&1)
rc=$?
check_yes "array <- bytes is rejected" "$(test "$rc" -ne 0 && echo yes || echo no)"
check_yes "array <- bytes reports E0290" "$(printf '%s' "$out" | grep -q E0290 && echo yes || echo no)"

cat > "$T/bad_assign.fl" <<'EOF'
fn main() -> i64 {
    mut b: bytes = flint_bytes_alloc(4)
    b = flint_array_alloc(4)
    0
}
EOF
out=$(timeout -s KILL 300 "$FLINTC" "$T/bad_assign.fl" -o "$T/bad3.bin" 2>&1)
rc=$?
check_yes "bytes <- array assignment is rejected" "$(test "$rc" -ne 0 && echo yes || echo no)"
check_yes "bytes <- array assignment reports E0292" "$(printf '%s' "$out" | grep -q E0292 && echo yes || echo no)"

echo "bytes: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
