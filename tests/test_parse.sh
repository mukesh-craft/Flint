#!/usr/bin/env bash
# Stage 2 golden tests: Flint parser output must match .sexp files exactly.
# Usage: bash tests/test_parse.sh [path/to/flintc]   (default: ./flintc)
set -u
FLINTC="${1:-./flintc}"
pass=0; fail=0
DIFFDIR="$(mktemp -d)"
# The golden corpus is the numbered p-cases (p2.sexp, p3.sexp). The glob is
# digit-anchored on purpose: `p*.fl` also matched stage2/parser.fl, whose
# parse has no golden (and is far too big to want one).
for f in stage2/p[0-9]*.fl; do
  base="${f%.fl}"
  "$FLINTC" stage2/parser.fl -- "$f" > "$DIFFDIR/got.txt" 2>/dev/null
  if [ $? -ne 0 ]; then echo "FAIL $f (nonzero exit)"; fail=$((fail+1)); continue; fi
  if diff -q "$base.sexp" "$DIFFDIR/got.txt" > /dev/null; then pass=$((pass+1));
  else echo "FAIL $f (golden diff)"; diff "$base.sexp" "$DIFFDIR/got.txt" | head -n 6; fail=$((fail+1)); fi
done
rm -rf "$DIFFDIR"
echo "parse-golden: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
