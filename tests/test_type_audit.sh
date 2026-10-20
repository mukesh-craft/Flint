#!/usr/bin/env bash
# Gate: the two inference engines must never break a CONTRACT.
#
# Flint has two type inference engines: the parser's (inferType) and the
# codegen's (exprFlintType and the declaration ladder). Phase 2 / 2A.0 added
# `flintc --type-audit`, which compares them at every declaration:
#
#   * an ANNOTATED declaration is a contract -- the program says what type
#     the value has, and the compiler must honour it. A disagreement here is
#     a BUG (the program does not have the type it claims).
#   * an INFERRED declaration is a guess -- a disagreement is DRIFT, which
#     the type-checker work (step 2A) will remove. It is reported, not
#     failed: the corpus currently has drift (measured: 186 declarations
#     corpus-wide, always the parse-time engine defaulting to i64 where
#     the codegen engine knows better).
#
# This gate fails on a contract violation and reports the drift.
#
# Usage: bash tests/test_type_audit.sh [path/to/flintc]  (default: ./flintc)
set -u
cd "$(dirname "$0")/.."
FLINTC="${1:-./flintc}"
T="${TMPDIR:-/data/data/com.termux/files/usr/tmp}/type_audit"
mkdir -p "$T"
pass=0; fail=0

files=$(find tests examples tutorial benchmarks -name "*.fl" 2>/dev/null | sort)
total=0
for f in $files; do
    out=$(timeout 120 "$FLINTC" --type-audit "$f" -o "$T/a.bin" 2>&1)
    if printf '%s\n' "$out" | grep -q "CONTRACT VIOLATED"; then
        echo "FAIL $f: the two inference engines broke an ANNOTATED declaration"
        printf '%s\n' "$out" | grep "CONTRACT VIOLATED" | head -3
        fail=$((fail+1))
        continue
    fi
    pass=$((pass+1))
    n=$(printf '%s\n' "$out" | awk '/type-audit: [0-9]+ declaration/ {print $2; exit}')
    case "$n" in ''|*[!0-9]*) ;; *) total=$((total + n)) ;; esac
done

echo "type-audit: $pass files with no contract violation, $fail failed"
echo "type-audit: $total inferred declaration(s) where the two engines still drift (informational)"
[ "$fail" -eq 0 ]
