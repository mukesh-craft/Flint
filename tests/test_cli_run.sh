#!/usr/bin/env bash
# CLI one-command contract.
#
#   flintc main.fl              compiles AND runs in one step (ORC JIT, no artifact)
#   flintc main.fl -o app       compiles, links AND runs ./app; app stays on disk
#   flintc main.fl -o app --no-run   builds ./app without running it
#   flintc main.fl --run        explicit compile+run (the default)
#   flintc main.fl -o app --run     -o still builds (and runs) regardless of order
#
# It also pins the failure contract: a compile error exits nonzero, never
# executes a stale artifact, and a program's exit status is the compiler's
# exit status. Existing .ll/.o emit modes are checked so the new default
# cannot silently swallow them.
#
# Usage: bash tests/test_cli_run.sh [path/to/flintc]   (default: ./flintc)
set -u
FLINTC="${1:-./flintc}"
_TMPBASE="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
T="$(mktemp -d "$_TMPBASE/flint-cli.XXXXXX" 2>/dev/null || echo "$_TMPBASE/flint-cli.$$")"
trap 'rm -rf "$T"' EXIT
# Isolate the compiler cache so the binary-hit and module-hit paths below are
# deterministic (the real HOME may already hold an entry for this source).
export HOME="$T/home"
mkdir -p "$HOME"
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

# 2. -o builds the executable, runs it immediately, and keeps it.
out="$(timeout -s KILL 300 "$FLINTC" "$T/ok.fl" -o "$T/app" 2>"$T/err2")"
rc=$?
check "-o exits 0" "0" "$rc"
check "-o runs the newly built program" "cli-ran" "$out"
check "-o produced an executable" "yes" "$(test -x "$T/app" && echo yes || echo no)"
check "the built executable remains runnable" "cli-ran" "$("$T/app")"

# 3. --no-run is the explicit build-only escape hatch.
out="$(timeout -s KILL 300 "$FLINTC" "$T/ok.fl" -o "$T/app2" --no-run 2>"$T/err3")"
check "--no-run exits 0" "0" "$?"
check "--no-run does not run the program" "" "$out"
check "--no-run still builds" "yes" "$(test -x "$T/app2" && echo yes || echo no)"
check "--no-run artifact runs" "cli-ran" "$("$T/app2")"

# 3b. --build-only is the same contract (compat spelling).
out="$(timeout -s KILL 300 "$FLINTC" "$T/ok.fl" -o "$T/app2b" --build-only 2>"$T/err3b")"
check "--build-only exits 0" "0" "$?"
check "--build-only does not run" "" "$out"
check "--build-only still builds" "yes" "$(test -x "$T/app2b" && echo yes || echo no)"

# 4. Explicit --run still runs; with -o it still builds and runs.
out="$(timeout -s KILL 300 "$FLINTC" "$T/ok.fl" --run 2>"$T/err4")"
check "--run exits 0" "0" "$?"
check "--run runs the program" "cli-ran" "$out"

out="$(timeout -s KILL 300 "$FLINTC" "$T/ok.fl" --run -o "$T/app3" 2>"$T/err4b")"
check "--run -o exits 0" "0" "$?"
check "--run -o runs the program" "cli-ran" "$out"
check "--run -o still builds" "yes" "$(test -x "$T/app3" && echo yes || echo no)"

# 5. Program arguments after -- reach the native program.
out="$(timeout -s KILL 300 "$FLINTC" "$T/args.fl" -o "$T/argsbin" -- alpha beta 2>"$T/err5")"
check "program args are forwarded to the AOT program" "3
alpha" "$out"
check "program args leave the executable on disk" "yes" "$(test -x "$T/argsbin" && echo yes || echo no)"

# 6. stdin reaches the native program unchanged.
out="$(printf 'from-stdin\n' | timeout -s KILL 300 "$FLINTC" "$T/stdin.fl" -o "$T/stdinbin" 2>"$T/err6")"
check "stdin is forwarded to the AOT program" "from-stdin" "$out"

# 7. The program's exit status is propagated, and the artifact remains.
out="$(timeout -s KILL 300 "$FLINTC" "$T/fail.fl" -o "$T/failbin" 2>"$T/err7")"
rc=$?
check "AOT runtime exit status propagates" "7" "$rc"
check "output before the AOT exit is visible" "cli-before-fail" "$out"
check "nonzero program leaves its executable on disk" "yes" "$(test -x "$T/failbin" && echo yes || echo no)"

# 8. The JIT path still propagates the program's exit status.
out="$(timeout -s KILL 300 "$FLINTC" "$T/fail.fl" 2>"$T/err8")"
rc=$?
check "JIT runtime exit status propagates" "7" "$rc"
check "JIT output before the exit is visible" "cli-before-fail" "$out"

