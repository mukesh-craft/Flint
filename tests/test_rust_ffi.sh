#!/usr/bin/env bash
# rust-ffi gate: prove the C-ABI works in BOTH directions, with Rust as the
# foreign language on both sides.
#
#   Flint -> Rust : a Rust staticlib provides rust_add/rust_sum_to/
#                   rust_strlen; Flint declares them extern and links it.
#   Rust -> Flint : flintc compiles tests/rust/lib_add.fl (a module with no
#                   main) to an object exporting unmangled `add`; a Rust
#                   program links that object plus the Flint runtime and
#                   calls `add` and `flint_str_length` directly.
#
# No cargo, no registry, no network: rustc + clang only, so the gate runs
# offline like every other gate here.
#
# Usage: bash tests/test_rust_ffi.sh [path/to/flintc]   (default: ./flintc)
set -u
FLINTC="${1:-./flintc}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
WORK="${TMPDIR:-/tmp}/flint-rust-ffi"
rm -rf "$WORK"; mkdir -p "$WORK"
pass=0; fail=0
ok()   { pass=$((pass+1)); echo "PASS $1"; }
bad()  { fail=$((fail+1)); echo "FAIL $1"; }

command -v rustc >/dev/null 2>&1 || { echo "SKIP rust-ffi (rustc not installed: pkg install rust)"; exit 0; }

# --- build the Rust static library (Flint -> Rust side) ---------------------
if rustc --edition 2021 --crate-type staticlib -O \
     tests/rust/flintrust.rs -o "$WORK/libflintrust.a" 2> "$WORK/lib.log"; then
    ok "rust staticlib builds"
else
    bad "rust staticlib builds"; sed -n '1,8p' "$WORK/lib.log"; fi
[ -f "$WORK/libflintrust.a" ] && ok "libflintrust.a exists" || bad "libflintrust.a exists"

# --- Flint program calls Rust ------------------------------------------------
if [ -f "$WORK/libflintrust.a" ] && \
   "$FLINTC" tests/rust/from_flint.fl --no-run -o "$WORK/from_flint" \
        --link "$WORK/libflintrust.a" > "$WORK/ff.log" 2>&1; then
    ok "flint links the rust library"
else
    bad "flint links the rust library"; sed -n '1,8p' "$WORK/ff.log"; fi

if [ -x "$WORK/from_flint" ]; then
    out="$("$WORK/from_flint" 2>&1)"; rc=$?
    if [ "$rc" -eq 0 ] && [ "$out" = "$(printf '42\n500500\n5')" ]; then
        ok "flint calls rust: 42 / 500500 / 5"
    else
        bad "flint calls rust (exit $rc): $(printf '%s' "$out" | tr '\n' ' ')"
    fi
else
    bad "flint->rust binary exists"
fi

# --- Flint module emitted for a foreign caller (Rust -> Flint side) ---------
if "$FLINTC" tests/rust/lib_add.fl -o "$WORK/lib_add.o" > "$WORK/la.log" 2>&1; then
    ok "flint emits a main-less library object"
else
    bad "flint emits a main-less library object"; sed -n '1,8p' "$WORK/la.log"; fi

if command -v llvm-nm >/dev/null 2>&1; then
    if llvm-nm "$WORK/lib_add.o" 2>/dev/null | grep -qE '^[0-9a-f]+ T add$'; then
        ok "flint object exports unmangled 'add'"
    else
        bad "flint object exports unmangled 'add'"
    fi
else
    echo "SKIP symbol check (llvm-nm not found)"
fi

# --- Rust program calls Flint (code + runtime) ------------------------------
# Every runtime object except the two that would drag in a foreign toolchain
# (pyruntime.o needs libpython, ffi_helper.o is the C-side FFI demo fixture).
RUNTIME_OBJS=()
for o in "$ROOT"/runtime.o "$ROOT"/flint_*.o; do
    case "$o" in
        */pyruntime.o|*/ffi_helper.o) continue ;;
        # flint_std.o is Rust std; linking it together with this gate's own
        # Rust staticlib produces duplicate std symbols (rust_eh_personality,
        # std::panicking::EMPTY_PANIC). That constraint is real and documented:
        # one Rust std per link. The stdlib modules get their own gate
        # (tests/test_rust_stdlib.sh), which links them on their own.
        */flint_std.o) continue ;;
    esac
    RUNTIME_OBJS+=(-C "link-arg=$o")
done

if rustc --edition 2021 -O tests/rust/to_rust.rs -o "$WORK/to_rust" \
     -C "link-arg=$WORK/lib_add.o" "${RUNTIME_OBJS[@]}" \
     > "$WORK/tr.log" 2>&1; then
    ok "rust links the flint object and runtime"
else
    bad "rust links the flint object and runtime"; sed -n '1,12p' "$WORK/tr.log"; fi

if [ -x "$WORK/to_rust" ]; then
    out="$("$WORK/to_rust" 2>&1)"; rc=$?
    if [ "$rc" -eq 0 ] && \
       printf '%s\n' "$out" | grep -qxF 'rust called flint add: 42' && \
       printf '%s\n' "$out" | grep -qxF 'rust called flint_str_length: 5'; then
        ok "rust calls flint add() and the runtime"
    else
        bad "rust calls flint (exit $rc): $(printf '%s' "$out" | tr '\n' ' ')"
    fi
else
    bad "rust->flint binary exists"
fi

echo "rust-ffi: $pass passed, $fail failed"
[ "$fail" -eq 0 ]