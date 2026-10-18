#!/usr/bin/env bash
# The compiler must always terminate.
#
# A hang is the worst failure mode a compiler has: no message, no exit code, no
# CPU spike, and CI simply stalls until a timeout. One shipped here: matching a
# PAYLOAD enum inside any function other than `main` looped forever, because the
# arm parser rejected `Node(a, b)` and `consume()` reported that error without
# advancing the token cursor, so the enclosing arm loop could not make progress.
#
# Two halves:
#
#   1. Run every .fl file in the corpus. A hang on any checked-in program is a
#      regression, not a curiosity.
#   2. Run deliberately MALFORMED programs -- the shapes that historically made
#      error recovery non-terminating: a missing comma, an unclosed paren, an
#      empty arm body, a truncated file. These must produce a diagnostic and
#      exit non-zero. They must never hang, and they must never "succeed".
#
# Usage: bash tests/test_no_hang.sh [path/to/flintc]   (default: ./flintc)
set -u
FLINTC="${1:-./flintc}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
pass=0; fail=0
ok()  { pass=$((pass+1)); echo "PASS $1"; }
bad() { fail=$((fail+1)); echo "FAIL $1${2:+ — $2}"; }

# run_case <label> <file> <expect: run|reject|terminate>
#
#   run       - must compile and exit 0
#   reject    - must exit non-zero with a diagnostic (never LLVM IR)
#   terminate - only that it must NOT hang. Used for the corpus, because many
#              checked-in examples are deliberate negative cases: examples/
#              aegis_uaf.fl, borrow_error.fl and move_error.fl are SUPPOSED to
#              be rejected, and ffi_printf.fl exits 20 by design. Demanding
#              exit 0 from them would make this gate fail on correct code.
run_case() {
    local label="$1" file="$2" expect="$3" out rc
    out="$(timeout -s KILL 45 "$FLINTC" "$file" 2>&1)"
    rc=$?
    if [ "$rc" = "137" ] || [ "$rc" = "124" ]; then
        bad "$label terminates" "HUNG (killed after 45s)"
        return
    fi
    case "$expect" in
        run)
            if [ "$rc" = "0" ]; then ok "$label terminates"; else bad "$label terminates" "exit=$rc $(printf '%s' "$out" | head -1)"; fi
            ;;
        reject)
            if [ "$rc" = "0" ]; then
                bad "$label terminates" "accepted a program that must be rejected"
            elif printf '%s' "$out" | grep -qE '%[0-9]+ = call|double [0-9]|getelementptr|Function verification failed|Call parameter type'; then
                bad "$label terminates" "leaked LLVM IR: $(printf '%s' "$out" | head -1)"
            else
                ok "$label terminates with a diagnostic"
            fi
            ;;
        terminate)
            if printf '%s' "$out" | grep -qE '%[0-9]+ = call|double [0-9]|getelementptr|Function verification failed|Call parameter type'; then
                bad "$label terminates" "leaked LLVM IR: $(printf '%s' "$out" | head -1)"
            else
                ok "$label terminates (exit=$rc)"
            fi
            ;;
    esac
}

# Known pre-existing IR leaks, listed rather than hidden. Each is a real
# defect that this gate deliberately does NOT yet fail on; removing an entry
# means fixing it. Keeping them visible is better than weakening the pattern.
#
#   examples/ai_arch_bench.fl -- an `extern "C"` declaration for farena_init
#     does not match the call, so LLVM's own verifier rejects the module. The
#     real fix is the extern signature, not the parser, and it is unrelated to
#     the hang work.
known_ir_leak() {
    case "$1" in
        examples/ai_arch_bench.fl) return 0 ;;
        *) return 1 ;;
    esac
}

# --- 1. the corpus must compile and run -------------------------------------
count=0
skipped=0
for f in examples/*.fl tests/*.fl; do
    [ -f "$f" ] || continue
    if known_ir_leak "$f"; then
        skipped=$((skipped+1))
        bad "corpus $f terminates" "KNOWN pre-existing IR leak (see known_ir_leak)"
        continue
    fi
    count=$((count+1))
    run_case "corpus $f" "$f" terminate
done
ok "corpus checked ($count files, $skipped known issue(s))"

# --- 2. malformed input must be rejected, never hang ------------------------
# Each case is a shape that has historically broken error recovery.
write_case() { printf '%s' "$2" > "$WORK/$1.fl"; }

write_case missing_comma 'enum T { A, B(i64, i64) }
fn g(t: T) -> i64 {
    match t {
        T.A => { return 0 }
        T.B(x, y) => { return x + y },
    }
    return -1
}
fn main() { print(g(T.B(1, 2))) }
'

write_case unclosed_paren 'fn f(a: i64, b: i64) -> i64 {
    return a + b
}
fn main() { print(f(1, 2) }
'

write_case empty_arm 'enum T { A, B }
fn g(t: T) -> i64 {
    match t {
        T.A => ,
        T.B => { return 1 },
    }
    return 0
}
fn main() { print(g(T.A)) }
'

write_case no_arrow 'enum T { A, B }
fn g(t: T) -> i64 {
    match t {
        T.A 1,
        T.B 2,
    }
    return 0
}
fn main() { print(g(T.A)) }
'

write_case truncated 'enum T { A, B
fn g(t: T) -> i64 {
    match t {
        T.A => { return 0
'

write_case stray_close 'fn main() {
    a = [1, 2, 3]
    print(a[0])
}
}
'

write_case bad_binding_count 'enum T { A, B(i64, i64) }
fn g(t: T) -> i64 {
    match t {
        T.A => { return 0 },
        T.B(only) => { return only },
    }
    return -1
}
fn main() { print(g(T.B(1, 2))) }
'

for c in missing_comma unclosed_paren empty_arm no_arrow truncated stray_close bad_binding_count; do
    run_case "malformed $c" "$WORK/$c.fl" reject
done

echo "no-hang: $pass passed, $fail failed"
[ "$fail" -eq 0 ]