#!/usr/bin/env bash
# rust-stdlib gate: a Rust standard-library module must work through BOTH
# Flint execution paths.
#
# This gate exists because the two paths fail differently and both failed during
# development:
#   * AOT links flint_std.o (the staticlib flattened with `ld -r`), so a Rust
#     module that only ever sees the JIT is not proof of anything.
#   * The JIT could not take a Rust object when the module used `std`: Rust std
#     resolves thread-local handles through libc, and inside JIT memory Android's
#     linker aborts with "increment_dso_handle_reference_counter: Couldn't find
#     soinfo by dso_handle". The module is therefore #![no_std] -- no allocator,
#     no TLS -- so one flattened object serves both paths.
# A module that passes one path and fails the other is a broken module, so both
# are checked here, plus the error path and out-of-range behaviour.
#
# Usage: bash tests/test_rust_stdlib.sh [path/to/flintc]   (default: ./flintc)
set -u
FLINTC="${1:-./flintc}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
WORK="${TMPDIR:-$HOME/.tmp}/flint-rust-stdlib"
rm -rf "$WORK"; mkdir -p "$WORK"
pass=0; fail=0
ok()  { pass=$((pass+1)); echo "PASS $1"; }
bad() { fail=$((fail+1)); echo "FAIL $1${2:+ — $2}"; }

command -v cargo >/dev/null 2>&1 || { echo "SKIP rust-stdlib (cargo not installed: pkg install rust)"; exit 0; }

if [ ! -f flint_std.o ] || [ runtime/rust/src/lib.rs -nt flint_std.o ]; then
    echo "building the Rust stdlib modules..."
    (cd runtime/rust && cargo build --release) > "$WORK/build.log" 2>&1 \
        || { echo "FAIL cargo build"; tail -n 12 "$WORK/build.log"; exit 1; }
    ld -r --whole-archive runtime/rust/target/release/libflint_std.a -o flint_std.o \
        || { echo "FAIL ld -r flatten"; exit 1; }
fi
[ -f flint_std.o ] && ok "flint_std.o exists (shared JIT+AOT artifact)" || bad "flint_std.o exists"

# The module must carry no Rust std: a std symbol here would abort in JIT memory
# and duplicate a user library's std at link time.
if llvm-nm flint_std.o 2>/dev/null | grep -qE "rust_eh_personality|std::"; then
    bad "module is no_std (no std symbols in the object)"
else
    ok "module is no_std (no std symbols in the object)"
fi

# The crate's own unit tests: parsing rules, null handling, free round-trip.
if (cd runtime/rust && cargo test --quiet) > "$WORK/test.log" 2>&1; then
    n=$(grep -oE "[0-9]+ passed" "$WORK/test.log" | head -1)
    ok "cargo test: $n"
else
    bad "cargo test"; tail -n 12 "$WORK/test.log"
fi

expect_all() { # $1=label $2=output — every marker must be present
    local label="$1" out="$2" missing=0
    shift 2
    for want in "$@"; do
        printf '%s\n' "$out" | grep -qxF -- "$want" || { missing=1; echo "   missing: $want"; }
    done
    if [ "$missing" = 0 ]; then ok "$label"; else bad "$label"; fi
}

# --- JIT path ---------------------------------------------------------------
if out="$(timeout -s KILL 300 "$FLINTC" tests/t_csv.fl 2>&1)"; then
    expect_all "JIT: csv parse, rows, cols, quoted field, error path" "$out" \
        "3" "widget, large" "42" "-1" \
        "csv: unterminated quoted field at end of input" "0" "csv-ok"

# Crypto: published vectors through the Flint surface, not just in cargo tests.
if out="$(timeout -s KILL 300 "$FLINTC" tests/t_crypto.fl 2>&1)"; then
    expect_all "JIT: sha256/hmac vectors, GCM round trip, tamper rejection" "$out" \
        "64" "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" \
        "60" "28" "-1" \
        "crypto: GCM authentication failed (wrong key, nonce, aad or tag)" "crypto-ok"
else
    bad "JIT: tests/t_crypto.fl exits 0" "$(printf '%s' "$out" | tail -n 3 | tr '\n' ' ')"
fi
else
    bad "JIT: tests/t_csv.fl exits 0" "$(printf '%s' "$out" | tail -n 3 | tr '\n' ' ')"
fi

# SQLite: a Rust wrapper over the system libsqlite3. This is the check that
# proves the JIT can reach a NON-libc shared library at all -- the compiler
# dlopens libsqlite3 and publishes the sqlite3_* symbols into the ORC session.
if out="$(timeout -s KILL 300 "$FLINTC" tests/t_sqlite.fl 2>&1)"; then
    expect_all "JIT: sqlite create/insert/select/NULL/bad-sql" "$out" \
        "bolt" "42" "-1" "sqlite: cannot prepare statement" "sqlite-ok"
    case "$out" in
        3.*) ok "JIT: sqlite version reported" ;;
        *) bad "JIT: sqlite version reported" ;;
    esac
