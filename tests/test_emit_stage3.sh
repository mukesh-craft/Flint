#!/usr/bin/env bash
# Stage 3 golden ladder: .sexp -> Flint emitter -> diff golden -> llvm-as ->
# opt verify -> clang+runtime link -> run, exit must match manifest.
# Usage: bash tests/test_emit_stage3.sh [path/to/flintc]   (default: ./flintc)
set -u
FLINTC="${1:-./flintc}"
SELF_DIR="${SELF_DIR:-}"
EMIT_CMD=("$FLINTC" stage3/flint_emit.fl --)
if [ -n "$SELF_DIR" ] && [ -x "$SELF_DIR/emit.0" ]; then
  EMIT_CMD=("$SELF_DIR/emit.0")
fi
pass=0; fail=0
TMPD="/data/data/com.termux/files/usr/tmp/slipstream/ladder"
mkdir -p "$TMPD"
while read -r want name code; do
  case "$want" in
    \#*|"") continue ;;
  esac
  base="${name%.sexp}"
  # 1. emit
  if ! timeout -s KILL 120 "${EMIT_CMD[@]}" "stage3/ladder/$name" "$TMPD/$base.ll" > /dev/null 2>&1; then echo "FAIL $name (emit)"; fail=$((fail+1)); continue; fi
  # 2. diff golden
  if ! diff -q "stage3/ladder/$base.ll" "$TMPD/$base.ll" > /dev/null; then echo "FAIL $name (golden diff)"; fail=$((fail+1)); continue; fi
  if [ "$want" = "fail" ]; then echo "FAIL $name (expected fail, emitted ok)"; fail=$((fail+1)); continue; fi
  # 3. assemble + verify
  if ! llvm-as "$TMPD/$base.ll" -o "$TMPD/$base.bc" 2>/dev/null; then echo "FAIL $name (llvm-as)"; fail=$((fail+1)); continue; fi
  if ! opt -passes=verify "$TMPD/$base.bc" -o /dev/null 2>/dev/null; then echo "FAIL $name (verify)"; fail=$((fail+1)); continue; fi
  # 4. link + 5. run
  if ! clang "$TMPD/$base.ll" runtime/runtime.c -lm -o "$TMPD/$base" 2>/dev/null; then echo "FAIL $name (clang)"; fail=$((fail+1)); continue; fi
  "$TMPD/$base" > /dev/null 2>&1
  rc=$?
  if [ "$rc" -eq "$code" ]; then pass=$((pass+1));
  else echo "FAIL $name (exit $rc, want $code)"; fail=$((fail+1)); fi
done < stage3/ladder/manifest.txt
echo "stage3-ladder: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
