#!/usr/bin/env bash
# A1: verify README tables match the tree (deterministic counts only).
# Benchmark timings are device-sensitive and are NOT checked here.
# Usage: bash tools/docs_check.sh [repo-root]   (default: .)
# Exit nonzero listing every drifted value; keep in sync with README.md
# "Testing" table when files change.
set -u
ROOT="${1:-.}"
fail=0
check() { # $1=desc $2=want $3=got
    if [ "$2" = "$3" ]; then echo "ok: $1 ($3)";
    else echo "FAIL: $1 (want [$2] got [$3]; update README.md Testing table)"; fail=$((fail+1)); fi
}
lines() { wc -l < "$ROOT/$1" | tr -d ' '; }

check "t_hello.fl" 8 "$(lines tests/t_hello.fl)"
check "t_arith.fl" 14 "$(lines tests/t_arith.fl)"
check "t_flow.fl" 48 "$(lines tests/t_flow.fl)"
check "t_funcs.fl" 15 "$(lines tests/t_funcs.fl)"
check "t_types.fl" 23 "$(lines tests/t_types.fl)"
check "smoke total" 108 "$(( $(lines tests/t_hello.fl) + $(lines tests/t_arith.fl) + $(lines tests/t_flow.fl) + $(lines tests/t_funcs.fl) + $(lines tests/t_types.fl) ))"
check "run.sh" 45 "$(lines tests/run.sh)"
check "run_tutorial.sh" 18 "$(lines tests/run_tutorial.sh)"
check "test_registry.sh" 43 "$(lines tests/test_registry.sh)"
check "runners total" 106 "$(( $(lines tests/run.sh) + $(lines tests/run_tutorial.sh) + $(lines tests/test_registry.sh) ))"
check "fib.fl" 15 "$(lines benchmarks/fib.fl)"
check "fib2.fl" 11 "$(lines benchmarks/fib2.fl)"
check "pi.fl" 23 "$(lines benchmarks/pi.fl)"
check "primes.fl" 33 "$(lines benchmarks/primes.fl)"
check "strrev.fl" 34 "$(lines benchmarks/strrev.fl)"
check "sum_array.fl" 31 "$(lines benchmarks/sum_array.fl)"
check "workloads total" 147 "$(( $(lines benchmarks/fib.fl) + $(lines benchmarks/fib2.fl) + $(lines benchmarks/pi.fl) + $(lines benchmarks/primes.fl) + $(lines benchmarks/strrev.fl) + $(lines benchmarks/sum_array.fl) ))"
check "test_* probes total" 86 "$(cat "$ROOT"/benchmarks/test_*.fl | wc -l | tr -d ' ')"
check "workloads+probes" 233 "$(( $(cat "$ROOT"/benchmarks/fib.fl "$ROOT"/benchmarks/fib2.fl "$ROOT"/benchmarks/pi.fl "$ROOT"/benchmarks/primes.fl "$ROOT"/benchmarks/strrev.fl "$ROOT"/benchmarks/sum_array.fl "$ROOT"/benchmarks/test_*.fl | wc -l | tr -d ' ') ))"
check "mirrors total" 275 "$(cat "$ROOT"/benchmarks/*.c "$ROOT"/benchmarks/*.cpp "$ROOT"/benchmarks/*.py | wc -l | tr -d ' ')"
check "tutorial files" 8 "$(ls "$ROOT"/tutorial/0*.fl | wc -l | tr -d ' ')"
check "tutorial lines" 198 "$(cat "$ROOT"/tutorial/0*.fl | wc -l | tr -d ' ')"
check "agent-bench tasks" 136 "$(find "$ROOT"/agent-bench/task-* -type f | xargs cat | wc -l | tr -d ' ')"
check "agent-bench total" 160 "$(find "$ROOT"/agent-bench -type f | xargs cat | wc -l | tr -d ' ')"
check "ladder goldens" 21 "$(ls "$ROOT"/stage3/ladder/*.ll | wc -l | tr -d ' ')"

echo "docs-check: failures=$fail"
[ "$fail" -eq 0 ]
