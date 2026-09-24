#!/usr/bin/env bash
# fmt gate: canonical sources must be flint-fmt clean, formatting must be
# idempotent, and formatting must preserve stage-2 S-expr output exactly.
# Usage: bash tests/test_fmt.sh [path/to/flintc]   (default: ./flintc)
set -u
FLINTC="${1:-./flintc}"
SELF_DIR="${SELF_DIR:-}"
# Device note: JIT-parsing the stage sources per file OOMs the 3.6GB
# device (parser JIT-compile + run exceeds RAM). Use AOT parse.0 when
# provided, exactly like test_lexdiff/test_parse_gate/test_fixpoint.
if [ -n "$SELF_DIR" ] && [ -x "$SELF_DIR/parse.0" ]; then
  echo "using SELF_DIR $SELF_DIR for parse" >&2
  PARSE_BIN="$SELF_DIR/parse.0"
else
  PARSE_BIN=""
fi
CANON="stage1/flint_lex.fl stage2/flint_parse.fl stage3/flint_emit.fl driver/flintc.fl tools/merge_sexp.fl"
TMPD="/data/data/com.termux/files/usr/tmp/slipstream/fmtgate"
mkdir -p "$TMPD"
fail=0
# 1. --check clean
if python3 ./flint-fmt --check $CANON > /dev/null 2>&1; then echo "PASS fmt-check-clean";
else echo "FAIL fmt-check-clean"; fail=$((fail+1)); fi
# 2+3. idempotency + parse-stability per file
for f in $CANON; do
  b="$(basename "$f")"
  cp "$f" "$TMPD/$b.once.fl"
  python3 ./flint-fmt "$TMPD/$b.once.fl" > /dev/null 2>&1
  cp "$TMPD/$b.once.fl" "$TMPD/$b.twice.fl"
  python3 ./flint-fmt "$TMPD/$b.twice.fl" > /dev/null 2>&1
  if cmp -s "$TMPD/$b.once.fl" "$TMPD/$b.twice.fl"; then echo "PASS fmt-idem-$b";
  else echo "FAIL fmt-idem-$b"; fail=$((fail+1)); continue; fi
  if [ -n "${PARSE_BIN:-}" ]; then
    if ! timeout -s KILL 200 "$PARSE_BIN" "$f" > "$TMPD/$b.orig.sexp" 2>/dev/null; then echo "FAIL fmt-parse-orig-$b"; fail=$((fail+1)); continue; fi
    if ! timeout -s KILL 200 "$PARSE_BIN" "$TMPD/$b.once.fl" > "$TMPD/$b.fmt.sexp" 2>/dev/null; then echo "FAIL fmt-parse-fmt-$b"; fail=$((fail+1)); continue; fi
  else
    if ! timeout -s KILL 200 "$FLINTC" stage2/flint_parse.fl -- "$f" > "$TMPD/$b.orig.sexp" 2>/dev/null; then echo "FAIL fmt-parse-orig-$b"; fail=$((fail+1)); continue; fi
    if ! timeout -s KILL 200 "$FLINTC" stage2/flint_parse.fl -- "$TMPD/$b.once.fl" > "$TMPD/$b.fmt.sexp" 2>/dev/null; then echo "FAIL fmt-parse-fmt-$b"; fail=$((fail+1)); continue; fi
  fi
  if cmp -s "$TMPD/$b.orig.sexp" "$TMPD/$b.fmt.sexp"; then echo "PASS fmt-stable-$b";
  else echo "FAIL fmt-stable-$b"; fail=$((fail+1)); fi
done
echo "fmt-gate: failures=$fail"
[ "$fail" -eq 0 ]
