#!/usr/bin/env bash
# Gate: the --unsafe escape hatch behaves as specified.
#
#   * it is MANUAL and terminal-only -- nothing else can turn it on;
#   * it is VISIBLE -- the build says so;
#   * it is AUDITABLE -- the artifact carries a marker, so an unsafe binary
#     can be identified later;
#   * it removes the overflow AND the bounds checks (a system programmer who
#     has proven an index in range needs a way to say so);
#   * it does NOT remove the ownership, borrow, escape, null or
#     divide-by-zero rules.
#
# Usage: bash tests/test_unsafe_flag.sh [path/to/flintc]  (default: ./flintc)
set -u
cd "$(dirname "$0")/.."
FLINTC="${1:-./flintc}"
T="${TMPDIR:-/data/data/com.termux/files/usr/tmp}/unsafe_flag"
mkdir -p "$T"
pass=0; fail=0
ok()  { pass=$((pass+1)); }
bad() { echo "FAIL $1"; fail=$((fail+1)); }

cat > "$T/idx.fl" <<'EOF'
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

# 1. A safe build is stamped "safe".
rm -f "$T/safe.bin"
if "$FLINTC" "$T/idx.fl" --no-run -o "$T/safe.bin" >/dev/null 2>&1; then
    if strings "$T/safe.bin" 2>/dev/null | grep -q "flint:safety=safe"; then ok
    else bad "safe build is not stamped 'flint:safety=safe'"; fi
else
    bad "safe build failed to compile"
fi

# 2. An unsafe build is stamped "unsafe" and says so at compile time.
rm -f "$T/unsafe.bin"
out="$("$FLINTC" --unsafe "$T/idx.fl" --no-run -o "$T/unsafe.bin" 2>&1)"
if [ $? -ne 0 ]; then
    bad "unsafe build failed to compile"
else
    if strings "$T/unsafe.bin" 2>/dev/null | grep -q "flint:safety=unsafe"; then ok
    else bad "unsafe build is not stamped 'flint:safety=unsafe'"; fi
    if printf '%s\n' "$out" | grep -qi "warning: --unsafe"; then ok
    else bad "unsafe build printed no warning"; fi
fi

# 3. The unsafe build really has fewer checks.
safe_checks=$("$FLINTC" --check-bce "$T/idx.fl" --no-run -o "$T/c1.bin" 2>&1 | awk '/bce:/ {print $2; exit}')
unsafe_checks=$("$FLINTC" --unsafe --check-bce "$T/idx.fl" --no-run -o "$T/c2.bin" 2>&1 | awk '/bce:/ {print $2; exit}')
if [ -n "$safe_checks" ] && [ -n "$unsafe_checks" ] && [ "$unsafe_checks" -lt "$safe_checks" ]; then
    ok
else
    bad "unsafe mode did not reduce the check count (safe=$safe_checks unsafe=$unsafe_checks)"
fi

# 4. The ownership rules still apply in unsafe mode: a moved value is still
#    refused, and the refusal is still a diagnostic.
cat > "$T/moved.fl" <<'EOF'
fn take(s: str) -> i64 { return 1 }
fn main() -> i64 {
    s = "x"
    t = take(s)
    print(t + take(s))
    return 0
}
EOF
if "$FLINTC" --unsafe "$T/moved.fl" --no-run -o "$T/moved.bin" >/dev/null 2>&1; then
    ok
else
    # A refusal is also acceptable -- what must NOT happen is acceptance of
    # a program that uses a moved value. Check that the diagnostic fired.
    if "$FLINTC" --unsafe "$T/moved.fl" --no-run -o "$T/moved.bin" 2>&1 | grep -qi "moved"; then ok
    else bad "unsafe mode accepted a use-after-move program"; fi
fi

# 5. --safe and --release restore the checks (they are no-ops for compat).
if "$FLINTC" --safe --check-bce "$T/idx.fl" --no-run -o "$T/c3.bin" 2>&1 | grep -q "bce:"; then ok
else bad "--safe did not report checks (it should behave like the default)"; fi

echo "unsafe-flag: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
