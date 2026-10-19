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
echo "smoke: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
