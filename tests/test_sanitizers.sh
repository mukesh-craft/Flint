#!/usr/bin/env bash
# Sanitizer gate (S2): build runnable Flint programs with
# -fsanitize=address,undefined and run them. Fails on any sanitizer
# finding. Leak detection is OFF (no GC yet — leaks are a separate
# story, not memory safety); OOM panics are expected-clean.
# Expected-fail programs (panic tests) must print their flint PANIC
# with no sanitizer ERROR lines.
# Usage: bash tests/test_sanitizers.sh [path/to/flintc]   (default: ./flintc)
set -u
FLINTC="${1:-./flintc}"
TMPD="/data/data/com.termux/files/usr/tmp/slipstream/sanitizers"
mkdir -p "$TMPD"
export ASAN_OPTIONS=detect_leaks=0
export UBSAN_OPTIONS=print_stacktrace=1
pass=0; fail=0
build_run() { # $1=label $2=fl $3=want_exit $4=want_grep(optional)
  label="$1"; src="$2"; want="$3"; wantgrep="${4:-}"
  if ! timeout -s KILL 300 "$FLINTC" "$src" --emit-llvm -o "$TMPD/t.ll" > /dev/null 2>&1; then
    echo "FAIL $label (emit)"; fail=$((fail+1)); return
  fi
  if ! clang -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
      "$TMPD/t.ll" runtime/runtime.c -lm -o "$TMPD/t.bin" 2> "$TMPD/build.err"; then
    echo "FAIL $label (sanitizer build)"; head -n 3 "$TMPD/build.err"; fail=$((fail+1)); return
  fi
  out="$("$TMPD/t.bin" 2> "$TMPD/run.err")"; rc=$?
  if grep -q "ERROR: AddressSanitizer\|runtime error:" "$TMPD/run.err"; then
    echo "FAIL $label (sanitizer finding)"; head -n 6 "$TMPD/run.err"; fail=$((fail+1)); return
  fi
  if [ "$rc" -ne "$want" ]; then
    echo "FAIL $label (exit $rc, want $want)"; fail=$((fail+1)); return
  fi
  if [ -n "$wantgrep" ] && ! printf '%s' "$out" | grep -qF -- "$wantgrep"; then
    echo "FAIL $label (output mismatch)"; fail=$((fail+1)); return
  fi
  echo "PASS $label"; pass=$((pass+1))
}
# Memory-heavy but finite: arrays, strings, sieve, concat builder.
build_run "sum_array" benchmarks/sum_array.fl 0 "sum: 99999990000000"
build_run "primes" benchmarks/primes.fl 0 "664579"
build_run "pi" benchmarks/pi.fl 0 "pi = 3.14159"
build_run "strrev" benchmarks/strrev.fl 0 "checksum: 5044012"
build_run "fib2" benchmarks/fib2.fl 0 "55"
build_run "strings-ex" examples/strings.fl 0 ""
build_run "arrays-ex" examples/arrays.fl 0 ""
# Expected panics: flint PANIC (abort) with the right message on stderr
# and no sanitizer ERROR lines.
panic_run() { # $1=label $2=fl $3=message
  label="$1"; src="$2"; msg="$3"
  if ! timeout -s KILL 300 "$FLINTC" "$src" --emit-llvm -o "$TMPD/t.ll" > /dev/null 2>&1; then
    echo "FAIL $label (emit)"; fail=$((fail+1)); return
  fi
  if ! clang -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
      "$TMPD/t.ll" runtime/runtime.c -lm -o "$TMPD/t.bin" 2> "$TMPD/build.err"; then
    echo "FAIL $label (sanitizer build)"; head -n 3 "$TMPD/build.err"; fail=$((fail+1)); return
  fi
  "$TMPD/t.bin" > /dev/null 2> "$TMPD/run.err"; rc=$?
  if grep -q "ERROR: AddressSanitizer\|runtime error:" "$TMPD/run.err"; then
    echo "FAIL $label (sanitizer finding)"; head -n 6 "$TMPD/run.err"; fail=$((fail+1)); return
  fi
  if [ "$rc" -eq 0 ]; then
    echo "FAIL $label (expected panic abort, exited 0)"; fail=$((fail+1)); return
  fi
  if ! grep -qF -- "$msg" "$TMPD/run.err"; then
    echo "FAIL $label (panic message missing)"; head -n 3 "$TMPD/run.err"; fail=$((fail+1)); return
  fi
  echo "PASS $label"; pass=$((pass+1))
}
printf 'fn main() -> i64 {\n    a = [10, 20, 30]\n    print(a[5])\n    0\n}\n' > "$TMPD/oob.fl"
panic_run "oob-panic" "$TMPD/oob.fl" "index 5 out of bounds"
printf 'fn main() -> i64 {\n    a = [10, 20, 30]\n    print(a[0 - 1])\n    0\n}\n' > "$TMPD/neg.fl"
panic_run "neg-panic" "$TMPD/neg.fl" "index -1 out of bounds"
printf 'fn f(n: i64) -> i64 {\n    if n <= 1 {\n        n\n    } else {\n        f(n - 1) + f(n - 2)\n    }\n}\nfn main() -> i64 {\n    print(f(30))\n    0\n}\n' > "$TMPD/fib30.fl"
build_run "fib30" "$TMPD/fib30.fl" 0 "832040"
echo "sanitizers: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
