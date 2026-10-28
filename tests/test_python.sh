#!/usr/bin/env bash
# python gate: the embedded-CPython bridge must work, and must stay optional.
#
# Two things this protects:
#
#   1. The feature was marked complete in ROADMAP.md while nothing in the
#      repository -- no test, example, benchmark or tutorial -- ever ran a
#      `python { }` block or py_eval(). Writing this gate found a real defect:
#      py_eval is integer-only (PyLong_AsLongLong), so Python's `/`, which
#      returns a float even for integers, raised TypeError for nearly every
#      arithmetic expression. py_eval_float is the fix.
#
#   2. A program that never touches Python must not link libpython. The bridge
#      used to be linked unconditionally, so every AOT binary carried a
#      libpython.so NEEDED entry whether or not the program used it. This gate
#      fails if a Python-free program still links it.
#
# Usage: bash tests/test_python.sh [path/to/flintc]   (default: ./flintc)
set -u
FLINTC="${1:-./flintc}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
pass=0; fail=0
ok()  { pass=$((pass+1)); echo "PASS $1"; }
bad() { fail=$((fail+1)); echo "FAIL $1${2:+ — $2}"; }

# The expected values, one per line, in fixture order.
EXPECTED="5 1024 3 5050 44 3 1 1 1 1 42 1 1 1"

if ! out="$(timeout -s KILL 120 "$FLINTC" tests/t_python.fl 2>/dev/null)"; then
    bad "tests/t_python.fl runs" "$(printf '%s' "$out" | tail -n 3 | tr '\n' ' ')"
    echo "python: $pass passed, $fail failed"
    exit 1
fi
if [ "$(printf '%s' "$out" | tr '\n' ' ' | sed 's/  */ /g;s/ *$//')" = "$EXPECTED" ]; then
    ok "JIT: python blocks, py_eval and py_eval_float all behave"
else
    bad "JIT: python assertions" "got [$(printf '%s' "$out" | tr '\n' ' ')]"
fi

# --- AOT: same behaviour, and it must link libpython because it uses Python --
if timeout -s KILL 120 "$FLINTC" tests/t_python.fl --no-run -o "$WORK/py.bin" > "$WORK/aot.log" 2>&1; then
    if out="$("$WORK/py.bin" 2>/dev/null)"; then
        if [ "$(printf '%s' "$out" | tr '\n' ' ' | sed 's/  */ /g;s/ *$//')" = "$EXPECTED" ]; then
            ok "AOT: python behaves as in JIT"
        else
            bad "AOT: python assertions" "got [$(printf '%s' "$out" | tr '\n' ' ')]"
        fi
    else
        bad "AOT: python binary runs" "$(printf '%s' "$out" | tail -n 3 | tr '\n' ' ')"
    fi
    if readelf -d "$WORK/py.bin" 2>/dev/null | grep -q libpython; then
        ok "AOT: a program using python links libpython"
    else
        bad "AOT: python program links libpython" "no NEEDED entry for libpython"
    fi
else
    bad "AOT: tests/t_python.fl links" "$(tail -n 3 "$WORK/aot.log" | tr '\n' ' ')"
fi

# --- A program that never touches Python must not carry libpython -----------
cat > "$WORK/nopy.fl" <<'FL'
fn main() {
    print(1 + 1)
}
FL
if timeout -s KILL 120 "$FLINTC" "$WORK/nopy.fl" --no-run -o "$WORK/nopy.bin" > "$WORK/nopy.log" 2>&1; then
    if readelf -d "$WORK/nopy.bin" 2>/dev/null | grep -q libpython; then
        bad "AOT: a Python-free program does not link libpython" \
            "$(readelf -d "$WORK/nopy.bin" 2>/dev/null | grep libpython | tr -s ' ' | cut -d' ' -f2)"
    else
        ok "AOT: a Python-free program does not link libpython"
    fi
else
    bad "AOT: Python-free program links" "$(tail -n 3 "$WORK/nopy.log" | tr '\n' ' ')"
fi

echo "python: $pass passed, $fail failed"
[ "$fail" -eq 0 ]