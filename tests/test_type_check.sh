#!/usr/bin/env bash
# Gate: Phase 2 / 2A -- the type-checking compiler phase.
#
# Three properties are checked:
#
#   1. every provable type error is rejected with the checker's OWN
#      structured diagnostic (E0290-E0294), before codegen: no LLVM
#      "function verification failed" text, no output artifact;
#   2. valid programs are accepted unchanged -- including the i64<->ptr
#      handle convention (`cell: ptr = flint_atomic_new(0)`, which the
#      backend stores as an opaque pointer);
#   3. the DIFFERENTIAL property over the corpus: a program's accept/reject
#      decision must be the same with the checker and with --no-type-check.
#      The checker may MISS an error, never invent one. A mismatch here is
#      a false positive and fails the gate.
#
# The 2A.1 facts (instantiated type arguments, resolved trait impls) are
# gated by tests/test_type_audit.sh, which owns the audit mode.
#
# Usage: bash tests/test_type_check.sh [path/to/flintc]  (default: ./flintc)
set -u
cd "$(dirname "$0")/.."
FLINTC="${1:-./flintc}"
T="${TMPDIR:-/data/data/com.termux/files/usr/tmp}/type_check"
mkdir -p "$T"
pass=0; fail=0

ok()  { pass=$((pass + 1)); }
bad() { fail=$((fail + 1)); echo "FAIL: $*"; }

# --- 1. the checker rejects, with its own code -----------------------------

expect_reject() { # <file> <code>
    local f="$1" code="$2" out rc
    rm -f "$T/out.ll"
    out=$("$FLINTC" "$f" -o "$T/out.ll" 2>&1); rc=$?
    if [ "$rc" -eq 0 ]; then
        bad "$f: accepted (expected $code)"; return
    fi
    if ! printf '%s\n' "$out" | grep -q "$code"; then
        bad "$f: did not report $code"
        printf '%s\n' "$out" | head -5
        return
    fi
    if printf '%s\n' "$out" | grep -q 'function verification failed'; then
        bad "$f: the checker did not stop it; LLVM's verifier did"
        return
    fi
    if [ -e "$T/out.ll" ]; then
        bad "$f: wrote an output artifact despite the type error"
        return
    fi
    ok
}

expect_reject tests/typecheck/e0290_str_i64.fl E0290
expect_reject tests/typecheck/e0290_f64_i64.fl E0290
expect_reject tests/typecheck/e0290_i64_f64.fl E0290
expect_reject tests/typecheck/e0291_str_i64.fl E0291
expect_reject tests/typecheck/e0291_i64_f64.fl E0291
expect_reject tests/typecheck/e0292_assign.fl E0292
expect_reject tests/typecheck/e0293_arg.fl E0293
expect_reject tests/typecheck/e0294_cmp.fl E0294

# --- 2. valid programs pass -------------------------------------------------

expect_accept() { # <file>
    local f="$1" out rc
    rm -f "$T/out.ll"
    out=$("$FLINTC" "$f" -o "$T/out.ll" 2>&1); rc=$?
    if [ "$rc" -ne 0 ]; then
        bad "$f: rejected a valid program"
        printf '%s\n' "$out" | head -6
        return
    fi
    ok
}

expect_accept tests/typecheck/ok_ptr_handle.fl
expect_accept tests/typecheck/ok_valid.fl
expect_accept tests/typecheck/ok_mixed_cmp_cast.fl
expect_accept tests/typecheck/facts.fl

# --- 3. differential: accept/reject must not depend on the checker ----------

# EVERY .fl in the tree, including the pipeline sources (stage1-3, driver,
# tools) and the benchmark tasks. A checker false positive on a pipeline
# source breaks the bootstrap, and this gate is the only place that would
# see it.
files=$(find . -name '*.fl' -not -path './.git/*' \
             -not -path './tests/typecheck/*' 2>/dev/null | sort)
for f in $files; do
    timeout 180 "$FLINTC" --no-type-check "$f" -o "$T/n.ll" >/dev/null 2>&1; n=$?
    timeout 180 "$FLINTC" "$f" -o "$T/c.ll" >/dev/null 2>&1; c=$?
    if { [ "$n" -eq 0 ] && [ "$c" -ne 0 ]; } || { [ "$n" -ne 0 ] && [ "$c" -eq 0 ]; }; then
        bad "differential: $f (--no-type-check=$n, checker=$c)"
    else
        ok
    fi
done

echo "type-check: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
