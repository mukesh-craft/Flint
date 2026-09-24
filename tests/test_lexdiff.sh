#!/usr/bin/env bash
# Stage 1 differential test: C++ --dump-tokens vs Flint lexer (stage1/).
# Every repo .fl file must produce byte-identical token dumps.
# Usage: bash tests/test_lexdiff.sh [path/to/flintc]   (default: ./flintc)
set -u
FLINTC="${1:-./flintc}"
SELF_DIR="${SELF_DIR:-}"
if [ -n "$SELF_DIR" ] && [ -x "$SELF_DIR/parse.0" ] && [ -x "$SELF_DIR/lex.0" ]; then
  echo "using SELF_DIR $SELF_DIR for parse/lex" >&2
fi
pass=0; fail=0
DIFFDIR="$(mktemp -d)"
rm -rf ~/.cache/flintc
for f in $(find tests tutorial benchmarks examples stage1 agent-bench -name "*.fl" | sort); do
  # Contract is the TOKEN STREAM (exit codes deliberately differ on lex
  # errors: C++ exits 0, Flint lexer returns 1 — see memory.md Stage 1).
  # Crashes/hangs still fail: they produce no/short output, failing diff.
  "$FLINTC" --dump-tokens "$f" > "$DIFFDIR/cpp.txt" 2>/dev/null
  if [ -n "$SELF_DIR" ] && [ -x "$SELF_DIR/lex.0" ]; then
    "$SELF_DIR/lex.0" "$f" > "$DIFFDIR/fl.txt" 2>/dev/null
  else
    "$FLINTC" stage1/flint_lex.fl -- "$f" > "$DIFFDIR/fl.txt" 2>/dev/null
  fi
  if diff -q "$DIFFDIR/cpp.txt" "$DIFFDIR/fl.txt" > /dev/null; then pass=$((pass+1));
  else echo "FAIL $f (diff)"; fail=$((fail+1)); fi
done
rm -rf "$DIFFDIR"
echo "lexdiff: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
