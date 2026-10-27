#!/usr/bin/env bash
# Comparison, part 2: binary size, build time and cold start.
#
# - binary size: the linked artifact of a hello-world program (stripped
#   where the toolchain strips by default, so the numbers compare the
#   runtime payload, not the debug info).
# - build time: cold, from source to executable.
# - cold start: time to run the trivial program (process start + runtime
#   init), which is what a CLI tool feels like.
#
# Usage: bash tools/bench_tooling.sh
set -u
cd "$(dirname "$0")/.."
FLINTC="${FLINTC:-./flintc}"
T="${TMPDIR:-/data/data/com.termux/files/usr/tmp}/bench_tooling"
mkdir -p "$T"
# flintc's binary cache is keyed by source content (plus compiler identity
# and codegen flags), not by path or output name: identical bytes at a new
# path still hit. Append a unique comment so every run is a real build.
RUNID="run_$$_$(date +%s)"
mkdir -p "$T/$RUNID"
T="$T/$RUNID"

cat > "$T/hello.fl" <<EOF
fn main() {
    safe_x = 5
    print(safe_x)
}
# size probe $RUNID
EOF

cat > "$T/hello.c" <<'EOF'
#include <stdio.h>
int main(void) { printf("hello\n"); return 0; }
EOF
cat > "$T/hello.cpp" <<'EOF'
#include <cstdio>
int main() { std::printf("hello\n"); return 0; }
EOF
cat > "$T/hello.rs" <<'EOF'
fn main() { println!("hello"); }
EOF
cat > "$T/hello.py" <<'EOF'
print("hello")
EOF

size_of() { # $1=path
    [ -f "$1" ] || { echo "-"; return; }
    stat -c %s "$1" 2>/dev/null || echo "-"
}
strip_size() { # $1=path
    [ -f "$1" ] || { echo "-"; return; }
    local tmp="$1.stripped"
    cp "$1" "$tmp" 2>/dev/null
    strip "$tmp" >/dev/null 2>&1
    stat -c %s "$tmp" 2>/dev/null || echo "-"
    rm -f "$tmp"
}

printf "%-22s %12s %12s\n" "artifact" "bytes" "stripped"
printf "%-22s %12s %12s\n" "----------------------" "------------" "------------"

"$FLINTC" "$T/hello.fl" --no-run -o "$T/hello.flint" >/dev/null 2>&1
printf "%-22s %12s %12s\n" "Flint (native)" "$(size_of "$T/hello.flint")" "$(strip_size "$T/hello.flint")"
clang -O2 -o "$T/hello.c.bin" "$T/hello.c" >/dev/null 2>&1
printf "%-22s %12s %12s\n" "C (clang -O2)" "$(size_of "$T/hello.c.bin")" "$(strip_size "$T/hello.c.bin")"
clang++ -O2 -o "$T/hello.cpp.bin" "$T/hello.cpp" >/dev/null 2>&1
printf "%-22s %12s %12s\n" "C++ (clang++ -O2)" "$(size_of "$T/hello.cpp.bin")" "$(strip_size "$T/hello.cpp.bin")"
rustc -O -o "$T/hello.rs.bin" "$T/hello.rs" >/dev/null 2>&1
printf "%-22s %12s %12s\n" "Rust (rustc -O)" "$(size_of "$T/hello.rs.bin")" "$(strip_size "$T/hello.rs.bin")"
printf "%-22s %12s %12s\n" "Python (interpreter)" "$(size_of "$(which python3)")" "$(strip_size "$(which python3)")"

echo
echo "build time (cold, source -> executable):"
bt() { # $1=label $2=command...
    local label="$1"; shift
    local t0 t1
    t0=$(date +%s%N)
    "$@" >/dev/null 2>&1
    t1=$(date +%s%N)
    printf "  %-24s %8.0f ms\n" "$label" "$(( (t1 - t0) / 1000000 ))"
}
cat > "$T/bt.fl" <<EOF
fn main() {
    safe_x = 5
    print(safe_x)
}
# build-time probe $RUNID
EOF
bt "Flint" "$FLINTC" "$T/bt.fl" --no-run -o "$T/bt.flint"
bt "C (clang -O2)" clang -O2 -o "$T/bt.c" "$T/hello.c"
bt "C++ (clang++ -O2)" clang++ -O2 -o "$T/bt.cpp" "$T/hello.cpp"
bt "Rust (rustc -O)" rustc -O -o "$T/bt.rs" "$T/hello.rs"
printf "  %-24s %8s\n" "Python (no build)" "n/a"

echo
echo "cold start (hello program, 5 runs, wall clock incl. process start):"
cs() { # $1=label $2=cmd...
    local label="$1"; shift
    local t0 t1 total=0
    local i
    for i in 1 2 3 4 5; do
        t0=$(date +%s%N)
        "$@" >/dev/null 2>&1
        t1=$(date +%s%N)
        total=$(( total + (t1 - t0) / 1000000 ))
    done
    printf "  %-24s %8.0f ms\n" "$label" "$(( total / 5 ))"
}
cs "Flint (native)" "$T/hello.flint"
cs "C" "$T/hello.c.bin"
cs "C++" "$T/hello.cpp.bin"
cs "Rust" "$T/hello.rs.bin"
cs "Python (interpreter)" python3 "$T/hello.py"
