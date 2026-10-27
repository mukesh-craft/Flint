#!/usr/bin/env bash
# Gate: Rust-style unused-variable warnings (W0001/W0002).
#
# Contract under test:
#   * a local that is never read warns (W0001) and points at its name;
#   * a local that is written but never read warns (W0002) -- a write is
#     not a use;
#   * an unused parameter warns too;
#   * a leading underscore opts out (`_x`), as in Rust;
#   * a used local does not warn;
#   * the warning does not stop the program: JIT and AOT still run, and the
#     AOT artifact survives;
#   * `--no-warn-unused` / `-Wno-unused` silences the lint;
#   * imported files are linted against their own source path;
#   * the JSON form carries level=warning and a W code.
#
# Usage: bash tests/test_unused_warnings.sh [path/to/flintc]  (default: ./flintc)
set -u
FLINTC="${1:-./flintc}"
_TMPBASE="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
T="$(mktemp -d "$_TMPBASE/flint-warn.XXXXXX" 2>/dev/null || echo "$_TMPBASE/flint-warn.$$")"
trap 'rm -rf "$T"' EXIT
pass=0; fail=0

check() { # $1=description $2=expected $3=actual
    if [ "$2" = "$3" ]; then pass=$((pass+1));
    else fail=$((fail+1)); echo "FAIL: $1";
        echo "  expected: $(printf '%s' "$2" | tr '\n' '|')";
        echo "  actual:   $(printf '%s' "$3" | tr '\n' '|')"; fi
}
check_yes() { # $1=description $2=yes/no
    if [ "$2" = "yes" ]; then pass=$((pass+1));
    else fail=$((fail+1)); echo "FAIL: $1"; fi
}

cat > "$T/warn.fl" <<'EOF'
fn add(a: i64, b: i64) -> i64 { return a + b }
fn unused_param(p: i64) -> i64 { return 0 }
fn main() -> i64 {
    x = 1
    mut y = 2
    y = 3
    z = 4
    _ignored = 5
    print(z)
    print(add(1, 2))
    0
}
EOF

# 1. The default lint: warning, exit 0, program still runs.
if out="$("$FLINTC" "$T/warn.fl" 2>"$T/warn.err")"; then pass=$((pass+1));
else fail=$((fail+1)); echo "FAIL: warnings made the compile fail"; cat "$T/warn.err"; fi
check "warnings do not stop execution" "4
3" "$out"
check_yes "unused local is W0001" \
    "$(grep -q "warning\[W0001\]: unused variable 'x'" "$T/warn.err" && echo yes || echo no)"
check_yes "written-only local is W0002" \
    "$(grep -q "warning\[W0002\]: variable 'y' is assigned but never read" "$T/warn.err" && echo yes || echo no)"
check_yes "unused parameter warns" \
    "$(grep -q "warning\[W0001\]: unused variable 'p'" "$T/warn.err" && echo yes || echo no)"
check_yes "underscore-prefixed local is silent" \
    "$(grep -q "_ignored" "$T/warn.err" && echo no || echo yes)"
check_yes "used local is silent" \
    "$(grep -q "'z'" "$T/warn.err" && echo no || echo yes)"
check_yes "the warning points at the declaration line" \
    "$(grep -q "$T/warn.fl:4:6" "$T/warn.err" && echo yes || echo no)"
check_yes "the warning renders the source line" \
    "$(grep -q '^4 |     x = 1$' "$T/warn.err" && echo yes || echo no)"
check_yes "the warning carries the underscore help" \
    "$(grep -q "prefix it with an underscore" "$T/warn.err" && echo yes || echo no)"

# 2. Opt-out flag: no warnings at all.
"$FLINTC" "$T/warn.fl" --no-warn-unused >"$T/off.out" 2>"$T/off.err"
check "warnings can be silenced" "" "$(cat "$T/off.err")"
"$FLINTC" "$T/warn.fl" -Wno-unused >"$T/off2.out" 2>"$T/off2.err"
check "the -Wno-unused spelling works" "" "$(cat "$T/off2.err")"

# 3. AOT still builds/runs and keeps the artifact, warnings notwithstanding.
if "$FLINTC" "$T/warn.fl" -o "$T/warn.bin" >"$T/aot.out" 2>"$T/aot.err"; then
    pass=$((pass+1))
else
    fail=$((fail+1)); echo "FAIL: AOT build failed under warnings"; cat "$T/aot.err"
fi
check "AOT runs under warnings" "4
3" "$(cat "$T/aot.out")"
check_yes "AOT artifact survives warnings" \
    "$(test -x "$T/warn.bin" && echo yes || echo no)"
check_yes "AOT emits the warning too" \
    "$(grep -q 'W0001' "$T/aot.err" && echo yes || echo no)"

# 4. JSON form: level=warning, code=W0001, and a span.
"$FLINTC" --error-format json "$T/warn.fl" >/dev/null 2>"$T/warn.jsonl"
if python3 - "$T/warn.jsonl" <<'PY'
import json, sys
warn = [json.loads(l) for l in open(sys.argv[1]) if l.strip().startswith('{')]
w = [d for d in warn if d.get('code') == 'W0001' and d.get('spans')]
assert w, "no W0001 object with a span"
assert all(d.get('level') == 'warning' for d in w), [d.get('level') for d in w]
at_line4 = [d for d in w if d['spans'][0].get('line_start') == 4]
assert at_line4, [d['spans'][0] for d in w]
sys.exit(0)
PY
then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL: JSON warning shape"; fi

# 5. Imports are linted against their own file.
mkdir -p "$T/imp"
cat > "$T/imp/helper.fl" <<'EOF'
fn helper() -> i64 {
    unused_here = 9
    return 1
}
EOF
cat > "$T/imp/main.fl" <<'EOF'
import "./helper.fl"
fn main() -> i64 { return helper() }
EOF
"$FLINTC" "$T/imp/main.fl" >/dev/null 2>"$T/imp.err"
check_yes "an imported file is linted" \
    "$(grep -q "unused variable 'unused_here'" "$T/imp.err" && echo yes || echo no)"
check_yes "the import warning names the imported file" \
    "$(grep -q "helper.fl:2:" "$T/imp.err" && echo yes || echo no)"

# 6. A clean program is silent.
cat > "$T/clean.fl" <<'EOF'
fn main() -> i64 {
    x = 41
    print(x + 1)
    0
}
EOF
"$FLINTC" "$T/clean.fl" >/dev/null 2>"$T/clean.err"
check "a clean program emits no warnings" "" "$(cat "$T/clean.err")"

echo "unused-warnings: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
