#!/usr/bin/env bash
# Differential test: C++ flintc vs self-hosted pipeline must agree.
# See COMPATIBILITY.md. Fails on any UNLISTED divergence.
# Usage: bash tests/test_differential.sh [path/to/flintc]
# Env: SELF_DIR with parse.0/emit.0 (default $TMPDIR/slipstream/self);
# C2 hermetic tmp: $TMPDIR honored; fallback is the historical path (unchanged default).
#      FLINT_JIT=1 falls back to JIT (slow: ~2 min per stage per file).
set -u
FLINTC="${1:-./flintc}"
_TMPBASE="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
SELF_DIR="${SELF_DIR:-$_TMPBASE/slipstream/self}"
# The tmp copy is cleaned by the system from time to time, and then this
# gate fails with "self binaries" for a reason that has nothing to do with
# the code. The STABLE self-host artifacts (built by tests/test_selfhost.sh)
# are used as a fallback.
if [ ! -x "$SELF_DIR/parse.0" ] && [ -x /data/data/com.termux/files/home/flintwork/self/parse.0 ]; then
    # Only use the stable artifacts if they are NEWER than the pipeline
    # sources. Stale artifacts produce FALSE divergences (the pipeline of
    # last month rejects what today's pipeline accepts), which looks like a
    # bug in the compiler and hides real ones.
    STABLE=/data/data/com.termux/files/home/flintwork/self
    stable_ok=1
    for src in stage2/flint_parse.fl stage3/flint_emit.fl stage1/flint_lex.fl; do
        if [ "$src" -nt "$STABLE/parse.0" ] 2>/dev/null; then stable_ok=0; fi
    done
    if [ "$stable_ok" = 1 ]; then
        SELF_DIR="$STABLE"
    else
        echo "differential: the stable self-host artifacts are OLDER than the pipeline sources;" >&2
        echo "            build fresh ones with: bash tests/test_selfhost.sh A" >&2
    fi
fi
TMPD="$_TMPBASE/slipstream/diff"
mkdir -p "$TMPD"
pass=0; fail=0; known=0

run_cpp() { # $1=src $2=outbase ; echoes "emit_ok run_exit"
  base="$2"
  # Link flint_std.o as well: the array primitives live in
  # runtime/rust/src/array.rs now, so an emit that calls flint_array_* cannot
  # link against runtime.c alone. This is the same object build.sh links, so
  # the gate tests the artifact that actually ships rather than a subset.
  if timeout -s KILL 300 "$FLINTC" "$1" --emit-llvm -o "$TMPD/$base.cpp.ll" > /dev/null 2>&1 \
     && llvm-as "$TMPD/$base.cpp.ll" -o "$TMPD/$base.cpp.bc" > /dev/null 2>&1 \
     && clang "$TMPD/$base.cpp.ll" runtime/runtime.c runtime/flint_aegis.c flint_std.o -lsqlite3 -lcurl -lm -o "$TMPD/$base.cpp" > /dev/null 2>&1; then
    "$TMPD/$base.cpp" > "$TMPD/$base.cpp.out" 2>&1
    echo "1 $?"
  else
    echo "0 -1"
  fi
}

run_self() { # $1=src $2=outbase ; echoes "emit_ok run_exit"
  base="$2"
  if [ "${FLINT_JIT:-0}" = 1 ]; then
    PARSE=(timeout -s KILL 600 "$FLINTC" stage2/flint_parse.fl --)
    EMIT=(timeout -s KILL 600 "$FLINTC" stage3/flint_emit.fl --)
  else
    [ -x "$SELF_DIR/parse.0" ] && [ -x "$SELF_DIR/emit.0" ] \
      || { echo "MISSING self binaries (build via tests/test_selfhost.sh A or FLINT_JIT=1)"; echo "0 -1"; return; }
    PARSE=("$SELF_DIR/parse.0")
    EMIT=("$SELF_DIR/emit.0")
  fi
  if "${PARSE[@]}" "$1" > "$TMPD/$base.sexp" 2>/dev/null \
     && "${EMIT[@]}" "$TMPD/$base.sexp" "$TMPD/$base.self.ll" > /dev/null 2>&1 \
     && llvm-as "$TMPD/$base.self.ll" -o "$TMPD/$base.self.bc" > /dev/null 2>&1 \
     && clang "$TMPD/$base.self.ll" runtime/runtime.c runtime/flint_aegis.c -lm -o "$TMPD/$base.self" > /dev/null 2>&1; then
    "$TMPD/$base.self" > "$TMPD/$base.self.out" 2>&1
    echo "1 $?"
  else
    echo "0 -1"
  fi
}

while read -r name exit want allow; do
  case "$name" in \#*|"") continue ;; esac
  base="${name%.fl}"
  read -r cpp_ok cpp_exit < <(run_cpp "tests/differential/$name" "$base")
  read -r self_ok self_exit < <(run_self "tests/differential/$name" "$base")
  if [ "$cpp_ok" = 0 ] && [ "$self_ok" = 0 ]; then
    echo "PASS $name (both reject; allow=$allow)"; pass=$((pass+1)); continue
  fi
  if [ "$cpp_ok" = 0 ] || [ "$self_ok" = 0 ]; then
    if [ "$allow" != same ]; then
      echo "PASS $name (cpp_ok=$cpp_ok self_ok=$self_ok, allow=$allow)"; pass=$((pass+1)); known=$((known+1)); continue
    fi
    echo "FAIL $name (one-side reject: cpp_ok=$cpp_ok self_ok=$self_ok)"; fail=$((fail+1)); continue
  fi
  agree=1
  [ "$cpp_exit" = "$self_exit" ] || agree=0
  cmp -s "$TMPD/$base.cpp.out" "$TMPD/$base.self.out" || agree=0
  [ "$self_exit" = "$exit" ] || agree=0
  if [ "$want" != "-" ]; then
    cmp -s "$TMPD/$base.self.out" "tests/differential/$want" || agree=0
  fi
  if [ "$agree" = 1 ]; then
    echo "PASS $name"; pass=$((pass+1)); continue
  fi
  if [ "$allow" = cpp_may_differ ] && [ "$self_exit" = "$exit" ] \
     && { [ "$want" = "-" ] || cmp -s "$TMPD/$base.self.out" "tests/differential/$want"; }; then
    echo "PASS $name (known C++ divergence, self correct)"; pass=$((pass+1)); known=$((known+1)); continue
  fi
  echo "FAIL $name (cpp_exit=$cpp_exit self_exit=$self_exit)"; fail=$((fail+1))
done < tests/differential/manifest.txt
echo "differential: $pass passed, $fail failed ($known known-divergence)"
[ "$fail" -eq 0 ]
