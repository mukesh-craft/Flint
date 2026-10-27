#!/usr/bin/env bash
# Gate: secure line input (flint_input / flint_input_i64 / flint_input_f64).
#
# The contract under test:
#   * one call consumes exactly one line, including CRLF;
#   * EOF before any byte is status 1, not a silent empty string;
#   * a numeric parse either consumes the whole line or reports 5/6 and
#     returns 0 (no partial value leaks to the caller);
#   * an over-long line is capped at 1 MiB and the rest is drained, status 2;
#   * an embedded NUL is skipped and reported as status 7;
#   * a truncated line does not poison the next call.
#
# Usage: bash tests/test_input.sh [path/to/flintc]   (default: ./flintc)
set -u
FLINTC="${1:-./flintc}"
T="${TMPDIR:-/data/data/com.termux/files/usr/tmp}/flint-input-gate"
rm -rf "$T"
mkdir -p "$T"

cat > "$T/probe.fl" <<'EOF'
fn main() -> i64 {
    line = flint_input("")
    print(flint_str_length(line))
    print(flint_input_status())
    n = flint_input_i64("")
    print(n)
    print(flint_input_status())
    x = flint_input_f64("")
    flint_println_f64(x)
    print(flint_input_status())
    0
}
EOF

"$FLINTC" "$T/probe.fl" --no-run -o "$T/probe" > "$T/build.log" 2>&1
if [ $? -ne 0 ]; then
  echo "FAIL: probe did not compile"; cat "$T/build.log"; exit 1
fi

pass=0; fail=0
check() { # name expected actual
  if [ "$2" = "$3" ]; then
    pass=$((pass+1))
  else
    fail=$((fail+1))
    echo "FAIL: $1"
    echo "  expected: $(printf '%s' "$2" | tr '\n' '|')"
    echo "  actual:   $(printf '%s' "$3" | tr '\n' '|')"
  fi
}

out=$(printf 'Alice\n42\n3.5\n' | "$T/probe")
check "basic values" "5
0
42
0
3.5
0" "$out"

out=$(printf 'Alice\r\n42\r\n3.5\r\n' | "$T/probe")
check "CRLF is stripped" "5
0
42
0
3.5
0" "$out"

out=$(printf 'Alice\nabc\n3.5\n' | "$T/probe")
check "invalid i64 is status 5 and value 0" "5
0
0
5
3.5
0" "$out"

out=$(printf 'Alice\n999999999999999999999\n3.5\n' | "$T/probe")
check "overflowing i64 is status 6 and value 0" "5
0
0
6
3.5
0" "$out"

out=$(printf 'Alice\n42\nnot-a-number\n' | "$T/probe")
check "invalid f64 is status 5 and value 0" "5
0
42
0
0
5" "$out"

out=$(printf 'a\0b\n42\n3.5\n' | "$T/probe")
check "embedded NUL is skipped and reported" "2
7
42
0
3.5
0" "$out"

# 1,100,000 bytes: keep the first 1 MiB, drain the rest, then read 42/3.5.
python3 -c 'print("x"*1100000); print("42"); print("3.5")' > "$T/long.txt"
out=$("$T/probe" < "$T/long.txt")
check "over-long line is capped and drained" "1048576
2
42
0
3.5
0" "$out"

# EOF before any byte (all three calls see EOF).
out=$(printf '' | "$T/probe")
check "EOF is status 1" "0
1
0
1
0
1" "$out"

echo "test_input: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
