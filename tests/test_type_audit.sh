#!/usr/bin/env bash
# Gate: the type audit (Phase 2 / 2A.0) and the call facts (2A.1).
#
# Flint historically had TWO type inference engines: the parser's
# (inferType) and the codegen's (exprFlintType plus the declaration
# ladder). `flintc --type-audit` compares them at every declaration:
#
#   * an ANNOTATED declaration is a contract -- the program says what type
#     the value has, and the compiler must honour it. A disagreement is a
#     BUG (the program does not have the type it claims) and this gate
#     fails on it.
#   * an INFERRED declaration is a guess. A disagreement is DRIFT. The 2A
#     work removed the drift the audit measured (the tensor-literal shape,
#     the user-call signature table, the `-> ptr` pre-pass), so the gate
#     now requires ZERO drift: any new shape that drifts fails the build.
#
# It also requires the 2A.1 call facts to be printed: a generic call's
# instantiated type arguments, and a trait method's resolved implementation
# (the name AND the receiver type that name+arity dispatch selected).
#
# Files the compiler rejects for an unrelated reason (the deliberate
# borrow/move/aegis error demos) have no audit to compare; they are skipped
# here. tests/test_type_check.sh owns the accept/reject decision (the
# differential property) for those.
#
# Usage: bash tests/test_type_audit.sh [path/to/flintc]  (default: ./flintc)
set -u
cd "$(dirname "$0")/.."
FLINTC="${1:-./flintc}"
T="${TMPDIR:-/data/data/com.termux/files/usr/tmp}/type_audit"
mkdir -p "$T"
pass=0; fail=0; skipped=0; facts=0

# The whole tree: the audit's contract is corpus-wide, and the pipeline
# sources are the programs a missed drift would break first.
files=$(find . -name '*.fl' -not -path './.git/*' \
             -not -path './tests/typecheck/*' 2>/dev/null | sort)
for f in $files; do
    out=$(timeout 180 "$FLINTC" --type-audit "$f" -o "$T/a.ll" 2>&1)
    rc=$?
    if [ "$rc" -ne 0 ]; then
        skipped=$((skipped + 1))
        continue
    fi
    if printf '%s\n' "$out" | grep -q "CONTRACT VIOLATED"; then
        echo "FAIL $f: the two inference engines broke an ANNOTATED declaration"
        printf '%s\n' "$out" | grep "CONTRACT VIOLATED" | head -3
        fail=$((fail + 1))
        continue
    fi
    d=$(printf '%s\n' "$out" | grep -c 'parse=' || true)
    if [ "$d" -ne 0 ]; then
        echo "FAIL $f: $d inferred declaration(s) still drift between the engines"
        printf '%s\n' "$out" | grep 'parse=' | head -3
        fail=$((fail + 1))
        continue
    fi
    n=$(printf '%s\n' "$out" | awk '/type-audit: [0-9]+ call fact/ {print $2; exit}')
    case "$n" in ''|*[!0-9]*) n=0 ;; esac
    facts=$((facts + n))
    pass=$((pass + 1))
done

# The facts fixture in detail: one inferred generic call, one explicit
# generic call, one trait method call.
summary=$("$FLINTC" --type-audit tests/typecheck/facts.fl -o "$T/facts.ll" 2>&1)
if [ $? -ne 0 ]; then
    echo "FAIL tests/typecheck/facts.fl: did not compile"
    printf '%s\n' "$summary" | head -5
    fail=$((fail + 1))
else
    for want in "type-args=[i64]" "type-args=[f64]" "impl='area for Circle'"; do
        if printf '%s\n' "$summary" | grep -qF "$want"; then pass=$((pass + 1)); else
            echo "FAIL: --type-audit did not print '$want'"
            fail=$((fail + 1))
        fi
    done
fi

echo "type-audit: $pass checks passed, $skipped file(s) skipped (rejected for another reason), $fail failed"
echo "type-audit: $facts call fact(s) recorded across the corpus"
[ "$fail" -eq 0 ]
