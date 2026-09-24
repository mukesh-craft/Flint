#!/usr/bin/env bash
# Stage 2 corpus gate: every valid repo .fl file must parse with exit 0.
# Exclusions (both fail in C++ too — verified):
#   stage1/lex_edge.fl — token-valid but syntax-invalid (`=>` in expr)
#   stage1/lex_err.fl  — lexer errors (parse driver rejects before parsing)
# Usage: bash tests/test_parse_gate.sh [path/to/flintc]   (default: ./flintc)
set -u
FLINTC="${1:-./flintc}"
SELF_DIR="${SELF_DIR:-}"
if [ -n "$SELF_DIR" ] && [ -x "$SELF_DIR/parse.0" ] && [ -x "$SELF_DIR/lex.0" ]; then
  echo "using SELF_DIR $SELF_DIR for parse/lex" >&2
fi
pass=0; fail=""
for f in $(find tests tutorial benchmarks examples stage1 stage2 agent-bench -name "*.fl" | grep -v "stage1/lex_edge.fl\|stage1/lex_err.fl" | sort); do
  if [ -n "$SELF_DIR" ] && [ -x "$SELF_DIR/parse.0" ]; then
    timeout -s KILL 60 "$SELF_DIR/parse.0" "$f" > /dev/null 2>&1
  else
    timeout -s KILL 60 "$FLINTC" stage2/flint_parse.fl -- "$f" > /dev/null 2>&1
  fi
  rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); else fail="$fail $f($rc)"; fi
done
echo "parse-gate pass=$pass"
if [ -n "$fail" ]; then echo "nonzero:$fail"; exit 1; fi
