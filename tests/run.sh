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
check tests/t_hello.fl "1"
check tests/t_arith.fl "48"
check tests/t_arith.fl "-1"
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
check tests/t_types.fl "HELLO"
check tests/t_types.fl "42"
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
check_fail tests/t_for_bad.fl "for-in needs an array or str collection"
echo "smoke: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
