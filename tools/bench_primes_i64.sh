#!/usr/bin/env bash
# Same-representation control for the `primes` benchmark.
#
# benchmarks/primes.fl uses Flint's only array element type, i64 (80 MB
# sieve).  The main C/C++/Rust mirrors use 1-byte elements (10 MB), so the
# headline ratio measures memory traffic as much as codegen.  This script
# builds the i64 mirrors in benchmarks/controls/ and runs every variant in
# the same session, alternating, so thermal drift affects all of them.
#
# Usage: bash tools/bench_primes_i64.sh [rounds]   (default: 3)
set -u
cd "$(dirname "$0")/.."
FLINTC="${FLINTC:-./flintc}"
T="${TMPDIR:-/data/data/com.termux/files/usr/tmp}/bench_primes_i64"
ROUNDS="${1:-3}"
mkdir -p "$T"

if ! "$FLINTC" benchmarks/primes.fl --no-run -o "$T/primes.flint" >/dev/null 2>&1; then
    echo "FAIL flint build"; exit 1
fi
if ! "$FLINTC" benchmarks/primes_bytes.fl --no-run -o "$T/primes.bytes" >/dev/null 2>&1; then
    echo "FAIL flint bytes build"; exit 1
fi
if ! clang -O2 benchmarks/controls/primes_i64.c -o "$T/primes.c.bin" -lm; then
    echo "FAIL C build"; exit 1
fi
if ! clang++ -O2 benchmarks/controls/primes_i64.cpp -o "$T/primes.cpp.bin" -lm; then
    echo "FAIL C++ build"; exit 1
fi
if ! clang -O2 benchmarks/primes.c -o "$T/primes_char.c.bin" -lm; then
    echo "FAIL C char build"; exit 1
fi
if command -v rustc >/dev/null 2>&1; then
    if ! rustc -O benchmarks/controls/primes_i64.rs -o "$T/primes.rs.bin" >/dev/null 2>&1; then
        echo "FAIL Rust build"; exit 1
    fi
else
    echo "rustc not found: the Rust column will be n/a"
fi

time_of() { "$1" 2>/dev/null | awk '/^time:/ {print $2; exit}'; }
ms() { awk -v n="$1" 'BEGIN { printf "%.1f", n / 1000000 }'; }
median() { sort -n | awk '{a[NR]=$1} END { if (NR == 0) print "n/a"; else printf "%.1f", a[int((NR+1)/2)] / 1000000 }'; }

rm -f "$T"/*.ns
printf "%-6s %10s %10s %10s %10s %10s %10s\n" "round" "Flint i64" "Flint bytes" "C i64" "C++ i64" "Rust i64" "C char"
for r in $(seq 1 "$ROUNDS"); do
    f=$(time_of "$T/primes.flint")
    fb=$(time_of "$T/primes.bytes")
    c=$(time_of "$T/primes.c.bin")
    cpp=$(time_of "$T/primes.cpp.bin")
    rs=""
    [ -x "$T/primes.rs.bin" ] && rs=$(time_of "$T/primes.rs.bin")
    ch=$(time_of "$T/primes_char.c.bin")
    echo "$f" >> "$T/flint.ns"; echo "$fb" >> "$T/flint_bytes.ns"
    echo "$c" >> "$T/c.ns"
    echo "$cpp" >> "$T/cpp.ns"; echo "$ch" >> "$T/char.ns"
    [ -n "$rs" ] && echo "$rs" >> "$T/rs.ns"
    printf "%-6s %8sms %10sms %8sms %8sms %8sms %8sms\n" "$r" \
        "$(ms "$f")" "$(ms "$fb")" "$(ms "$c")" "$(ms "$cpp")" \
        "$([ -n "$rs" ] && ms "$rs" || echo n/a)" "$(ms "$ch")"
done
echo
printf "%-6s %8sms %10sms %8sms %8sms %8sms %8sms\n" "median" \
    "$(median < "$T/flint.ns")" "$(median < "$T/flint_bytes.ns")" \
    "$(median < "$T/c.ns")" \
    "$(median < "$T/cpp.ns")" \
    "$([ -s "$T/rs.ns" ] && median < "$T/rs.ns" || echo n/a)" \
    "$(median < "$T/char.ns")"
echo
echo 'Flint i64 is the [i64] array (8-byte elements); Flint bytes is the'
echo 'same algorithm over the i8 `bytes` type. The C/C++/Rust i64 columns'
echo 'are the same 8-byte representation; C char is the 1-byte mirror.'
