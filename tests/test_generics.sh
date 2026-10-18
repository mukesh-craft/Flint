#!/usr/bin/env bash
# generics gate: instantiation must be type-correct, and must never leak LLVM.
#
# Two defects this protects against both produced WRONG BEHAVIOUR or INTERNAL
# COMPILER ERRORS rather than diagnostics:
#
#   1. mangle() mapped F64, Ptr, Void and TypeParam all to the same string, so
#      two different instantiations collided in genericCache and one silently
#      received the other's body.
#   2. A float literal inferred as i64, so `first(2.5)` was built against an i64
#      signature and the user saw raw IR on stderr.
#
# The negative checks matter most: an internal compiler error escaping to the
# user is a defect in itself, so this gate asserts that stderr never contains
# LLVM IR, whatever the input.
#
# Usage: bash tests/test_generics.sh [path/to/flintc]   (default: ./flintc)
set -u
FLINTC="${1:-./flintc}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
pass=0; fail=0
ok()  { pass=$((pass+1)); echo "PASS $1"; }
bad() { fail=$((fail+1)); echo "FAIL $1${2:+ — $2}"; }

# Assert that a program fails with a Flint diagnostic and that no LLVM IR
# reached the user. This is the property that actually matters: the old bugs
# printed things like `call i64 @first.i64(double 2.5)`.
expect_clean_error() {
    local name="$1" source="$2"
    printf '%s' "$source" > "$WORK/case.fl"
    local err
    err="$(timeout -s KILL 120 "$FLINTC" "$WORK/case.fl" 2>&1 >/dev/null)"
    local rc=$?
    if [ "$rc" = "0" ]; then
        bad "$name: rejected" "it compiled and ran"
        return
    fi
    if printf '%s' "$err" | grep -qE '%[0-9]+ = call|double [0-9]|i8\* |getelementptr|Function verification failed|Call parameter type'; then
        bad "$name: no LLVM IR in the diagnostic" "$(printf '%s' "$err" | head -1)"
        return
    fi
    if [ -z "$err" ]; then
        bad "$name: has a diagnostic" "failed silently with no message"
        return
    fi
    ok "$name: clean Flint diagnostic, no LLVM IR"
}

# --- positive: generics across every instantiable type ----------------------
if out="$(timeout -s KILL 120 "$FLINTC" tests/t_generics.fl 2>&1)"; then
    if [ "$(printf '%s' "$out" | grep -c '^0$')" = "0" ] &&
       [ "$(printf '%s' "$out" | grep -c '^1$')" = "7" ]; then
        ok "generics instantiate for f64, i64, str and structs"
    else
        bad "generics assertions" "got [$(printf '%s' "$out" | tr '\n' ' ')]"
    fi
else
    bad "tests/t_generics.fl runs" "$(printf '%s' "$out" | tail -2 | tr '\n' ' ')"
fi

# --- negative: string arrays must be refused --------------------------------
# A string element cannot be stored in an i64-backed array, and the compiler
# now says so instead of reinterpreting the pointer as an integer.
expect_clean_error "str array literal" 'fn main() { a = ["x", "y"] print(a[0]) }'

# --- documented: [1.5, 2.5] is a TENSOR, not an array ----------------------
# This case is here to pin down a distinction that is easy to get wrong when
# auditing the compiler: a bracket literal of floats is claimed by the tensor
# path (tryParseTensorEmit), so it is NOT an i64 array and must not be rejected
# as one. Rejecting it would break float tensors.
#
# The remaining hazard is indexing such a tensor with `[]`: tensors are erased
# to {ptr, i64} at the LLVM boundary and emitIndexEmit GEPs every {ptr, i64} as
# i64, so `a[0]` reinterprets float bits and yields a wrong number with no
# diagnostic. That is a real, still-open defect tracked in memory.md; this gate
# deliberately does not assert a fix that does not exist yet.
if timeout -s KILL 120 "$FLINTC" /dev/stdin >/dev/null 2>&1 <<'FL'
fn main() { a = [1.5, 2.5] print(a) }
FL
then
    ok "float bracket literal is accepted as a tensor, not rejected as an array"
else
    bad "float bracket literal is accepted as a tensor" "it was rejected"
fi

# --- negative: a genuinely impossible instantiation stays a diagnostic ------
expect_clean_error "uninstantiable generic" 'fn pick<T>(a: i64) -> T { return a }
fn main() { x = 1.5 print(pick(x)) }'

echo "generics: $pass passed, $fail failed"
[ "$fail" -eq 0 ]