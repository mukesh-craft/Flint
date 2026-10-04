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

# ---- S5: UAF quarantine for str / map / chan ----
# Leases cover only explicit flint_aegis_* allocations, so a stale pointer to a
# freed string used to read whatever the allocator handed out next. With
# FLINT_AEGIS_QUARANTINE=1 the freed block is retained and poisoned, so the
# same read traps. Both behaviours are asserted: without the flag the program
# runs (that is the default fast path), with it the read must panic.

quarantine_traps() { # $1=name $2=src $3=expected-output-before-panic
  name="$1"; src="$2"; want="$3"
  printf '%s\n' "$src" > "$TMPD/$name.fl"
  if ! "$FLINTC" "$TMPD/$name.fl" -o "$TMPD/$name.bin" > "$TMPD/$name.err" 2>&1; then
    echo "FAIL $name: failed to compile: $(head -n1 "$TMPD/$name.err")"
    fail=$((fail+1)); return
  fi
  # No env var: the quarantine is on by default, so this asserts the default.
  out="$("$TMPD/$name.bin" 2>&1)"
  if printf '%s' "$out" | grep -q "use-after-free"; then
    if printf '%s' "$out" | grep -qF "$want"; then pass=$((pass+1));
    else echo "FAIL $name: trapped but output before it was wrong"; fail=$((fail+1)); fi
  else
    echo "FAIL $name: stale read did not trap: $(printf '%s' "$out" | head -n1)"
    fail=$((fail+1))
  fi
}

quarantine_traps uaf_str \
"fn main() -> i64 {
    s = flint_str_concat(\"ab\", \"cd\")
    flint_println(s)
    flint_str_free(s)
    print(flint_str_length(s))
    0
}" "abcd"

quarantine_traps uaf_map \
"fn main() -> i64 {
    m = flint_map_new()
    flint_map_set(m, \"k\", 7)
    flint_println(\"mapped\")
    flint_map_free(m)
    print(flint_map_get(m, \"k\"))
    0
}" "mapped"

quarantine_traps uaf_chan \
"fn main() -> i64 {
    ch = flint_chan_new(4)
    flint_chan_send(ch, 7)
    flint_println(\"sent\")
    flint_chan_free(ch)
    flint_chan_send(ch, 9)
    print(flint_chan_recv(ch))
    0
}" "sent"

# FLINT_AEGIS_QUARANTINE=0 is the documented opt-out and must still work.
accept ok_after_free \
"fn main() -> i64 {
    s = flint_str_concat(\"ab\", \"cd\")
    flint_println(s)
    flint_str_free(s)
    0
}"

# The documented opt-out must still work: quarantine off, the stale read
# completes (that is the point of the escape hatch) and valid code is fine.
optout_ok() { # $1=name $2=src
  name="$1"; src="$2"
  printf '%s\n' "$src" > "$TMPD/$name.fl"
  if ! "$FLINTC" "$TMPD/$name.fl" -o "$TMPD/$name.bin" > "$TMPD/$name.err" 2>&1; then
    echo "FAIL $name: failed to compile: $(head -n1 "$TMPD/$name.err")"
    fail=$((fail+1)); return
  fi
  if FLINT_AEGIS_QUARANTINE=0 "$TMPD/$name.bin" > /dev/null 2>&1; then
    pass=$((pass+1))
  else
    echo "FAIL $name: FLINT_AEGIS_QUARANTINE=0 should let the read through"
    fail=$((fail+1))
  fi
}

optout_ok optout_str \
"fn main() -> i64 {
    s = flint_str_concat(\"ab\", \"cd\")
    flint_println(s)
    flint_str_free(s)
    print(flint_str_length(s))
    0
}"

echo "aegis: $pass passed, $fail failed"
[ "$fail" -eq 0 ]