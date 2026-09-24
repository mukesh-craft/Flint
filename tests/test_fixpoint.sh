#!/usr/bin/env bash
# Stage 2 fixpoint (AOT): parse -> pretty-print -> re-lex must yield
# identical kind+value token streams. Builds the parser once natively, then
# runs the binary per file (fresh process = clean globals; JIT-compile
# per file would dominate runtime).
# Same exclusions as test_parse_gate.sh (syntax-invalid files).
# Usage: bash tests/test_fixpoint.sh [path/to/flintc]   (default: ./flintc)
set -u
FLINTC="${1:-./flintc}"
SELF_DIR="${SELF_DIR:-}"
if [ -n "$SELF_DIR" ] && [ -x "$SELF_DIR/parse.0" ] && [ -x "$SELF_DIR/lex.0" ]; then
  echo "using SELF_DIR $SELF_DIR for parse/lex" >&2
fi
mkdir -p /data/data/com.termux/files/usr/tmp/slipstream
FXBIN="/data/data/com.termux/files/usr/tmp/slipstream/fxbin"
if [ -n "$SELF_DIR" ] && [ -x "$SELF_DIR/parse.0" ]; then
  : # SELF_DIR provides parse.0; skip slow AOT build (C++ syntax has no `build` subcommand)
else
  "$FLINTC" stage2/flint_parse.fl -o "$FXBIN" > /dev/null 2>&1
  if [ $? -ne 0 ]; then echo "fixpoint: AOT build failed"; exit 1; fi
fi
pass=0; fail=""
for f in $(find tests tutorial benchmarks examples stage1 stage2 agent-bench -name "*.fl" | grep -v "stage1/lex_edge.fl\|stage1/lex_err.fl" | sort); do
  if [ -n "$SELF_DIR" ] && [ -x "$SELF_DIR/parse.0" ]; then
    timeout -s KILL 120 "$SELF_DIR/parse.0" --fixpoint "$f" > /dev/null 2>&1
  else
    timeout -s KILL 120 "$FXBIN" --fixpoint "$f" > /dev/null 2>&1
  fi
  rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); else fail="$fail $f($rc)"; fi
done
echo "fixpoint pass=$pass"
if [ -n "$fail" ]; then echo "nonzero:$fail"; exit 1; fi
