#!/usr/bin/env bash
# 2E.5 signatures gate: the C++ compiler's query mode
# (`flintc --emit-signatures`) and the self-hosted emitter's overlay.
# See docs/PIPELINE-GENERICS.md §5.
#
# What must hold:
#   - the query file has the documented format and is deterministic;
#   - an untyped parameter is resolved from its CALL SITES (the measured
#     miscompile: `fn show(x)` called with a str used to print the pointer);
#   - unlisted functions keep their scanned signature (partial file = safe);
#   - templates keep their type parameters and mangled instances;
#   - the C++ compiler and the pipeline agree on the fixtures (differential);
#   - a bad file fails loudly with E0284-E0289, never silently.
#
# Usage: bash tests/test_signatures.sh [path/to/flintc]
# Env: SELF_DIR with parse.0/emit.0 (default $TMPDIR/slipstream/self)
# C2 hermetic tmp: $TMPDIR honored; fallback is the historical path.
set -u
FLINTC="${1:-./flintc}"
_TMPBASE="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
SELF_DIR="${SELF_DIR:-$_TMPBASE/slipstream/self}"
TMPD="$_TMPBASE/slipstream/sig"
mkdir -p "$TMPD"
P="$SELF_DIR/parse.0"
E="$SELF_DIR/emit.0"
pass=0; fail=0
ok()  { pass=$((pass+1)); }
bad() { fail=$((fail+1)); echo "FAIL signatures ($1)"; shift; for line in "$@"; do echo "  $line"; done; }

[ -x "$FLINTC" ] || { echo "MISSING compiler ($FLINTC)"; exit 1; }
[ -x "$P" ] && [ -x "$E" ] \
  || { echo "MISSING self binaries (build via tests/test_selfhost.sh A)"; exit 1; }
[ -f tests/signatures/untyped.fl ] && [ -f tests/signatures/generic.fl ] \
  || { echo "MISSING tests/signatures fixtures"; exit 1; }

