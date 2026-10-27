#!/usr/bin/env bash
# Language comparison: run the SAME workloads in Flint, C, C++, Rust and
# Python on this device and print one table.
#
# Method: every implementation prints its own in-program time (the same
# clock, around the same work), so the numbers are comparable without
# process-startup noise. Flint and the mirrors are the files in
# benchmarks/ -- one workload each, same algorithm.
#
# Usage: bash tools/bench_compare.sh [workload ...]
#        (default: sum_array primes fib pi)
set -u
cd "$(dirname "$0")/.."
FLINTC="${FLINTC:-./flintc}"
T="${TMPDIR:-/data/data/com.termux/files/usr/tmp}/bench_compare"
mkdir -p "$T"

WORKLOADS=("$@")
if [ ${#WORKLOADS[@]} -eq 0 ]; then
    WORKLOADS=(sum_array primes fib pi)
fi

# ns -> a readable unit: ms for >= 1 ms, us below. A 500 ns call must not
# print as "0.0ms" just because the large workloads use milliseconds.
fmt() { awk -v n="$1" 'BEGIN{ if (n < 1000000) printf "%.1fus", n/1000; else printf "%.1fms", n/1000000 }'; }

run_flint() { # $1=workload
    local w="$1"
    if ! "$FLINTC" "benchmarks/$w.fl" --no-run -o "$T/$w.flint" >/dev/null 2>&1; then
        echo "n/a"; return
    fi
    "$T/$w.flint" 2>/dev/null | awk '/^time:/ {print $2; exit}'
}
run_c() { # $1=workload
    local w="$1"
    if ! clang -O2 -o "$T/$w.c.bin" "benchmarks/$w.c" -lm >/dev/null 2>&1; then
        echo "n/a"; return
    fi
    "$T/$w.c.bin" 2>/dev/null | awk '/^time:/ {print $2; exit}'
}
run_cpp() {
    local w="$1"
    if ! clang++ -O2 -o "$T/$w.cpp.bin" "benchmarks/$w.cpp" -lm >/dev/null 2>&1; then
        echo "n/a"; return
    fi
    "$T/$w.cpp.bin" 2>/dev/null | awk '/^time:/ {print $2; exit}'
}
run_rust() {
    local w="$1"
    if [ ! -f "benchmarks/$w.rs" ]; then echo "n/a"; return; fi
    if ! rustc -O -o "$T/$w.rs.bin" "benchmarks/$w.rs" >/dev/null 2>&1; then
        echo "n/a"; return
    fi
    "$T/$w.rs.bin" 2>/dev/null | awk '/^time:/ {print $2; exit}'
}
run_python() {
    local w="$1"
    if [ ! -f "benchmarks/$w.py" ]; then echo "n/a"; return; fi
    python3 "benchmarks/$w.py" 2>/dev/null | awk '/^time:/ {print $2; exit}'
}

printf "%-12s %12s %12s %12s %12s %12s\n" "workload" "Flint" "C" "C++" "Rust" "Python"
printf "%-12s %12s %12s %12s %12s %12s\n" "------------" "------------" \
    "------------" "------------" "------------" "------------"
for w in "${WORKLOADS[@]}"; do
    f=$(run_flint "$w"); c=$(run_c "$w"); cpp=$(run_cpp "$w")
    r=$(run_rust "$w"); p=$(run_python "$w")
    printf "%-12s %12s %12s %12s %12s %12s\n" "$w" \
        "$([ "$f" = n/a ] && echo n/a || fmt "$f")" \
        "$([ "$c" = n/a ] && echo n/a || fmt "$c")" \
        "$([ "$cpp" = n/a ] && echo n/a || fmt "$cpp")" \
        "$([ "$r" = n/a ] && echo n/a || fmt "$r")" \
        "$([ "$p" = n/a ] && echo n/a || fmt "$p")"
done
echo
echo "raw ns: $T"
