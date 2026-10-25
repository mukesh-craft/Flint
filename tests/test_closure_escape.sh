#!/usr/bin/env bash
# Gate: Phase 2 / 2D -- a capturing closure may escape, and only pays for the
# heap when it actually does.
#
# The rule: captures are snapshotted by value into the env box. A box that
# stays inside its defining frame lives in an alloca; at an escape site
# (return, global store, call argument) the box is copied to the heap and
# every alias is repointed. A non-escaping closure must not allocate.
#
# Usage: bash tests/test_closure_escape.sh [path/to/flintc]
set -u
cd "$(dirname "$0")/.."
FLINTC="${1:-./flintc}"
T="${TMPDIR:-/data/data/com.termux/files/usr/tmp}/closure_escape"
mkdir -p "$T"
pass=0; fail=0

check_out() { # $1=file $2=expected stdout
    local file="$1" want="$2" out rc
    out="$("$FLINTC" "$file" 2>&1)"
    rc=$?
    if [ $rc -ne 0 ]; then
        echo "FAIL $file (exit $rc)"; printf '%s\n' "$out" | head -5
        fail=$((fail+1)); return
    fi
    if [ "$out" = "$want" ]; then pass=$((pass+1));
    else echo "FAIL $file (want '$want', got '$out')"; fail=$((fail+1)); fi
}

# The five escape shapes. Each prints 42 if the closure survived the frame.
check_out tests_manual/t_capturing_escape.fl "42"   # return a literal
check_out tests_manual/t_capturing_alias.fl  "42"   # return an alias
check_out tests_manual/t_capturing_global.fl "42"   # store into a global
check_out tests_manual/t_capturing_arg.fl    "42"   # pass as a call argument
check_out tests_manual/t_capturing_assign.fl "42"   # local assign, then return

# Codegen evidence: an escaping closure must call flint_malloc; a closure that
# stays in its frame must not. Checking the emitted module (not the source)
# keeps the gate honest about what the compiler actually did.
emit() { # $1=file $2=out.ll
    "$FLINTC" "$1" -o "$2" >/dev/null 2>&1
}

if emit tests_manual/t_capturing_escape.fl "$T/escape.ll"; then
    if grep -q "flint_malloc" "$T/escape.ll"; then pass=$((pass+1));
    else echo "FAIL escaped closure: no flint_malloc in the emitted IR"; fail=$((fail+1)); fi
else
    echo "FAIL tests_manual/t_capturing_escape.fl (could not emit IR)"; fail=$((fail+1))
fi

cat > "$T/inside.fl" <<'EOF'
fn main() {
    k = flint_time_ns() % 100
    cb = |x: i64| x + k
    g = cb
    print(g(37))
}
EOF
if emit "$T/inside.fl" "$T/inside.ll"; then
    if grep -q "flint_malloc" "$T/inside.ll"; then
        echo "FAIL non-escaping closure: flint_malloc in the emitted IR"; fail=$((fail+1))
    else
        pass=$((pass+1))
    fi
else
    echo "FAIL non-escaping closure (could not emit IR)"; fail=$((fail+1))
fi

echo "closure-escape: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
