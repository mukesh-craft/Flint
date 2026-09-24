#!/usr/bin/env bash
# v1 release gate: runs the complete battery, fails on any red gate.
# Usage: bash tests/v1_gate_check.sh [path/to/flintc] [--full]
# Default runs fast gates; --full adds the slow corpus gates
# (parse-gate 83, fixpoint 83, lexdiff 82, stage3-ladder 21, tutorial).
set -u
FLINTC="${1:-./flintc}"
FULL=0
[ "${2:-}" = "--full" ] && FULL=1
pass=0; fail=0
gate() { # $1=name $2...=command...
  name="$1"; shift
  if "$@" > /dev/null 2>&1; then echo "GATE $name: pass"; pass=$((pass+1));
  else echo "GATE $name: FAIL"; fail=$((fail+1)); fi
}
gate parse-golden bash tests/test_parse.sh "$FLINTC"
gate emit-gate bash tests/test_emit.sh "$FLINTC"
gate smoke bash tests/run.sh "$FLINTC"
gate merge bash tests/test_merge.sh "$FLINTC"
gate registry bash tests/test_registry.sh "$FLINTC"
gate differential bash tests/test_differential.sh "$FLINTC"
gate driver bash tests/test_driver.sh "$FLINTC"
gate decl-order python3 tests/test_decl_order.py stage1/flint_lex.fl stage2/flint_parse.fl stage3/flint_emit.fl driver/flintc.fl tools/merge_sexp.fl
gate fmt bash tests/test_fmt.sh "$FLINTC"
gate errors bash tests/test_errors.sh
if [ "$FULL" = 1 ]; then
  gate parse-gate-83 bash tests/test_parse_gate.sh "$FLINTC"
  gate fixpoint-83 bash tests/test_fixpoint.sh "$FLINTC"
  gate lexdiff-82 bash tests/test_lexdiff.sh "$FLINTC"
  gate stage3-ladder bash tests/test_emit_stage3.sh "$FLINTC"
  gate tutorial bash tests/run_tutorial.sh "$FLINTC"
  gate sanitizers bash tests/test_sanitizers.sh "$FLINTC"
  gate opt-identity bash tests/test_opt_identity.sh "$FLINTC"
fi
echo "v1-gate: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
