#!/usr/bin/env bash
# Flint agent benchmark: each task-N dir needs solution.fl; output must match
# expected.txt exactly (exit 0). Reports pass/fail + seconds per task.
# Usage: bash agent-bench/check.sh [path/to/flintc]   (default: ./flintc)
# Reference solutions (reference.fl) are NOT used by the checker — they only
# validate the harness (see memory.md v0.21).
set -u
FLINTC="${1:-./flintc}"
pass=0; fail=0; skipped=0; total_sec=0
for d in agent-bench/task-*; do
    name="$(basename "$d")"
    [ -f "$d/solution.fl" ] || { echo "SKIP $name (no solution.fl)"; skipped=$((skipped+1)); continue; }
    start=$(date +%s)
    got="$("$FLINTC" "$d/solution.fl" 2>&1)"
    rc=$?
    end=$(date +%s)
    total_sec=$((total_sec + end - start))
    want="$(cat "$d/expected.txt")"
    if [ $rc -ne 0 ]; then echo "FAIL $name (exit $rc)"; echo "$got" | head -n 3; fail=$((fail+1));
    elif [ "$got" = "$want" ]; then echo "PASS $name ($((end-start))s)"; pass=$((pass+1));
    else echo "FAIL $name (output mismatch)"; echo "--- want ---"; printf '%s\n' "$want"; echo "--- got ---"; printf '%s\n' "$got" | head -n 8; fail=$((fail+1)); fi
done
echo "agent-bench: $pass passed, $fail failed, $skipped skipped (${total_sec}s total)"
[ "$fail" -eq 0 ] && [ "$pass" -gt 0 ]
