#!/usr/bin/env bash
# Compile-time benchmark: C++ flintc vs self-hosted pipeline, split by phase.
# Usage: bash bench/compile_time/bench.sh [path/to/flintc] [runs]
# Env: SELF_DIR with parse.0/emit.0 (default /tmp/slipstream/self).
# Prints CSV: input,bytes,cpp_ms,self_parse_ms,self_emit_ms,self_total_ms
# Corpus: tests/differential/common.fl (small), stage1/flint_lex.fl (large).
set -u
FLINTC="${1:-./flintc}"
RUNS="${2:-3}"
SELF_DIR="${SELF_DIR:-/data/data/com.termux/files/usr/tmp/slipstream/self}"
TMPD="/data/data/com.termux/files/usr/tmp/slipstream/benchct"
mkdir -p "$TMPD"
[ -x "$SELF_DIR/parse.0" ] && [ -x "$SELF_DIR/emit.0" ] \
  || { echo "MISSING self binaries (build via tests/test_selfhost.sh A)"; exit 1; }

ms() { # $1=start_ns $2=end_ns
  echo $(( ($2 - $1) / 1000000 ))
}
bench_one() { # $1=label $2=src
  src="$2"; bytes=$(wc -c < "$src")
  cpp_sum=0; pp_sum=0; pe_sum=0
  for _ in $(seq 1 "$RUNS"); do
    s=$(date +%s%N)
    timeout -s KILL 900 "$FLINTC" "$src" --emit-llvm -o "$TMPD/b.ll" > /dev/null 2>&1
    e=$(date +%s%N); cpp_sum=$((cpp_sum + (e - s)))
    s=$(date +%s%N)
    "$SELF_DIR/parse.0" "$src" > "$TMPD/b.sexp" 2>/dev/null
    e=$(date +%s%N); pp_sum=$((pp_sum + (e - s)))
    s=$(date +%s%N)
    "$SELF_DIR/emit.0" "$TMPD/b.sexp" "$TMPD/b.self.ll" > /dev/null 2>&1
    e=$(date +%s%N); pe_sum=$((pe_sum + (e - s)))
  done
  n=$RUNS
  echo "$1,$bytes,$((cpp_sum/n/1000000)),$((pp_sum/n/1000000)),$((pe_sum/n/1000000)),$(((pp_sum+pe_sum)/n/1000000))"
}
echo "input,bytes,cpp_ms,self_parse_ms,self_emit_ms,self_total_ms"
bench_one small tests/differential/common.fl
bench_one lex stage1/flint_lex.fl
