#!/usr/bin/env bash
# v1 release gate: runs the complete battery, fails on any red gate.
# Usage: bash tests/v1_gate_check.sh [path/to/flintc] [--full]
# Default runs fast gates; --full adds the slow corpus gates
# (parse-gate, fixpoint, lexdiff, stage3-ladder, tutorial, sanitizers,
# opt-identity, type-check, type-audit).
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
gate signatures bash tests/test_signatures.sh "$FLINTC"
gate driver bash tests/test_driver.sh "$FLINTC"
if [ -x tools/rust/target/release/flint-tool ]; then
  gate decl-order tools/rust/target/release/flint-tool decl-order stage1/lexer.fl stage2/parser.fl stage3/emitter.fl driver/flintc.fl tools/merge_sexp.fl
else
  gate decl-order python3 tests/test_decl_order.py stage1/lexer.fl stage2/parser.fl stage3/emitter.fl driver/flintc.fl tools/merge_sexp.fl
fi
gate fmt bash tests/test_fmt.sh "$FLINTC"
gate errors bash tests/test_errors.sh
gate explicit-targs bash tests/test_explicit_targs.sh "$FLINTC"
gate closure-escape bash tests/test_closure_escape.sh "$FLINTC"
# The safety gates belong to the default battery: they are the contract that
# a check is only removed when it is proven unnecessary.
gate bounds-safety bash tests/test_bounds_safety.sh "$FLINTC"
gate unsafe-flag bash tests/test_unsafe_flag.sh "$FLINTC"
gate range-facts bash tests/test_range_facts.sh "$FLINTC"
gate vectorize bash tests/test_vectorize.sh "$FLINTC"
gate input bash tests/test_input.sh "$FLINTC"
gate aegis bash tests/test_aegis.sh "$FLINTC"
gate stdlib bash tests/test_ai.sh "$FLINTC"
# The documentation and diagnostic-catalogue gates are fast and need no
# compiler run, so they belong to the default battery.
gate docs bash tests/test_docs.sh
gate errors-cpp bash tests/test_errors_cpp.sh
gate diagnostic-codes bash tests/test_diagnostic_codes.sh "$FLINTC"
if [ "$FULL" = 1 ]; then
  gate parse-gate bash tests/test_parse_gate.sh "$FLINTC"
  gate fixpoint bash tests/test_fixpoint.sh "$FLINTC"
  gate lexdiff bash tests/test_lexdiff.sh "$FLINTC"
  gate stage3-ladder bash tests/test_emit_stage3.sh "$FLINTC"
  gate tutorial bash tests/run_tutorial.sh "$FLINTC"
  gate sanitizers bash tests/test_sanitizers.sh "$FLINTC"
  gate opt-identity bash tests/test_opt_identity.sh "$FLINTC"
  # Phase 2 / 2A: the type-checking phase and the inference audit. Both walk
  # the whole corpus twice, so they are slow gates.
  gate type-check bash tests/test_type_check.sh "$FLINTC"
  gate type-audit bash tests/test_type_audit.sh "$FLINTC"
fi
echo "v1-gate: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
