#!/usr/bin/env bash
# --target: cross-target triple control at the emit stage.
#
# Before this gate there was no argv way to change the module triple (it was
# hardcoded to the Android host in the Codegen constructor), so WASM/x86
# objects could not be emitted and the tiers table said so. The flag now
# rewrites triple + datalayout from a real TargetMachine; the canonical
# datalayout strings below are pinned because they are what clang 21 emits
# for those triples -- if LLVM changes a layout, this gate flags a real
# change, not a false positive.
#
# Verified behaviors:
#   1. host default emit is unchanged (triple + datalayout lines exact)
#   2. --target x86_64-pc-linux-gnu emits the canonical x86_64 layout
#   3. --target wasm32-unknown-wasi emits the canonical wasm layout
#   4. .o output produces a real object for the target (wasm magic bytes)
#   5. --target without -o refuses loudly (JIT is host-triple only)
# Usage: bash tests/test_target.sh [path/to/flintc]   (default: ./flintc)
set -u
FLINTC="${1:-./flintc}"
TMPBASE="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
TMPD="$TMPBASE/slipstream/target"
mkdir -p "$TMPD"
pass=0; fail=0

check() { # name expected actual
  if [ "$2" = "$3" ]; then pass=$((pass+1)); echo "PASS $1"; else fail=$((fail+1)); echo "FAIL $1"; echo "  want: $2"; echo "  got:  $3"; fi
}

"$FLINTC" tests/t_hello.fl --emit-llvm -o "$TMPD/host.ll" > /dev/null 2>&1
check "host triple" \
  'target triple = "aarch64-unknown-linux-android24"' \
  "$(grep 'target triple' "$TMPD/host.ll")"
check "host datalayout" \
  'target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128-Fn32"' \
  "$(grep 'target datalayout' "$TMPD/host.ll")"

"$FLINTC" tests/t_hello.fl --target x86_64-pc-linux-gnu --emit-llvm -o "$TMPD/x86.ll" > /dev/null 2>&1
check "x86 triple" \
  'target triple = "x86_64-pc-linux-gnu"' \
  "$(grep 'target triple' "$TMPD/x86.ll")"
check "x86 datalayout" \
  'target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"' \
  "$(grep 'target datalayout' "$TMPD/x86.ll")"

"$FLINTC" tests/t_hello.fl --target wasm32-unknown-wasi --emit-llvm -o "$TMPD/wasi.ll" > /dev/null 2>&1
check "wasi triple" \
  'target triple = "wasm32-unknown-wasi"' \
  "$(grep 'target triple' "$TMPD/wasi.ll")"
check "wasi datalayout" \
  'target datalayout = "e-m:e-p:32:32-p10:8:8-p20:8:8-i64:64-i128:128-n32:64-S128-ni:1:10:20"' \
  "$(grep 'target datalayout' "$TMPD/wasi.ll")"

"$FLINTC" tests/t_hello.fl --target wasm32-unknown-wasi --emit-llvm -o "$TMPD/wasi.o" > /dev/null 2>&1
magic="$(head -c 8 "$TMPD/wasi.o" | od -An -tx1 | tr -d ' \n')"
check "wasi object magic" "0061736d01000000" "$magic"

out="$("$FLINTC" tests/t_hello.fl --target x86_64-pc-linux-gnu 2>&1)"; rc=$?
check "no -o refuses (rc)" "1" "$rc"
if echo "$out" | grep -q 'requires an emit output'; then pass=$((pass+1)); echo "PASS no -o refuses (message)"; else fail=$((fail+1)); echo "FAIL no -o refuses (message)"; fi

echo "--target: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
