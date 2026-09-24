#!/usr/bin/env bash
# Opt-identity gate (V1): the same program built at different opt levels
# must produce identical stdout (minus timing lines) and exit codes.
# Catches opt-level miscompiles (e.g. the historic O0 `trunc void`
# segfault class). `--unsafe` is included: on overflow-free programs it
# must agree exactly (it only drops traps, never changes values).
# Usage: bash tests/test_opt_identity.sh [path/to/flintc]   (default: ./flintc)
set -u
FLINTC="${1:-./flintc}"
TMPD="/data/data/com.termux/files/usr/tmp/slipstream/optident"
mkdir -p "$TMPD"
pass=0; fail=0
# Deterministic, quick corpus (no timing dependence; fib45 excluded: slow).
CASES="benchmarks/fib2.fl benchmarks/sum_array.fl benchmarks/strrev.fl benchmarks/primes.fl benchmarks/pi.fl benchmarks/test_fib3.fl benchmarks/test_simple_if.fl benchmarks/test_rec.fl examples/fn_demo.fl examples/strings.fl examples/math.fl"
norm() { # strip timing lines + trailing whitespace
  grep -v -E "^time: [0-9]+ ns$" | sed 's/[[:space:]]*$//'
}
for src in $CASES; do
  b="$(basename "$src" .fl)"
  ref_ok=1
  ref_out=""; ref_rc=0
  i=0
  for cfg in O2 fast O0 O3 unsafe; do
    case "$cfg" in
      O2)     flag="--opt-level 2" ;;
      fast)   flag="--fast" ;;
      O0)     flag="--opt-level 0" ;;
      O3)     flag="--opt-level 3" ;;
      unsafe) flag="--unsafe" ;;
    esac
    # shellcheck disable=SC2086
    if ! timeout -s KILL 300 "$FLINTC" "$src" -o "$TMPD/p" $flag > /dev/null 2>&1; then
      echo "FAIL $b/$cfg (build)"; fail=$((fail+1)); ref_ok=0; continue
    fi
    out="$(timeout -s KILL 120 "$TMPD/p" 2>&1 | norm)"; rc=$?
    if [ "$i" -eq 0 ]; then
      ref_out="$out"; ref_rc=$rc
    else
      if [ "$rc" -ne "$ref_rc" ] || [ "$out" != "$ref_out" ]; then
        echo "FAIL $b/$cfg (diverges from O2: rc=$rc vs $ref_rc)"
        diff <(printf '%s' "$ref_out") <(printf '%s' "$out") | head -n 4
        fail=$((fail+1)); ref_ok=0
      fi
    fi
    i=$((i+1))
  done
  if [ "$ref_ok" -eq 1 ]; then echo "PASS $b (5 configs agree)"; pass=$((pass+1)); fi
done
echo "opt-identity: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
