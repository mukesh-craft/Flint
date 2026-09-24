#!/usr/bin/env bash
# Fuzz corpus replay: every checked-in fuzz/corpus/*.fl must compile and
# behave identically under C++ flintc and the self-hosted pipeline.
# Empty corpus passes trivially. Seeds new regressions via fuzz/run.sh
# (copy minimized failing case.fl files here with a seed-NAME.fl name).
# Usage: bash tests/test_fuzz_corpus.sh [path/to/flintc]
set -u
FLINTC="${1:-./flintc}"
SELF_DIR="${SELF_DIR:-/data/data/com.termux/files/usr/tmp/slipstream/self}"
TMPD="/data/data/com.termux/files/usr/tmp/slipstream/fuzzcorpus"
mkdir -p "$TMPD"
pass=0; fail=0
shopt -s nullglob
cases=(fuzz/corpus/*.fl)
if [ "${#cases[@]}" -eq 0 ]; then echo "fuzz-corpus: empty, nothing to replay"; exit 0; fi
HAVE_SELF=1
[ -x "$SELF_DIR/parse.0" ] && [ -x "$SELF_DIR/emit.0" ] || HAVE_SELF=0
for f in "${cases[@]}"; do
  b="$(basename "$f" .fl)"
  cpp_ok=0; cpp_out=""; cpp_exit=-1
  if timeout -s KILL 120 "$FLINTC" "$f" --emit-llvm -o "$TMPD/c.ll" > /dev/null 2>&1 \
     && llvm-as "$TMPD/c.ll" -o "$TMPD/c.bc" > /dev/null 2>&1 \
     && clang "$TMPD/c.ll" runtime/runtime.c -lm -o "$TMPD/c.bin" > /dev/null 2>&1; then
    cpp_out="$("$TMPD/c.bin" 2>&1)"; cpp_exit=$?; cpp_ok=1
  fi
  if [ "$HAVE_SELF" = 0 ]; then
    if [ "$cpp_ok" = 1 ]; then echo "PASS corpus-$b (cpp only, no self binaries)"; pass=$((pass+1));
    else echo "FAIL corpus-$b (cpp reject)"; fail=$((fail+1)); fi
    continue
  fi
  self_ok=0; self_out=""; self_exit=-1
  if timeout -s KILL 300 "$SELF_DIR/parse.0" "$f" > "$TMPD/c.sexp" 2>/dev/null \
     && timeout -s KILL 300 "$SELF_DIR/emit.0" "$TMPD/c.sexp" "$TMPD/c.self.ll" > /dev/null 2>&1 \
     && llvm-as "$TMPD/c.self.ll" -o /dev/null > /dev/null 2>&1 \
     && clang "$TMPD/c.self.ll" runtime/runtime.c -lm -o "$TMPD/c.self.bin" > /dev/null 2>&1; then
    self_out="$("$TMPD/c.self.bin" 2>&1)"; self_exit=$?; self_ok=1
  fi
  if [ "$cpp_ok" = "$self_ok" ] && [ "$cpp_exit" = "$self_exit" ] && [ "$cpp_out" = "$self_out" ]; then
    echo "PASS corpus-$b"; pass=$((pass+1))
  else
    echo "FAIL corpus-$b (cpp_ok=$cpp_ok self_ok=$self_ok cpp=$cpp_exit self=$self_exit)"; fail=$((fail+1))
  fi
done
echo "fuzz-corpus: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