# pipeline <src> <base> [sigfile]: parse + emit; 0 only if both succeed.
pipeline() {
  "$P" "$1" > "$2.sexp" 2> "$2.parse.log" || return 1
  if [ $# -ge 3 ]; then
    "$E" "$2.sexp" "$2.ll" --signatures "$3" > "$2.emit.log" 2>&1 || return 1
  else
    "$E" "$2.sexp" "$2.ll" > "$2.emit.log" 2>&1 || return 1
  fi
}
# linkrun <base>: link base.ll against the runtime and run it into base.out.
linkrun() {
  clang "$1.ll" runtime/runtime.c runtime/flint_aegis.c -lm -o "$1.bin" \
    > "$1.link.log" 2>&1 || return 1
  "$1.bin" > "$1.out" 2>&1
}

# ---- 1. the query file's content (untyped param resolved from the call) ----
if "$FLINTC" --emit-signatures tests/signatures/untyped.fl "$TMPD/untyped.sig" \
     > "$TMPD/untyped.q.log" 2>&1; then
  printf 'flint-signatures 1\nfn show () (str) -> i64\nfn main () () -> i64\n' \
    > "$TMPD/untyped.want"
  if cmp -s "$TMPD/untyped.sig" "$TMPD/untyped.want"; then ok; else
    bad "untyped file content" "want:" "$(cat "$TMPD/untyped.want")" \
        "got:" "$(cat "$TMPD/untyped.sig" 2>/dev/null)"
  fi
else
  bad "untyped query run" "$(cat "$TMPD/untyped.q.log")"
fi

# ---- 2. determinism: the same input gives the same bytes ----
if "$FLINTC" --emit-signatures tests/signatures/untyped.fl "$TMPD/untyped2.sig" \
     > /dev/null 2>&1 && cmp -s "$TMPD/untyped.sig" "$TMPD/untyped2.sig"; then
  ok
else
  bad "determinism" "two runs differ"
fi

# ---- 3. templates: type parameters and type words survive ----
if "$FLINTC" --emit-signatures tests/signatures/generic.fl "$TMPD/generic.sig" \
     > "$TMPD/generic.q.log" 2>&1; then
  printf 'flint-signatures 1\nfn idc (T) (T) -> T\nfn pair (A,B) (A,B) -> i64\nfn take () (Inner) -> i64\nfn main () () -> i64\n' \
    > "$TMPD/generic.want"
  if cmp -s "$TMPD/generic.sig" "$TMPD/generic.want"; then ok; else
    bad "generic file content" "want:" "$(cat "$TMPD/generic.want")" \
        "got:" "$(cat "$TMPD/generic.sig" 2>/dev/null)"
  fi
else
  bad "generic query run" "$(cat "$TMPD/generic.q.log")"
fi

# ---- 4. the measured drift is fixed through the pipeline ----------------
# `show("hi")` used to print the pointer's decimal value: the S-expression
# says the parameter is untyped (`-`) and the emitter defaulted it to i64.
# The query file says `str`, so the emitted signature takes a pointer and
# the runtime prints the string.
if pipeline tests/signatures/untyped.fl "$TMPD/u" "$TMPD/untyped.sig" \
   && grep -q 'define i64 @show(ptr %x.arg)' "$TMPD/u.ll" \
   && linkrun "$TMPD/u" && [ "$(cat "$TMPD/u.out")" = hi ]; then
  ok
else
  bad "untyped overlay end to end" \
      "ir: $(grep 'define.*@show' "$TMPD/u.ll" 2>/dev/null)" \
      "out: $(cat "$TMPD/u.out" 2>/dev/null)" \
      "emit: $(tail -n 3 "$TMPD/u.emit.log" 2>/dev/null | tr '\n' ' ')"
fi

# ---- 5. the control: without the file the drift is still there -----------
# This is the honest baseline, not a second bug: it proves the overlay is
# what changes the answer, not something else in this test.
if pipeline tests/signatures/untyped.fl "$TMPD/unosig" \
   && grep -q 'define i64 @show(i64 %x.arg)' "$TMPD/unosig.ll" \
   && linkrun "$TMPD/unosig" && [ "$(cat "$TMPD/unosig.out")" != hi ]; then
  ok
else
  bad "control (no signature file)" \
      "ir: $(grep 'define.*@show' "$TMPD/unosig.ll" 2>/dev/null)" \
      "out: $(cat "$TMPD/unosig.out" 2>/dev/null)"
fi

# ---- 6. a partial file is safe: unlisted functions keep their scan ------
printf 'flint-signatures 1\nfn show () (str) -> i64\n' > "$TMPD/partial.sig"
if pipeline tests/signatures/untyped.fl "$TMPD/up" "$TMPD/partial.sig" \
   && grep -q 'define i64 @show(ptr %x.arg)' "$TMPD/up.ll" \
   && grep -q 'i32 @main(' "$TMPD/up.ll" \
   && linkrun "$TMPD/up" && [ "$(cat "$TMPD/up.out")" = hi ]; then
  ok
else
  bad "partial overlay" \
      "ir: $(grep 'define.*@' "$TMPD/up.ll" 2>/dev/null | head -n 2 | tr '\n' ' ')" \
      "out: $(cat "$TMPD/up.out" 2>/dev/null)"
fi

# ---- 7. templates through the pipeline (and the C++ oracle) -------------
# Same program, both frontends: the instance symbols and the output must
# agree. The C++ run is the oracle, so a pipeline-only "fix" cannot pass.
if "$FLINTC" tests/signatures/generic.fl --no-run -o "$TMPD/generic.cpp.bin" \
     > "$TMPD/generic.cpp.log" 2>&1 \
   && "$TMPD/generic.cpp.bin" > "$TMPD/generic.cpp.out" 2>&1 \
   && pipeline tests/signatures/generic.fl "$TMPD/g" "$TMPD/generic.sig" \
   && grep -q 'define i64 @idc.i64' "$TMPD/g.ll" \
   && grep -q 'define ptr @idc.str' "$TMPD/g.ll" \
   && linkrun "$TMPD/g" \
   && cmp -s "$TMPD/g.out" "$TMPD/generic.cpp.out" \
   && [ "$(cat "$TMPD/g.out")" = "$(printf '5\n3\n7')" ]; then
  ok
else
  bad "generic differential pair" \
      "cpp: $(cat "$TMPD/generic.cpp.out" 2>/dev/null | tr '\n' ' ')" \
      "self: $(cat "$TMPD/g.out" 2>/dev/null | tr '\n' ' ')" \
      "emit: $(tail -n 3 "$TMPD/g.emit.log" 2>/dev/null | tr '\n' ' ')"
fi

# ---- 8. loud failures: E0284-E0289, one shape each ----------------------
pipeline tests/signatures/untyped.fl "$TMPD/err" > /dev/null 2>&1 \
  || { echo "cannot build the error fixture"; exit 1; }
printf 'flint-signatures 2\n'                                                   > "$TMPD/e0285.sig"
printf 'flint-signatures 1\nfn show () (str) i64\n'                             > "$TMPD/e0286.sig"
printf 'flint-signatures 1\nfn show () () -> i64\nfn main () () -> i64\n'       > "$TMPD/e0287.sig"
printf 'flint-signatures 1\nfn show (T) (str) -> i64\nfn main () () -> i64\n'   > "$TMPD/e0288.sig"
printf 'flint-signatures 1\nfn show () ([f64]) -> i64\nfn main () () -> i64\n'  > "$TMPD/e0289.sig"
err_case() { # code sigfile
  out="$("$E" "$TMPD/err.sexp" "$TMPD/err.ll" --signatures "$2" 2>&1)"
  rc=$?
  if [ "$rc" -ne 0 ] && printf '%s' "$out" | grep -q "$1" \
     && ! printf '%s' "$out" | grep -q 'E0167'; then
    ok
  else
    bad "$1" "rc=$rc" "$(printf '%s' "$out" | head -n 4 | tr '\n' ' ')"
  fi
}
if out="$("$E" "$TMPD/err.sexp" "$TMPD/err.ll" --signatures "$TMPD/no-such.sig" 2>&1)" \
   && ! printf '%s' "$out" | grep -q 'E0284'; then
  bad "E0284" "a missing file was accepted"
else
  printf '%s' "$out" | grep -q 'E0284' && ok || bad "E0284" "$out"
fi
err_case E0285 "$TMPD/e0285.sig"
err_case E0286 "$TMPD/e0286.sig"
err_case E0287 "$TMPD/e0287.sig"
err_case E0288 "$TMPD/e0288.sig"
err_case E0289 "$TMPD/e0289.sig"

# ---- 9. the compiler's own source: the real untyped param resolves ------
# stage3/emitter.fl has exactly one untyped parameter (`sb` in
# e_unescape_step); querying the compiler with itself must say `str`.
if "$FLINTC" --emit-signatures stage3/emitter.fl "$TMPD/emitter.sig" \
     > "$TMPD/emitter.q.log" 2>&1 \
   && grep -q '^fn e_unescape_step () (str,str,i64,i64) -> i64$' "$TMPD/emitter.sig"; then
  ok
else
  bad "emitter source: e_unescape_step(sb, ...) resolves sb to str" \
      "$(grep 'e_unescape_step' "$TMPD/emitter.sig" 2>/dev/null)" \
      "$(tail -n 3 "$TMPD/emitter.q.log" 2>/dev/null)"
fi

echo "signatures: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
