#!/usr/bin/env bash
# High-level `flint` driver contract.
#
#   flint run app.fl        build if needed, then run the native artifact
#   flint build app.fl      build only
#   flintc app.fl -o app    lower-level compiler interface (unchanged)
#
# The driver is a thin front end over flintc: it picks .flint/bin/<name> as
# the default artifact, splits run from build, and forwards everything else.
# These checks pin that split, the default paths, program-argument forwarding,
# exit-status propagation, the cross-target emit rules, and the error
# contract. They run from a temporary project directory, which also proves
# flintc resolves its runtime objects from its own install directory rather
# than from the caller's cwd.
#
# Usage: bash tests/test_flint_cli.sh [path/to/flint] [path/to/flintc]
#   default: ./flint, sibling of flint (else ./flintc)
# Environment: FLINT / FLINTC may supply the paths instead.
set -u
FLINT="${1:-${FLINT:-./flint}}"
FLINTC="${2:-${FLINTC:-}}"
if [ -z "$FLINTC" ]; then
    case "$FLINT" in
        */flint) FLINTC="${FLINT%flint}flintc" ;;
        *)       FLINTC="./flintc" ;;
    esac
fi
# The test cd's into temporary projects, so both paths must be absolute.
case "$FLINT" in /*) ;; *) FLINT="$PWD/$FLINT" ;; esac
case "$FLINTC" in /*) ;; *) FLINTC="$PWD/$FLINTC" ;; esac

_TMPBASE="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
T="$(mktemp -d "$_TMPBASE/flint-driver.XXXXXX" 2>/dev/null || echo "$_TMPBASE/flint-driver.$$")"
trap 'rm -rf "$T"' EXIT
# Isolate the compiler cache so the second-run/cache checks are deterministic.
export HOME="$T/home"
mkdir -p "$HOME" "$T/proj"
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

has() { # $1=text $2=pattern -> yes/no
    printf '%s' "$1" | grep -q "$2" && echo yes || echo no
}

# The driver appends .exe on Windows; every artifact path below follows suit.
EXE=""
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*) EXE=".exe" ;;
esac

cat > "$T/proj/hello.fl" <<'EOF'
fn main() {
    print("hello-from-flint")
}
EOF

cat > "$T/proj/args.fl" <<'EOF'
fn main() -> i64 {
    let n = flint_arg_count()
    mut i = 1
    while i < n {
        print(flint_get_arg(i))
        i = i + 1
    }
    7
}
EOF

cat > "$T/proj/fail.fl" <<'EOF'
fn main() -> i64 {
    print("before-fail")
    7
}
EOF

cat > "$T/proj/bad.fl" <<'EOF'
fn main() {
    this is not flint
}
EOF

# ---------------------------------------------------------------- version/help
out="$("$FLINT" --version 2>&1)"
check "--version exits 0" "0" "$?"
check "--version identifies flint" "yes" "$(has "$out" '^flint [0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*$')"
flint_ver="${out#flint }"
flintc_ver="$("$FLINTC" --version 2>/dev/null)"
flintc_ver="${flintc_ver#flintc }"
check "driver and compiler versions agree" "$flintc_ver" "$flint_ver"

out="$("$FLINT" --help 2>&1)"
check "--help exits 0" "0" "$?"
check "--help documents run" "yes" "$(has "$out" 'flint run')"
check "--help documents build" "yes" "$(has "$out" 'flint build')"

# ---------------------------------------------------------------- run
out="$(cd "$T/proj" && timeout -s KILL 300 "$FLINT" run hello.fl 2>"$T/err-run")"
rc=$?
check "run exits 0" "0" "$rc"
check "run executes the program" "hello-from-flint" "$out"
check "run creates the default artifact" "yes" \
      "$(test -x "$T/proj/.flint/bin/hello$EXE" && echo yes || echo no)"
check "default artifact is a standalone executable" "hello-from-flint" \
      "$("$T/proj/.flint/bin/hello$EXE")"

# A second run goes through the existing cache path and must still work.
out="$(cd "$T/proj" && timeout -s KILL 300 "$FLINT" run hello.fl 2>"$T/err-run2")"
check "second run exits 0" "0" "$?"
check "second run executes the program" "hello-from-flint" "$out"
check "second run keeps the artifact" "yes" \
      "$(test -x "$T/proj/.flint/bin/hello$EXE" && echo yes || echo no)"

# ---------------------------------------------------------------- build
rm -rf "$T/proj/.flint"
out="$(cd "$T/proj" && timeout -s KILL 300 "$FLINT" build hello.fl 2>"$T/err-build")"
rc=$?
check "build exits 0" "0" "$rc"
check "build does not run the program" "" "$out"
check "build creates the default artifact" "yes" \
      "$(test -x "$T/proj/.flint/bin/hello$EXE" && echo yes || echo no)"
check "build reports the artifact path" "yes" \
      "$(has "$(cat "$T/err-build")" 'built \.flint/bin/hello')"
check "build artifact runs later" "hello-from-flint" "$("$T/proj/.flint/bin/hello$EXE")"

# --print-size is a measurement: it must report on a cold build and on the
# warm-cache build that follows (the binary-cache shortcut once skipped it).
cat > "$T/proj/psize.fl" <<'EOF'
fn main() {
    print("psize")
}
EOF
out="$(cd "$T/proj" && timeout -s KILL 300 "$FLINT" build psize.fl --print-size 2>&1)"
check "print-size reports on a cold build" "yes" "$(has "$out" 'file bytes:')"
out="$(cd "$T/proj" && timeout -s KILL 300 "$FLINT" build psize.fl --print-size 2>&1)"
check "print-size reports on a cache hit" "yes" "$(has "$out" 'file bytes:')"

# -o overrides the default path and does not create .flint.
rm -rf "$T/proj/.flint"
out="$(cd "$T/proj" && timeout -s KILL 300 "$FLINT" build hello.fl -o custom 2>"$T/err-build2")"
check "build -o exits 0" "0" "$?"
check "build -o creates the chosen artifact" "yes" \
      "$(test -x "$T/proj/custom$EXE" && echo yes || echo no)"
check "build -o leaves .flint alone" "no" \
      "$(test -e "$T/proj/.flint" && echo yes || echo no)"

out="$(cd "$T/proj" && timeout -s KILL 300 "$FLINT" run hello.fl -o custom2 2>"$T/err-run3")"
check "run -o exits 0" "0" "$?"
check "run -o executes the program" "hello-from-flint" "$out"
check "run -o creates the chosen artifact" "yes" \
      "$(test -x "$T/proj/custom2$EXE" && echo yes || echo no)"

# ---------------------------------------------------------------- program args
out="$(cd "$T/proj" && timeout -s KILL 300 "$FLINT" run args.fl one two 2>"$T/err-a1")"
rc=$?
check "direct program args reach the program" "one
two" "$out"
check "program exit status is the driver's status" "7" "$rc"

out="$(cd "$T/proj" && timeout -s KILL 300 "$FLINT" run args.fl -- alpha beta 2>"$T/err-a2")"
rc=$?
check "-- forwards program args" "alpha
beta" "$out"
check "-- forwards exit status" "7" "$rc"

out="$(cd "$T/proj" && timeout -s KILL 300 "$FLINT" run args.fl -- --help 2>"$T/err-a3")"
check "a literal --help after -- belongs to the program" "--help" "$out"

out="$(cd "$T/proj" && timeout -s KILL 300 "$FLINT" run args.fl "a b" 2>"$T/err-a5")"
check "arguments with spaces stay one argument" "a b" "$out"

out="$(cd "$T/proj" && timeout -s KILL 300 "$FLINT" run fail.fl 2>"$T/err-a4")"
check "nonzero program status propagates" "7" "$?"
check "program output is not swallowed" "before-fail" "$out"

# ---------------------------------------------------------------- mode errors
out="$(cd "$T/proj" && timeout -s KILL 300 "$FLINT" run hello.fl --no-run 2>&1)"
rc=$?
check "run --no-run is rejected" "yes" "$(test "$rc" -ne 0 && echo yes || echo no)"
check "run --no-run points at flint build" "yes" "$(has "$out" 'use .flint build.')"

out="$(cd "$T/proj" && timeout -s KILL 300 "$FLINT" run hello.fl --build-only 2>&1)"
check "run --build-only is rejected" "yes" \
      "$(test "$?" -ne 0 && echo yes || echo no)"

out="$(cd "$T/proj" && timeout -s KILL 300 "$FLINT" build hello.fl --run 2>&1)"
rc=$?
check "build --run is rejected" "yes" "$(test "$rc" -ne 0 && echo yes || echo no)"
check "build --run points at flint run" "yes" "$(has "$out" 'use .flint run.')"

out="$(cd "$T/proj" && timeout -s KILL 300 "$FLINT" build hello.fl extra 2>&1)"
check "build rejects program arguments" "yes" \
      "$(test "$?" -ne 0 && echo yes || echo no)"

out="$(cd "$T/proj" && timeout -s KILL 300 "$FLINT" run hello.fl --test 2>&1)"
check "low-level --test is rejected" "yes" \
      "$(test "$?" -ne 0 && echo yes || echo no)"
check "low-level rejection names flintc" "yes" "$(has "$out" 'use flintc directly')"

out="$(cd "$T/proj" && timeout -s KILL 300 "$FLINT" run hello.fl --dump-tokens x 2>&1)"
check "low-level --dump-tokens is rejected" "yes" \
      "$(test "$?" -ne 0 && echo yes || echo no)"

out="$("$FLINT" frobnicate hello.fl 2>&1)"
check "unknown command exits nonzero" "yes" \
      "$(test "$?" -ne 0 && echo yes || echo no)"
check "unknown command names the command" "yes" "$(has "$out" "unknown command 'frobnicate'")"

out="$("$FLINT" run 2>&1)"
check "missing input exits nonzero" "yes" \
      "$(test "$?" -ne 0 && echo yes || echo no)"

# ---------------------------------------------------------------- cross target
out="$(cd "$T/proj" && rm -rf .flint && timeout -s KILL 300 "$FLINT" build hello.fl --target x86_64-pc-linux-gnu 2>"$T/err-t1")"
rc=$?
check "cross-target build exits 0" "0" "$rc"
check "cross-target default output is an object" "yes" \
      "$(test -f "$T/proj/.flint/bin/hello.o" && echo yes || echo no)"
check "cross-target default output is not executable" "no" \
      "$(test -x "$T/proj/.flint/bin/hello.o" && echo yes || echo no)"

out="$(cd "$T/proj" && timeout -s KILL 300 "$FLINT" build hello.fl --target x86_64-pc-linux-gnu -o hello.ll 2>"$T/err-t2")"
check "cross-target -o .ll exits 0" "0" "$?"
check "cross-target -o .ll writes IR" "yes" \
      "$(has "$(cat "$T/proj/hello.ll" 2>/dev/null)" 'target datalayout')"

out="$(cd "$T/proj" && timeout -s KILL 300 "$FLINT" build hello.fl -o hello.ll --target x86_64-pc-linux-gnu 2>"$T/err-t3")"
check "cross-target flags are order-independent" "0" "$?"

out="$(cd "$T/proj" && timeout -s KILL 300 "$FLINT" build hello.fl --target x86_64-pc-linux-gnu -o hello.bin 2>&1)"
rc=$?
check "cross-target native -o is rejected" "yes" \
      "$(test "$rc" -ne 0 && echo yes || echo no)"
check "cross-target rejection explains .ll/.o" "yes" "$(has "$out" '\.ll or \.o')"

out="$(cd "$T/proj" && timeout -s KILL 300 "$FLINT" run hello.fl --target x86_64-pc-linux-gnu 2>&1)"
rc=$?
check "run --target is rejected" "yes" "$(test "$rc" -ne 0 && echo yes || echo no)"
check "run --target suggests flint build" "yes" \
      "$(has "$out" 'flint build --target')"

# flintc treats .ll/.o as emit-only; run must not silently skip execution.
for ext in ll o; do
    out="$(cd "$T/proj" && timeout -s KILL 300 "$FLINT" run hello.fl -o "emit.$ext" 2>&1)"
    rc=$?
    check "run -o .$ext is rejected" "yes" "$(test "$rc" -ne 0 && echo yes || echo no)"
    check "run -o .$ext suggests flint build" "yes" "$(has "$out" 'flint build')"
    check "run -o .$ext emits nothing" "no" \
          "$(test -e "$T/proj/emit.$ext" && echo yes || echo no)"
done

# ---------------------------------------------------------------- failures
# A failed rebuild must not execute the artifact from the previous success.
cp "$T/proj/hello.fl" "$T/proj/stale.fl"
(cd "$T/proj" && timeout -s KILL 300 "$FLINT" build stale.fl >/dev/null 2>&1)
cat > "$T/proj/stale.fl" <<'EOF'
fn main() {
    this is not flint
}
EOF
out="$(cd "$T/proj" && timeout -s KILL 300 "$FLINT" run stale.fl 2>"$T/err-stale")"
rc=$?
check "failed rebuild exits nonzero" "yes" "$(test "$rc" -ne 0 && echo yes || echo no)"
check "failed rebuild never runs the stale artifact" "no" \
      "$(has "$out" 'hello-from-flint')"

# An edited source must win over the cached artifact.
cat > "$T/proj/hello.fl" <<'EOF'
fn main() {
    print("hello-v2")
}
EOF
out="$(cd "$T/proj" && timeout -s KILL 300 "$FLINT" run hello.fl 2>"$T/err-v2")"
check "edited source exits 0" "0" "$?"
check "edited source is rebuilt, not cached" "hello-v2" "$out"
cat > "$T/proj/hello.fl" <<'EOF'
fn main() {
    print("hello-from-flint")
}
EOF

rm -rf "$T/proj/.flint"
out="$(cd "$T/proj" && timeout -s KILL 300 "$FLINT" run bad.fl 2>"$T/err-bad")"
rc=$?
check "compile error exits nonzero" "yes" "$(test "$rc" -ne 0 && echo yes || echo no)"
check "compile error leaves no artifact" "no" \
      "$(test -e "$T/proj/.flint/bin/bad$EXE" && echo yes || echo no)"
check "compile error is reported" "yes" "$(has "$(cat "$T/err-bad")" 'error')"

out="$(cd "$T/proj" && timeout -s KILL 300 "$FLINT" run nope.fl 2>&1)"
check "missing source exits nonzero" "yes" \
      "$(test "$?" -ne 0 && echo yes || echo no)"

out="$(cd "$T/proj" && FLINTC=/nonexistent/flintc timeout -s KILL 300 "$FLINT" run hello.fl 2>&1)"
rc=$?
check "missing flintc exits 127" "127" "$rc"
check "missing flintc explains FLINTC" "yes" "$(has "$out" 'set FLINTC')"

# ---------------------------------------------------------------- invocation
# Default artifacts belong to the caller's cwd, not the source's directory.
rm -rf "$T/.flint"
out="$(cd "$T" && timeout -s KILL 300 "$FLINT" run proj/hello.fl 2>"$T/err-cwd")"
check "source path with a directory exits 0" "0" "$?"
check "default artifact lands in the caller's cwd" "yes" \
      "$(test -x "$T/.flint/bin/hello$EXE" && echo yes || echo no)"

# Spaces in the project path must survive the argv handoff.
mkdir -p "$T/space dir"
cp "$T/proj/hello.fl" "$T/space dir/hello.fl"
out="$(cd "$T/space dir" && timeout -s KILL 300 "$FLINT" run hello.fl 2>"$T/err-space")"
check "project path with spaces exits 0" "0" "$?"
check "project path with spaces runs" "hello-from-flint" "$out"

# Installed side by side on PATH, with no sibling next to argv[0].
mkdir -p "$T/bin"
if ln -s "$FLINT" "$T/bin/flint" 2>/dev/null && ln -s "$FLINTC" "$T/bin/flintc" 2>/dev/null; then
    out="$(cd "$T/proj" && PATH="$T/bin:$PATH" timeout -s KILL 300 flint run hello.fl 2>"$T/err-path")"
    check "PATH-installed flint exits 0" "0" "$?"
    check "PATH-installed flint finds flintc" "hello-from-flint" "$out"
fi

echo "flint-cli: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
