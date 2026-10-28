#!/usr/bin/env bash
# rust-oracle gate: a memory-safe reimplementation must agree with Flint on
# deterministic output. The benchmark suite compares timings; this compares
# *results*, so a semantic drift in Flint's arithmetic or array handling is
# caught by a compiler that shares no code with it.
#
# Timing lines differ by design, so only the deterministic line is compared.
#
# Usage: bash tests/test_rust_oracle.sh [path/to/flintc]   (default: ./flintc)
set -u
FLINTC="${1:-./flintc}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
WORK="${TMPDIR:-/tmp}/flint-rust-oracle"
rm -rf "$WORK"; mkdir -p "$WORK"
pass=0; fail=0

command -v rustc >/dev/null 2>&1 || { echo "SKIP rust-oracle (rustc not installed: pkg install rust)"; exit 0; }

compare() { # $1=label $2=rust-src $3=flint-src $4=expected-line
    local label="$1" rsrc="$2" fsrc="$3" want="$4"
    if ! rustc --edition 2021 -O "$rsrc" -o "$WORK/$label.rs.bin" > "$WORK/$label.build" 2>&1; then
        echo "FAIL $label (rust build)"; sed -n '1,8p' "$WORK/$label.build"; fail=$((fail+1)); return
    fi
    local rout fout
    rout="$("$WORK/$label.rs.bin" 2>&1)"; local rrc=$?
    fout="$(timeout -s KILL 600 "$FLINTC" "$fsrc" 2>&1 | head -n 1)"; local frc=${PIPESTATUS[0]}
    if [ "$rrc" -ne 0 ]; then
        echo "FAIL $label (rust exit $rrc): $rout"; fail=$((fail+1)); return
    fi
    if [ "$fout" != "$want" ]; then
        echo "FAIL $label (flint line '$fout' != expected '$want')"; fail=$((fail+1)); return
    fi
    if [ "$rout" != "$fout" ]; then
        echo "FAIL $label (rust '$rout' != flint '$fout')"; fail=$((fail+1)); return
    fi
    pass=$((pass+1)); echo "PASS $label ($fout)"
}

compare sum_array tests/rust/oracle_sum.rs benchmarks/sum_array.fl "sum: 99999990000000"
compare primes tests/rust/oracle_primes.rs benchmarks/primes.fl "primes up to 10M: 664579"

echo "rust-oracle: $pass passed, $fail failed"
[ "$fail" -eq 0 ]