# 9. A compile failure is nonzero, produces no artifact, and never runs a
#    stale executable that happens to sit at the requested path.
timeout -s KILL 300 "$FLINTC" "$T/bad.fl" -o "$T/bad.bin" >"$T/bad.out" 2>&1
rc=$?
check "compile failure exits nonzero" "yes" "$(test "$rc" -ne 0 && echo yes || echo no)"
check "compile failure produces no artifact" "no" "$(test -e "$T/bad.bin" && echo yes || echo no)"

cat > "$T/stale" <<'EOF'
#!/bin/sh
echo stale-ran
EOF
chmod +x "$T/stale"
out="$(timeout -s KILL 300 "$FLINTC" "$T/bad.fl" -o "$T/stale" 2>&1)"
rc=$?
check "compile failure exits nonzero over a stale artifact" "yes" "$(test "$rc" -ne 0 && echo yes || echo no)"
check "compile failure never runs the stale artifact" "no" "$(printf '%s' "$out" | grep -q 'stale-ran' && echo yes || echo no)"
check "compile failure leaves the stale file untouched" "stale-ran" "$("$T/stale")"

# 10. Existing emit modes still emit instead of running.
timeout -s KILL 300 "$FLINTC" "$T/ok.fl" -o "$T/ok.ll" >"$T/ll.out" 2>&1
check ".ll emit exits 0" "0" "$?"
check ".ll emit wrote a file" "yes" "$(test -s "$T/ok.ll" && echo yes || echo no)"
check ".ll emit does not run" "" "$(cat "$T/ll.out")"
timeout -s KILL 300 "$FLINTC" "$T/ok.fl" -o "$T/ok.o" >"$T/o.out" 2>&1
check ".o emit exits 0" "0" "$?"
check ".o emit wrote a file" "yes" "$(test -s "$T/ok.o" && echo yes || echo no)"
check ".o emit does not run" "" "$(cat "$T/o.out")"

# 11. A binary-cache hit still runs the cached copy.
rm -rf "$HOME/.cache/flintc"
out="$(timeout -s KILL 300 "$FLINTC" "$T/ok.fl" -o "$T/cachebin" 2>"$T/err11a")"
check "cache seed run exits 0" "0" "$?"
check "cache seed run executes" "cli-ran" "$out"
bincache=$(find "$HOME/.cache/flintc" -name '*.bin' 2>/dev/null | head -n1)
check "cache seed stored a binary" "yes" "$(test -n "$bincache" && echo yes || echo no)"
out="$(timeout -s KILL 300 "$FLINTC" "$T/ok.fl" -o "$T/cachebin" 2>"$T/err11b")"
check "binary-cache hit exits 0" "0" "$?"
check "binary-cache hit executes the artifact" "cli-ran" "$out"

# 12. A module-cache hit (binary entry removed) links and runs too.
rm -f "$HOME"/.cache/flintc/*.bin "$T/cachebin"
out="$(timeout -s KILL 300 "$FLINTC" "$T/ok.fl" -o "$T/cachebin" 2>"$T/err12")"
check "module-cache hit exits 0" "0" "$?"
check "module-cache hit executes the artifact" "cli-ran" "$out"
check "module-cache hit leaves the executable" "yes" "$(test -x "$T/cachebin" && echo yes || echo no)"

# 13. --no-run without -o is a usage error, not a silent JIT run.
out="$(timeout -s KILL 300 "$FLINTC" "$T/ok.fl" --no-run 2>&1)"
rc=$?
check "--no-run without -o exits nonzero" "yes" "$(test "$rc" -ne 0 && echo yes || echo no)"
check "--no-run without -o explains itself" "yes" "$(printf '%s' "$out" | grep -q 'requires -o' && echo yes || echo no)"

# 14. -o cannot overwrite the input source. With -o running by default, a
#     typo like `flintc bench.fl -o bench.fl` must not replace the source
#     with the linked executable (that happened once to a benchmark).
cp "$T/ok.fl" "$T/self.fl"
before="$(cat "$T/self.fl")"
out="$(timeout -s KILL 300 "$FLINTC" "$T/self.fl" -o "$T/self.fl" 2>&1)"
rc=$?
check "-o at the input path is rejected" "yes" "$(test "$rc" -ne 0 && echo yes || echo no)"
check "-o at the input path explains itself" "yes" "$(printf '%s' "$out" | grep -q 'refusing to overwrite' && echo yes || echo no)"
check "-o at the input path leaves the source intact" "$before" "$(cat "$T/self.fl")"
check "-o at the input path does not run" "no" "$(printf '%s' "$out" | grep -q 'cli-ran' && echo yes || echo no)"

echo "cli-run: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
