#!/usr/bin/env bash
# Benchmark the CLI's one-command path against the old two-step workflow.
#
#   new: flintc main.fl                     (compile + run via ORC JIT)
#   old: flintc main.fl -o app && ./app     (compile + link + run)
#
# The AOT side clears the module cache before every build so the number is
# a cold build, which is what a user gets the first time. JIT never reads
# the module cache, so its number is already cold by construction.
#
# Usage: bash tools/bench_cli_run.sh [path/to/flintc]
# Report medians; absolute times are machine-specific (STATE.md §8).
set -u
cd "$(dirname "$0")/.." || exit 1
FLINTC="${1:-./flintc}"
T="${TMPDIR:-/data/data/com.termux/files/usr/tmp}/flint-cli-bench.$$"
rm -rf "$T"
mkdir -p "$T/home"
export HOME="$T/home" # isolate the module cache so the AOT rows are cold
trap 'rm -rf "$T"' EXIT

cat > "$T/hello.fl" <<'EOF'
fn main() -> i64 {
    print("hello")
    0
}
EOF

cat > "$T/loop.fl" <<'EOF'
fn main() -> i64 {
    mut s: i64 = 0
    mut i: i64 = 0
    while i < 200000000 {
        s = s + i % 7
        i = i + 1
    }
    print(s)
    0
}
EOF

median() { sort -n | awk '{a[NR]=$1} END {print a[int((NR+1)/2)]}'; }
now_ms() { echo $(( $(date +%s%N) / 1000000 )); }

bench_jit() { # $1=source
    local s e
    s=$(now_ms)
    "$FLINTC" "$1" > /dev/null 2>&1
    e=$(now_ms)
    echo $((e - s))
}

bench_aot() { # $1=source $2=binary
    local s e
    rm -rf "$T/home/.cache/flintc"
    s=$(now_ms)
    "$FLINTC" "$1" -o "$2" > /dev/null 2>&1 && "$2" > /dev/null 2>&1
    e=$(now_ms)
    echo $((e - s))
}

printf 'workload   new: flintc f.fl   old: flintc f.fl -o app && app   ratio (old/new)\n'
for name in hello loop; do
    jit=""
    aot=""
    # Alternate the two paths so thermal drift hits both.
    for i in 1 2 3 4 5; do
        jit="$jit $(bench_jit "$T/$name.fl")"
        aot="$aot $(bench_aot "$T/$name.fl" "$T/$name.bin")"
    done
    jm=$(printf '%s\n' $jit | median)
    am=$(printf '%s\n' $aot | median)
    ratio=$(awk -v a="$am" -v j="$jm" 'BEGIN { if (j > 0) printf "%.2fx", a/j; else print "n/a" }')
    printf '%-10s %8s ms          %8s ms                         %s\n' "$name" "$jm" "$am" "$ratio"
done
