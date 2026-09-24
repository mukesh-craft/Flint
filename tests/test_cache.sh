#!/usr/bin/env bash
# A1: cache-salt + import-sidecar behavior (no flintc rebuild needed).
# Proves: (1) safe vs --unsafe builds use distinct cache entries;
# (2) changing an imported file busts the cache (no stale reuse).
# Usage: bash tests/test_cache.sh [path/to/flintc]   (default: ./flintc)
set -u
FLINTC="${1:-./flintc}"
T="$(mktemp -d 2>/dev/null || echo /tmp/flint-cachetest-$$)"
trap 'rm -rf "$T"' EXIT
export HOME="$T/home" # isolate the module cache ($HOME/.cache/flintc)
mkdir -p "$HOME" "$T/app"
fail=0
expect() { # $1=desc $2=expected $3=actual
    if [ "$2" = "$3" ]; then echo "ok: $1";
    else echo "FAIL: $1 (want [$2] got [$3])"; fail=$((fail+1)); fi
}

printf 'fn main() -> i64 {\n    print(7)\n    0\n}\n' > "$T/app/main.fl"

# 1. flags salt: safe and --unsafe builds must not share a cache entry.
timeout -s KILL 300 "$FLINTC" "$T/app/main.fl" -o "$T/app/safe.bin" > /dev/null 2>&1
[ -x "$T/app/safe.bin" ] || { echo "FAIL: safe build"; fail=$((fail+1)); }
nbin1="$(ls "$HOME/.cache/flintc/"*.bin 2>/dev/null | wc -l | tr -d ' ')"
timeout -s KILL 300 "$FLINTC" --unsafe "$T/app/main.fl" -o "$T/app/unsafe.bin" > /dev/null 2>&1
[ -x "$T/app/unsafe.bin" ] || { echo "FAIL: unsafe build"; fail=$((fail+1)); }
nbin2="$(ls "$HOME/.cache/flintc/"*.bin 2>/dev/null | wc -l | tr -d ' ')"
expect "safe/unsafe use distinct cache entries" "$((nbin1 + 1))" "$nbin2"
expect "safe runs" "7" "$("$T/app/safe.bin" 2>/dev/null | tail -n 1)"
expect "unsafe runs" "7" "$("$T/app/unsafe.bin" 2>/dev/null | tail -n 1)"

# 2. import sidecar: editing an import must change the next build's output.
printf 'fn helper() -> i64 {\n    40\n}\n' > "$T/app/helper.fl"
printf 'import "./helper.fl"\nfn main() -> i64 {\n    print(helper() + 2)\n    0\n}\n' > "$T/app/use.fl"
timeout -s KILL 300 "$FLINTC" "$T/app/use.fl" -o "$T/app/u1.bin" > /dev/null 2>&1
expect "import build v1 prints 42" "42" "$("$T/app/u1.bin" 2>/dev/null | tail -n 1)"
[ "$(ls "$HOME/.cache/flintc/"*.imports 2>/dev/null | wc -l | tr -d ' ')" -ge 1 ] \
    && echo "ok: sidecar written" || { echo "FAIL: no .imports sidecar"; fail=$((fail+1)); }
printf 'fn helper() -> i64 {\n    41\n}\n' > "$T/app/helper.fl"
timeout -s KILL 300 "$FLINTC" "$T/app/use.fl" -o "$T/app/u2.bin" > /dev/null 2>&1
expect "edited import rebuilds (no stale reuse)" "43" "$("$T/app/u2.bin" 2>/dev/null | tail -n 1)"

echo "cache: failures=$fail"
[ "$fail" -eq 0 ]
