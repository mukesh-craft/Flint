#!/usr/bin/env bash
# 2E explicit-type-argument gate: `f::<T>(...)` in the self-hosted pipeline.
#
# What must hold:
#   - stage2 lexes `::` as two ':' tokens (no lexer change) and emits
#     `(call-t NAME (targs T*) ARGS...)`;
#   - stage3 resolves the list with the ANNOTATION mapping and skips
#     inference, producing the C++ compiler's mangled instance name;
#   - the program's output matches the C++ compiler's (the differential
#     gate also covers this fixture -- this gate pins the failure modes);
#   - a wrong count, a non-generic callee, and a non-concrete type are
#     REJECTED with E0295-E0298: the pipeline may never be more permissive
#     than the C++ compiler.
#
# Usage: bash tests/test_explicit_targs.sh [path/to/flintc]
# Env: SELF_DIR with parse.0/emit.0 (default $TMPDIR/slipstream/self)
set -u
FLINTC="${1:-./flintc}"
_TMPBASE="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
SELF_DIR="${SELF_DIR:-$_TMPBASE/slipstream/self}"
TMPD="$_TMPBASE/slipstream/explicit"
mkdir -p "$TMPD"
P="$SELF_DIR/parse.0"
E="$SELF_DIR/emit.0"
pass=0; fail=0
ok()  { pass=$((pass+1)); }
bad() { fail=$((fail+1)); echo "FAIL explicit-targs ($1)"; shift; for line in "$@"; do echo "  $line"; done; }

[ -x "$FLINTC" ] || { echo "MISSING compiler ($FLINTC)"; exit 1; }
[ -x "$P" ] && [ -x "$E" ] \
  || { echo "MISSING self binaries (build via tests/test_selfhost.sh A)"; exit 1; }

pipeline() { # <src> <base>
  "$P" "$1" > "$2.sexp" 2> "$2.parse.log" || return 1
  "$E" "$2.sexp" "$2.ll" > "$2.emit.log" 2>&1 || return 1
}
rejected_with() { # <name> <src> <code>
  name="$1"; src="$2"; want="$3"
  if pipeline "$src" "$TMPD/$name"; then
    bad "$name: rejected" "the pipeline emitted it"
    return
  fi
  if grep -q "$want" "$TMPD/$name.emit.log"; then
    ok
  else
    bad "$name: diagnostic" "wanted $want, got:" "$(head -3 "$TMPD/$name.emit.log")"
  fi
}

# ---- 1. the fixture runs and prints the C++ compiler's output ----
if pipeline tests/differential/t_explicit_targs.fl "$TMPD/ok" \
   && clang "$TMPD/ok.ll" runtime/runtime.c runtime/flint_aegis.c -lm \
        -o "$TMPD/ok.bin" > "$TMPD/ok.link.log" 2>&1; then
  "$TMPD/ok.bin" > "$TMPD/ok.out" 2>&1
  if cmp -s "$TMPD/ok.out" tests/differential/t_explicit_targs.txt; then ok; else
    bad "fixture output" "$(cat "$TMPD/ok.out")"
  fi
else
  bad "fixture pipeline" "$(head -3 "$TMPD/ok.emit.log" 2>/dev/null)"
fi

# ---- 2. the mangled instance name is the C++ compiler's ----
if grep -q '@idc\.i64' "$TMPD/ok.ll" && grep -q '@"idc.s:S"' "$TMPD/ok.ll"; then
  ok
else
  bad "mangled instances" "$(grep -o '@[^ (]*idc[^ (]*' "$TMPD/ok.ll" | sort -u)"
fi

# ---- 3. a wrong explicit count is refused ----
cat > "$TMPD/count.fl" <<'EOF'
fn idc<T>(x: T) -> T { return x }
fn main() { print(idc::<i64, i64>(5)) 0 }
EOF
rejected_with count "$TMPD/count.fl" 'E0296'

# ---- 4. explicit arguments on a non-generic callee are refused ----
cat > "$TMPD/nongen.fl" <<'EOF'
fn add(a: i64, b: i64) -> i64 { return a + b }
fn main() { print(add::<i64>(1, 2)) 0 }
EOF
rejected_with nongen "$TMPD/nongen.fl" 'E0298'

# ---- 5. a type parameter is not a concrete explicit argument ----
cat > "$TMPD/tparam.fl" <<'EOF'
fn idc<T>(x: T) -> T { return x }
fn first<A>(a: A) -> A { return idc::<A>(a) }
fn main() { print(first(3)) 0 }
EOF
rejected_with tparam "$TMPD/tparam.fl" 'E0297'

# ---- 6. an unknown type name is refused ----
cat > "$TMPD/unknown.fl" <<'EOF'
fn idc<T>(x: T) -> T { return x }
fn main() { print(idc::<Nope>(5)) 0 }
EOF
rejected_with unknown "$TMPD/unknown.fl" 'E0297'

echo "explicit-targs: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
