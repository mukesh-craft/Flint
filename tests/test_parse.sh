#!/usr/bin/env bash
# Stage 2 golden tests: Flint parser output must match .sexp files exactly.
# Usage: bash tests/test_parse.sh [path/to/flintc]   (default: ./flintc)
set -u
FLINTC="${1:-./flintc}"
pass=0; fail=0
DIFFDIR="$(mktemp -d)"
for f in stage2/p*.fl; do
  base="${f%.fl}"
  "$FLINTC" stage2/flint_parse.fl -- "$f" > "$DIFFDIR/got.txt" 2>/dev/null
  if [ $? -ne 0 ]; then echo "FAIL $f (nonzero exit)"; fail=$((fail+1)); continue; fi
  if diff -q "$base.sexp" "$DIFFDIR/got.txt" > /dev/null; then pass=$((pass+1));
  else echo "FAIL $f (golden diff)"; diff "$base.sexp" "$DIFFDIR/got.txt" | head -n 6; fail=$((fail+1)); fi
done
rm -rf "$DIFFDIR"
echo "parse-golden: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
