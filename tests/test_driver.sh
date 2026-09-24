#!/usr/bin/env bash
# Driver test: build driver/flintc.fl + matrix (single/multi/emit-llvm/errors).
# Usage: bash tests/test_driver.sh [path/to/flintc]   (default: ./flintc)
# Env: SELF_DIR with parse.0/emit.0 (default /tmp/slipstream/self).
set -u
FLINTC="${1:-./flintc}"
SELF_DIR="${SELF_DIR:-/data/data/com.termux/files/usr/tmp/slipstream/self}"
TMPD="/data/data/com.termux/files/usr/tmp/slipstream/driver"
mkdir -p "$TMPD"
pass=0; fail=0; skip=0
[ -x "$SELF_DIR/parse.0" ] && [ -x "$SELF_DIR/emit.0" ] \
  || { echo "MISSING self binaries (build via tests/test_selfhost.sh A)"; exit 1; }

if ! timeout -s KILL 600 "$FLINTC" driver/flintc.fl -o "$TMPD/driver.bin" > /dev/null 2>&1; then echo "FAIL driver (build)"; exit 1; fi
if ! timeout -s KILL 600 "$FLINTC" tools/merge_sexp.fl -o "$TMPD/merge_sexp.bin" > /dev/null 2>&1; then echo "FAIL driver (merge build)"; exit 1; fi
D="$TMPD/driver.bin"; P="$SELF_DIR/parse.0"; E="$SELF_DIR/emit.0"; M="$TMPD/merge_sexp.bin"

# 1. single-file build+run
if timeout -s KILL 500 "$D" tests/differential/common.fl --parse "$P" --emit "$E" --merge "$M" --rt runtime -o "$TMPD/c.bin" > "$TMPD/c.out" 2>&1 \
   && grep -q "^done$" "$TMPD/c.out"; then pass=$((pass+1)); else echo "FAIL driver (single)"; fail=$((fail+1)); fi
# 2. multi-file with import
if timeout -s KILL 500 "$D" tests/fixtures/driver/main.fl --parse "$P" --emit "$E" --merge "$M" --rt runtime -o "$TMPD/m.bin" --no-run > /dev/null 2>&1 \
   && [ "$("$TMPD/m.bin")" = 42 ]; then pass=$((pass+1)); else echo "FAIL driver (multi)"; fail=$((fail+1)); fi
# 3. --emit-llvm produces assemblable IR
if timeout -s KILL 500 "$D" tests/differential/common.fl --parse "$P" --emit "$E" --merge "$M" --rt runtime --emit-llvm -o "$TMPD/c.ll" > /dev/null 2>&1 \
   && llvm-as "$TMPD/c.ll" -o /dev/null 2>/dev/null; then pass=$((pass+1)); else echo "FAIL driver (emit-llvm)"; fail=$((fail+1)); fi
# 4. error paths exit nonzero
if "$D" > /dev/null 2>&1; then echo "FAIL driver (noargs ok)"; fail=$((fail+1)); else pass=$((pass+1)); fi
if "$D" /nonexistent.fl --parse "$P" > /dev/null 2>&1; then echo "FAIL driver (missing ok)"; fail=$((fail+1)); else pass=$((pass+1)); fi
# 5. --self-hosted uses emit.1 when present
if [ -x "$SELF_DIR/emit.1" ]; then
  if timeout -s KILL 500 "$D" tests/differential/common.fl --parse "$P" --emit "$E" --merge "$M" --rt runtime --self-hosted -o "$TMPD/s.bin" --no-run > /dev/null 2>&1 \
     && [ "$("$TMPD/s.bin" | tail -n 1)" = done ]; then pass=$((pass+1)); else echo "FAIL driver (self-hosted)"; fail=$((fail+1)); fi
else
  echo "SKIP driver (self-hosted; no emit.1)"; skip=$((skip+1))
fi
echo "driver: $pass passed, $fail failed, $skip skipped"
[ "$fail" -eq 0 ]
