#!/usr/bin/env bash
# Differential fuzzer: grammar-generated programs vs C++ and self-hosted.
# Usage: bash fuzz/run.sh [start_seed] [count] [path/to/flintc]
# Env: SELF_DIR with parse.0/emit.0 (required; build via test_selfhost.sh A).
# Classifies per seed: both-run-compare / both-reject-ok / C++-only-bug (save)
# / self-bug (save + FAIL). Timeouts catch hangs (a historic bug class).
set -u
START="${1:-1}"
COUNT="${2:-20}"
FLINTC="${3:-./flintc}"
SELF_DIR="${SELF_DIR:-/data/data/com.termux/files/usr/tmp/slipstream/self}"
TMPD="/data/data/com.termux/files/usr/tmp/slipstream/fuzz"
FAILDIR="fuzz/failures"
mkdir -p "$TMPD"
[ -x "$SELF_DIR/parse.0" ] && [ -x "$SELF_DIR/emit.0" ] \
  || { echo "MISSING self binaries (build via tests/test_selfhost.sh A)"; exit 1; }
pass=0; selfbug=0; cppbug=0
END=$((START + COUNT - 1))
for seed in $(seq "$START" "$END"); do
  f="$TMPD/fz_$seed.fl"
  python3 fuzz/generate.py "$seed" > "$f" 2>/dev/null || { echo "seed $seed GEN-FAIL"; continue; }
  # C++ side
  cpp_ok=0; cpp_out=""; cpp_exit=-1
  if timeout -s KILL 120 "$FLINTC" "$f" --emit-llvm -o "$TMPD/fz.ll" > /dev/null 2>&1 \
     && llvm-as "$TMPD/fz.ll" -o "$TMPD/fz.bc" > /dev/null 2>&1 \
     && clang "$TMPD/fz.ll" runtime/runtime.c -lm -o "$TMPD/fz.bin" > /dev/null 2>&1; then
    cpp_out="$("$TMPD/fz.bin" 2>&1)"; cpp_exit=$?; cpp_ok=1
  fi
  # Self side (timeout catches codegen hangs)
  self_ok=0; self_out=""; self_exit=-1
  if timeout -s KILL 300 "$SELF_DIR/parse.0" "$f" > "$TMPD/fz.sexp" 2>/dev/null \
     && timeout -s KILL 300 "$SELF_DIR/emit.0" "$TMPD/fz.sexp" "$TMPD/fz.self.ll" > /dev/null 2>&1 \
     && llvm-as "$TMPD/fz.self.ll" -o /dev/null > /dev/null 2>&1 \
     && clang "$TMPD/fz.self.ll" runtime/runtime.c -lm -o "$TMPD/fz.self.bin" > /dev/null 2>&1; then
    self_out="$("$TMPD/fz.self.bin" 2>&1)"; self_exit=$?; self_ok=1
  fi
  save() { # $1=kind
    d="$FAILDIR/seed-$seed-$1"
    mkdir -p "$d"
    cp "$f" "$d/case.fl"
    echo "cpp_ok=$cpp_ok cpp_exit=$cpp_exit self_ok=$self_ok self_exit=$self_exit" > "$d/result.txt"
    printf '%s' "$cpp_out" > "$d/cpp.out"
    printf '%s' "$self_out" > "$d/self.out"
    echo "SAVED $d"
  }
  if [ "$cpp_ok" = 0 ] && [ "$self_ok" = 0 ]; then pass=$((pass+1)); continue; fi
  if [ "$self_ok" = 0 ]; then save "self-bug"; selfbug=$((selfbug+1)); continue; fi
  if [ "$cpp_ok" = 0 ]; then save "cpp-bug"; cppbug=$((cppbug+1)); continue; fi
  if [ "$cpp_exit" = "$self_exit" ] && [ "$cpp_out" = "$self_out" ]; then pass=$((pass+1)); continue; fi
  save "self-bug"; selfbug=$((selfbug+1))
done
echo "fuzz: $pass clean, $selfbug self-bug, $cppbug cpp-bug"
[ "$selfbug" -eq 0 ]