else
    bad "JIT: tests/t_sqlite.fl exits 0" "$(printf '%s' "$out" | tail -n 3 | tr '\n' ' ')"
fi

# Encoders, password hashing and system info, called from Flint (not just in
# the crate's own tests).
if out="$(timeout -s KILL 600 "$FLINTC" tests/t_stdlib_ext.fl 2>&1)"; then
    expect_all "JIT: encoders, Argon2id, system info" "$out" \
        "3808858755" "MZXW6YTBOI======" "hello%20world" \
        "\$argon2id\$" "android" "stdlib-ok"
else
    bad "JIT: tests/t_stdlib_ext.fl exits 0" "$(printf '%s' "$out" | tail -n 3 | tr '\n' ' ')"
fi

# FTS5, snippet() and the math functions: the SQLite build on this device has
# them compiled in, and this proves a Flint program can actually use them --
# "the library supports it" and "a language can reach it" are different claims.
if out="$(timeout -s KILL 300 "$FLINTC" tests/t_fts5.fl 2>&1)"; then
    expect_all "JIT: FTS5 match, snippet, zero-result, math" "$out" \
        "Flint" "transfer data over the [network]" "4" "fts5-ok"
else
    bad "JIT: tests/t_fts5.fl exits 0" "$(printf '%s' "$out" | tail -n 3 | tr '\n' ' ')"
fi

# --- AOT path ---------------------------------------------------------------
if timeout -s KILL 600 "$FLINTC" tests/t_csv.fl -o "$WORK/t_csv.bin" > "$WORK/aot.log" 2>&1; then
    if out="$("$WORK/t_csv.bin" 2>&1)"; then
        expect_all "AOT: csv behaves as in JIT" "$out" "3" "widget, large" "42" "-1" "csv-ok"
    else
        bad "AOT: csv binary runs" "$(printf '%s' "$out" | tail -n 3 | tr '\n' ' ')"
    fi
else
    bad "AOT: csv links" "$(tail -n 3 "$WORK/aot.log" | tr '\n' ' ')"
fi

if timeout -s KILL 600 "$FLINTC" tests/t_sqlite.fl -o "$WORK/t_sqlite.bin" > "$WORK/aot3.log" 2>&1; then
    if out="$("$WORK/t_sqlite.bin" 2>&1)"; then
        expect_all "AOT: sqlite behaves as in JIT" "$out" "bolt" "42" "sqlite-ok"
    else
        bad "AOT: sqlite binary runs" "$(printf '%s' "$out" | tail -n 3 | tr '\n' ' ')"
    fi
else
    bad "AOT: sqlite links" "$(tail -n 3 "$WORK/aot3.log" | tr '\n' ' ')"
fi

if timeout -s KILL 600 "$FLINTC" tests/t_crypto.fl -o "$WORK/t_crypto.bin" > "$WORK/aot2.log" 2>&1; then
    if out="$("$WORK/t_crypto.bin" 2>&1)"; then
        expect_all "AOT: crypto behaves as in JIT" "$out" \
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" \
            "60" "28" "crypto-ok"
    else
        bad "AOT: crypto binary runs" "$(printf '%s' "$out" | tail -n 3 | tr '\n' ' ')"
    fi
else
    bad "AOT: crypto links" "$(tail -n 3 "$WORK/aot2.log" | tr '\n' ' ')"
fi

# --- JIT and AOT must agree exactly ---------------------------------------
jit_out="$(timeout -s KILL 300 "$FLINTC" tests/t_csv.fl 2>&1 || true)"
if [ -x "$WORK/t_csv.bin" ]; then
    aot_out="$("$WORK/t_csv.bin" 2>&1 || true)"
    if [ "$jit_out" = "$aot_out" ]; then ok "JIT output == AOT output"
    else bad "JIT output == AOT output" "$(printf '%s' "$jit_out" | tr '\n' ' ')"; fi
fi

# --- the module must not be reachable by accident --------------------------
# Rust std symbols would break JIT loading and collide with a user's own Rust
# library, so the no_std property is asserted on the shipped object, not just
# hoped for in the crate.
if llvm-nm flint_std.o 2>/dev/null | grep -qE "rust_eh_personality|std::"; then
    bad "module is no_std (no std symbols in the object)"
else
    ok "module is no_std (no std symbols in the object)"
fi

# A typo in a Flint extern must still fail loudly: the JIT defines only the
# explicitly listed module symbols, not the whole process image.
printf 'extern "C" {\n    fn flint_csv_typo_symbol(a: i64) -> i64\n}\nfn main() {\n    print(flint_csv_typo_symbol(1))\n}\n' > "$WORK/typo.fl"
if timeout -s KILL 120 "$FLINTC" "$WORK/typo.fl" > "$WORK/typo.log" 2>&1; then
    bad "a typo'd module symbol still fails loudly"
else
    ok "a typo'd module symbol still fails loudly"
fi

echo "rust-stdlib: $pass passed, $fail failed"
[ "$fail" -eq 0 ]