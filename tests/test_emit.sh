#!/usr/bin/env bash
# Stage 3 de-risk gate: canonical .ll files must assemble, verify, link, run.
# 5 gates per file (weavec0 order): llvm-as (+verify) -> clang link -> run+exit.
# This proves the IR template, runtime ABI, and harness BEFORE the Flint
# emitter exists. Usage: bash tests/test_emit.sh
set -u
pass=0; fail=0
check() { # $1=file $2=expected-exit
    local file="$1" want="$2" base out rc
    base="$(basename "$file" .ll)"
    out="/data/data/com.termux/files/usr/tmp/slipstream/emit_$base"
    if ! llvm-as "$file" -o "$out.bc" 2>/dev/null; then echo "FAIL $file (llvm-as)"; fail=$((fail+1)); return; fi
    if ! opt -passes=verify "$out.bc" -o /dev/null 2>/dev/null; then echo "FAIL $file (verify)"; fail=$((fail+1)); return; fi
    if ! clang "$file" -o "$out" 2>/dev/null; then echo "FAIL $file (clang)"; fail=$((fail+1)); return; fi
    "$out" > /dev/null 2>&1
    rc=$?
    if [ "$rc" -eq "$want" ]; then pass=$((pass+1));
    else echo "FAIL $file (exit $rc, want $want)"; fail=$((fail+1)); fi
}
check stage3/00_return_const.ll 42
check stage3/01_add.ll 42
check stage3/02_if_phi.ll 20
echo "emit-gate: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
