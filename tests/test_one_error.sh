#!/usr/bin/env bash
# Gate: a malformed program must produce exactly ONE error.
#
# The parse side had this contract since M6 ("stop the cascade"). The
# codegen side did not: after reporting an error, the emitter kept going,
# built IR that no longer matched the source, and LLVM's verifier then
# printed a second message about the compiler's internals -- so the user was
# told about a compiler bug instead of about their program.
#
# This gate counts the error lines of every known-bad fixture. It is the
# machine-checkable form of "one error per input".
#
# Usage: bash tests/test_one_error.sh [path/to/flintc]  (default: ./flintc)
set -u
cd "$(dirname "$0")/.."
FLINTC="${1:-./flintc}"
T="${TMPDIR:-/data/data/com.termux/files/usr/tmp}/one_error"
mkdir -p "$T"
pass=0; fail=0

# Fixtures that must be rejected, with the message each must contain.
CASES=(
    "tests/t_for_bad.fl|for-in needs an array or str collection"
    "tests/t_pfor_removed.fl|'parallel for' was removed"
    "tests_manual/t_explicit_targs_bad.fl|expects 1 type argument(s), got 2"
    "tests_manual/t_explicit_targs_nongen.fl|explicit type arguments are only allowed on a generic function"
    "tests_manual/t_named_fn_escape.fl|cannot return a function value"
    "tests_manual/t_generic_no_infer.fl|cannot infer type parameter"
)

for c in "${CASES[@]}"; do
    file="${c%%|*}"
    want="${c##*|}"
    if [ ! -f "$file" ]; then
        echo "FAIL $file (missing fixture)"; fail=$((fail+1)); continue
    fi
    out="$("$FLINTC" "$file" -o "$T/out.bin" 2>&1)"
    rc=$?
    if [ $rc -eq 0 ]; then
        echo "FAIL $file (expected a compile error, exited 0)"; fail=$((fail+1)); continue
    fi
    if ! printf '%s\n' "$out" | grep -qF -- "$want"; then
        echo "FAIL $file (missing '$want')"; printf '%s\n' "$out" | head -3; fail=$((fail+1)); continue
    fi
    # Count the error lines. The driver's "function body emission failed"
    # banner is a consequence, not a second error; LLVM's verifier output
    # ("... does not match ...") IS a second error and must not appear.
    n=$(printf '%s\n' "$out" | grep -cE '^(codegen:|error at line|error:|[A-Za-z].*does not match|.*Assertion.*failed)')
    if [ "$n" != "1" ]; then
        echo "FAIL $file (expected exactly 1 error line, got $n)"
        printf '%s\n' "$out" | head -6
        fail=$((fail+1)); continue
    fi
    pass=$((pass+1))
done

echo "one-error: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
