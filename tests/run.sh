#!/usr/bin/env bash
# Flint smoke tests: each tests/*.fl must exit 0 and print its markers,
# except tests/t_for_bad.fl which must FAIL to compile with the A2 error.
# Usage: bash tests/run.sh [path/to/flintc]   (default: ./flintc)
set -u
FLINTC="${1:-./flintc}"
pass=0; fail=0
check() { # $1=file $2=expected-line
    local file="$1" want="$2" out rc
    out="$("$FLINTC" "$file" 2>&1)"
    rc=$?
    if [ $rc -ne 0 ]; then echo "FAIL $file (exit $rc)"; echo "$out" | head -n 3; fail=$((fail+1)); return; fi
    if printf '%s\n' "$out" | grep -qxF -- "$want"; then pass=$((pass+1));
    else echo "FAIL $file (missing '$want')"; echo "$out" | head -n 5; fail=$((fail+1)); fi
}
check tests/t_hello.fl "hello"
check tests/t_strings.fl "json: {\"a\": 42}"
check tests/t_strings.fl "7 items for world"
check tests/t_hello.fl "1"
check tests/t_arith.fl "48"
check tests/t_arith.fl "-1"
check tests/t_arith.fl "-7"
check tests/t_arith.fl "4611686018427387904"
check tests/t_arith.fl "57"
check tests/t_flow.fl "25"
check tests/t_flow.fl "9"
check tests/t_flow.fl "3"
check tests/t_flow.fl "42"
check tests/t_flow.fl "100"
check tests/t_flow.fl "101"
check tests/t_flow.fl "102"
check tests/t_flow.fl "103"
check tests/t_flow.fl "104"
check tests/t_funcs.fl "42"
check tests/t_funcs.fl "55"
# M2 residue: generic argument inference for cast and nested-call arguments.
# The 1.5 line is the discriminator: an i64 instantiation would print 1.
check tests/t_generic_infer.fl "5"
check tests/t_generic_infer.fl "9"
check tests/t_generic_infer.fl "2"
check tests/t_generic_infer.fl "1.5"
check tests/t_generic_infer.fl "7"
check tests/t_generic_infer.fl "4"
# M2c: a float arithmetic expression and a float field infer f64.
check tests/t_generic_infer.fl "2.5"
# M2b: explicit type arguments `f::<T>(...)`. New syntax, so the fixture
# lives in tests_manual/ (the self-hosted parser does not implement it yet).
check tests_manual/t_explicit_targs.fl "5"
check tests_manual/t_explicit_targs.fl "3"
check tests_manual/t_explicit_targs.fl "7"
check tests_manual/t_explicit_targs.fl "2.5"
check tests_manual/t_explicit_targs.fl "42"
# M4 follow-up: a named function used as a value (`let h = inc`).
check tests_manual/t_named_fn_value.fl "6"
check tests_manual/t_named_fn_value.fl "10"
# Phase 2 / 2A: return-type inference across the shapes that used to emit
# invalid IR (forward reference, explicit return, return-statement type).
check tests_manual/t_return_inference.fl "hello"
check tests_manual/t_return_inference.fl "hi"
# Copying a function VALUE into another binding keeps its fn type.
check tests_manual/t_fn_value_copy.fl "6"
check tests_manual/t_fn_value_copy.fl "10"
# A function with an fn(...) RETURN type that falls off the end must build
# valid IR (the null value of the return type, not an integer zero).
check tests_manual/t_fn_return_type.fl "1"
check tests/t_types.fl "HELLO"
check tests/t_types.fl "42"
# util + time under JIT: these load .o files whose atomics must resolve in
# the JIT's own symbol pool (compiler-rt gap, item 17) — a standing gate
# for the no-outline-atomics runtime build.
check tests/t_util.fl "cli-ok"
check tests/t_time.fl "EDT"
check tests/t_lexkit.fl "hello world 42"
check tests/t_lexkit.fl "1998"
check_fail() { # $1=file $2=expected-error-substring: must exit nonzero with message
    local file="$1" want="$2" out rc
    out="$("$FLINTC" "$file" 2>&1)"
    rc=$?
    if [ $rc -eq 0 ]; then echo "FAIL $file (expected compile failure, exited 0)"; fail=$((fail+1)); return; fi
    if printf '%s\n' "$out" | grep -qF -- "$want"; then pass=$((pass+1));
    else echo "FAIL $file (missing error '$want')"; echo "$out" | head -n 5; fail=$((fail+1)); fi
}
check_flag() { # $1=flag $2=expected-prefix: --help/--version must exit 0 and print it
    local flag="$1" want="$2" out rc
    out="$("$FLINTC" "$flag" 2>&1)"
    rc=$?
    if [ $rc -ne 0 ]; then echo "FAIL $flag (exit $rc)"; fail=$((fail+1)); return; fi
    if printf '%s\n' "$out" | grep -qF -- "$want"; then pass=$((pass+1));
    else echo "FAIL $flag (missing '$want')"; echo "$out" | head -n 3; fail=$((fail+1)); fi
}
check_flag --help "USAGE"
check_flag --version "flintc "
check_fail tests/t_for_bad.fl "for-in needs an array or str collection"
# B6: 'parallel for' is removed with a migration message.
check_fail tests/t_pfor_removed.fl "'parallel for' was removed: it could not capture variables and had no result channel. Use a plain 'for' loop, or move the work into a Rust runtime function such as flint_parallel_reduce"
# M2b: explicit type arguments — wrong count, and a non-generic callee.
check_fail tests_manual/t_explicit_targs_bad.fl "'idc' expects 1 type argument(s), got 2"
check_fail tests_manual/t_explicit_targs_nongen.fl "explicit type arguments are only allowed on a generic function"
# M4 follow-up: a named function value is frame-bound, like a lambda.
check_fail tests_manual/t_named_fn_escape.fl "cannot return a function value: it would escape its defining frame"
# M2c: an argument whose type cannot be determined is reported by name.
check_fail tests_manual/t_generic_no_infer.fl "cannot infer type parameter 'T' for 'idc'; the argument's type could not be determined"
echo "smoke: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
