#!/usr/bin/env bash
# AEGIS static pass: each documented compile-time reject must be produced,
# and valid lease code must still compile. These are the errors
# runtime/flint_aegis.h documents as "compile-time rejects"; before
# AegisChecker was restored all of them were runtime-only panics.
# Usage: bash tests/test_aegis.sh [path/to/flintc]   (default: ./flintc)
set -u
FLINTC="${1:-./flintc}"
_TMPBASE="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
TMPD="$_TMPBASE/slipstream/aegis"
mkdir -p "$TMPD"
pass=0; fail=0

# trap <name> <expected-substring> <source> -- must compile, then panic at run
trap() {
  name="$1"; want="$2"; src="$3"
  printf '%s\n' "$src" > "$TMPD/$name.fl"
  if ! "$FLINTC" "$TMPD/$name.fl" -o "$TMPD/$name.bin" > "$TMPD/$name.err" 2>&1; then
    echo "FAIL $name: failed to compile: $(head -n1 "$TMPD/$name.err")"
    fail=$((fail+1)); return
  fi
  out="$("$TMPD/$name.bin" 2>&1)"
  if printf '%s' "$out" | grep -qF "$want"; then
    pass=$((pass+1))
  else
    echo "FAIL $name: expected trap '$want', got: $(printf '%s' "$out" | head -n1)"
    fail=$((fail+1))
  fi
}

# reject <name> <expected-substring> <source>
reject() {
  name="$1"; want="$2"; src="$3"
  printf '%s\n' "$src" > "$TMPD/$name.fl"
  out="$("$FLINTC" "$TMPD/$name.fl" --emit-llvm -o "$TMPD/$name.ll" 2>&1)"
  if printf '%s' "$out" | grep -qF "$want"; then
    pass=$((pass+1))
  else
    echo "FAIL $name: expected '$want', got: $(printf '%s' "$out" | head -n1)"
    fail=$((fail+1))
  fi
}

# accept <name> <source> -- must compile cleanly
accept() {
  name="$1"; src="$2"
  printf '%s\n' "$src" > "$TMPD/$name.fl"
  if "$FLINTC" "$TMPD/$name.fl" --emit-llvm -o "$TMPD/$name.ll" > "$TMPD/$name.err" 2>&1; then
    pass=$((pass+1))
  else
    echo "FAIL $name: valid code rejected: $(head -n1 "$TMPD/$name.err")"
    fail=$((fail+1))
  fi
}

ALLOC='a = flint_aegis_alloc(64)'

reject uaf "aegis_uaf" \
"fn main() -> i64 {
    $ALLOC
    flint_aegis_write_i64(a, 0, 5)
    flint_aegis_free(a)
    print(flint_aegis_read_i64(a, 0))
    0
}"

reject double_free "aegis_double_free" \
"fn main() -> i64 {
    $ALLOC
    flint_aegis_free(a)
    flint_aegis_free(a)
    0
}"

reject branch_uaf "aegis_branch_uaf" \
"fn main(c: i64) -> i64 {
    $ALLOC
    if c == 1 {
        flint_aegis_free(a)
        print(flint_aegis_read_i64(a, 0))
    } else {
        print(flint_aegis_read_i64(a, 1))
    }
    0
}"

reject move_twice "moved twice" \
"fn main() -> i64 {
    $ALLOC
    b = flint_aegis_move(a)
    c = flint_aegis_move(a)
    0
}"

reject use_after_move "used after move" \
"fn main() -> i64 {
    $ALLOC
    b = flint_aegis_move(a)
    print(flint_aegis_read_i64(a, 0))
    0
}"

reject free_while_borrowed "while it is borrowed" \
"fn main() -> i64 {
    $ALLOC
    c = flint_aegis_borrow(a)
    flint_aegis_free(a)
    0
}"

# Valid: alloc, write, read, move, borrow/unborrow, free, and the documented
# weak-reference query on a freed lease (which must NOT be a use-after-free).
accept ok_basic \
"fn main() -> i64 {
    $ALLOC
    flint_aegis_write_i64(a, 0, 42)
    print(flint_aegis_read_i64(a, 0))
    flint_println(\"\")
    b = flint_aegis_move(a)
    print(flint_aegis_len(b))
    flint_println(\"\")
    c = flint_aegis_borrow(b)
    print(flint_aegis_read_i64(c, 0))
    flint_println(\"\")
    flint_aegis_unborrow(c)
    flint_aegis_free(b)
    print(flint_aegis_check(b))
    flint_println(\"\")
    0
}"

accept ok_branch_free \
"fn main(c: i64) -> i64 {
    $ALLOC
    if c == 1 {
        flint_aegis_free(a)
    } else {
        flint_aegis_free(a)
    }
    0
}"

# Integer division/modulo by zero is UB in LLVM, not a panic: `100 / 0`
# silently produced a garbage exit status. Both must trap.
trap div_zero "division by zero" \
"fn main() -> i64 {
    z = 0
    print(100 / z)
    0
}"

trap mod_zero "division by zero" \
"fn main() -> i64 {
    print(5 % 0)
    0
}"

accept ok_div \
"fn main() -> i64 {
    a = 84
    b = 2
    print(a / b)
    print(a % b)
    print(a / 7)
    0
}"

echo "aegis: $pass passed, $fail failed"
[ "$fail" -eq 0 ]