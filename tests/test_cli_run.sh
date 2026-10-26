#!/usr/bin/env bash
# CLI one-command contract.
#
#   flintc main.fl          compiles AND runs in one step (ORC JIT, no artifact)
#   flintc main.fl -o app   builds ./app and NEVER runs it
#   flintc main.fl --run    explicit compile+run (the default)
#   flintc main.fl -o app --run  -o still wins: build only, order-independent
#
# It also pins the failure contract: a compile error exits nonzero, and a
# program's exit status is the compiler's exit status. Existing .ll/.o emit
# modes are checked so the new default cannot silently swallow them.
#
# Usage: bash tests/test_cli_run.sh [path/to/flintc]   (default: ./flintc)
set -u
FLINTC="${1:-./flintc}"
_TMPBASE="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
T="$(mktemp -d "$_TMPBASE/flint-cli.XXXXXX" 2>/dev/null || echo "$_TMPBASE/flint-cli.$$")"
trap 'rm -rf "$T"' EXIT
pass=0; fail=0

check() { # $1=description $2=expected $3=actual
    if [ "$2" = "$3" ]; then
        pass=$((pass+1))
    else
        fail=$((fail+1))
        echo "FAIL: $1"
        echo "  expected: $(printf '%s' "$2" | tr '\n' '|')"
        echo "  actual:   $(printf '%s' "$3" | tr '\n' '|')"
    fi
}

cat > "$T/ok.fl" <<'EOF'
fn main() -> i64 {
    print("cli-ran")
    0
}
EOF

cat > "$T/args.fl" <<'EOF'
fn main() -> i64 {
    print(flint_arg_count())
    print(flint_get_arg(1))
    0
}
EOF

cat > "$T/stdin.fl" <<'EOF'
fn main() -> i64 {
    line = flint_input("")
    print(line)
    0
}
EOF

cat > "$T/fail.fl" <<'EOF'
fn main() -> i64 {
    print("cli-before-fail")
    7
}
EOF

cat > "$T/bad.fl" <<'EOF'
fn main() -> i64 {
    this is not flint
    0
}
EOF

# 1. Bare invocation: compile + run, output visible, exit 0, no artifact.
out="$(timeout -s KILL 300 "$FLINTC" "$T/ok.fl" 2>"$T/err1")"
rc=$?
check "bare invocation exits 0" "0" "$rc"
check "bare invocation runs the program" "cli-ran" "$out"
check "bare invocation leaves no executable" "no" "$(test -e "$T/ok" && echo yes || echo no)"

# 2. -o builds an executable and does not run it.
out="$(timeout -s KILL 300 "$FLINTC" "$T/ok.fl" -o "$T/app" 2>"$T/err2")"
rc=$?
check "-o exits 0" "0" "$rc"
check "-o does not run the program" "" "$out"
check "-o produced an executable" "yes" "$(test -x "$T/app" && echo yes || echo no)"
check "the built executable runs" "cli-ran" "$("$T/app")"

# 3. Explicit --run is still the compile+run path.
out="$(timeout -s KILL 300 "$FLINTC" "$T/ok.fl" --run 2>"$T/err3")"
check "--run exits 0" "0" "$?"
check "--run runs the program" "cli-ran" "$out"

# 4. -o wins over --run regardless of order: build only, never execute.
out="$(timeout -s KILL 300 "$FLINTC" "$T/ok.fl" --run -o "$T/app2" 2>"$T/err4")"
check "-o after --run exits 0" "0" "$?"
check "-o after --run does not run" "" "$out"
check "-o after --run still builds" "yes" "$(test -x "$T/app2" && echo yes || echo no)"

# 5. Program arguments after -- reach the JIT'd main.
out="$(timeout -s KILL 300 "$FLINTC" "$T/args.fl" -- alpha beta 2>"$T/err5")"
check "program args are forwarded" "3
alpha" "$out"

# 6. stdin reaches the JIT'd program unchanged.
out="$(printf 'from-stdin\n' | timeout -s KILL 300 "$FLINTC" "$T/stdin.fl" 2>"$T/err6")"
check "stdin is forwarded" "from-stdin" "$out"

# 7. A compile failure is nonzero and produces no artifact.
timeout -s KILL 300 "$FLINTC" "$T/bad.fl" -o "$T/bad.bin" >"$T/bad.out" 2>&1
rc=$?
check "compile failure exits nonzero" "yes" "$(test "$rc" -ne 0 && echo yes || echo no)"
check "compile failure produces no artifact" "no" "$(test -e "$T/bad.bin" && echo yes || echo no)"

# 8. The program's exit status is propagated by the default path.
out="$(timeout -s KILL 300 "$FLINTC" "$T/fail.fl" 2>"$T/err8")"
rc=$?
check "runtime exit status propagates" "7" "$rc"
check "output before the exit is visible" "cli-before-fail" "$out"

# 9. Existing emit modes still emit instead of running.
timeout -s KILL 300 "$FLINTC" "$T/ok.fl" -o "$T/ok.ll" >"$T/ll.out" 2>&1
check ".ll emit exits 0" "0" "$?"
check ".ll emit wrote a file" "yes" "$(test -s "$T/ok.ll" && echo yes || echo no)"
check ".ll emit does not run" "" "$(cat "$T/ll.out")"
timeout -s KILL 300 "$FLINTC" "$T/ok.fl" -o "$T/ok.o" >"$T/o.out" 2>&1
check ".o emit exits 0" "0" "$?"
check ".o emit wrote a file" "yes" "$(test -s "$T/ok.o" && echo yes || echo no)"
check ".o emit does not run" "" "$(cat "$T/o.out")"

echo "cli-run: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
