#!/usr/bin/env bash
# Gate: native string methods (P2, 2026-10-27).
#
# Contract under test:
#   * the common methods exist with native implementations (no Python/JIT
#     detour): len, is_empty, upper, lower, capitalize, title, trim/strip,
#     lstrip/ltrim, rstrip/rtrim, reverse, is_ascii;
#   * search and slicing: get/charat/char_at, codepointat, repeat,
#     startswith/starts_with, endswith/ends_with, indexof/find, rfind,
#     contains, count, replace, substring/slice;
#   * conversions: to_i64, to_f64, to_bool;
#   * string-returning methods chain (methodRecvKind follows the rewritten
#     call, so `"  Hi  ".strip().upper().len()` classifies at every step);
#   * an unknown method is E0126 and lists the available methods; a known
#     method with the wrong arity is E0097;
#   * the one-command AOT path and the JIT path agree, and the artifact
#     survives execution.
#
# Usage: bash tests/test_string_methods.sh [path/to/flintc]   (default: ./flintc)
set -u
FLINTC="${1:-./flintc}"
_TMPBASE="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
T="$(mktemp -d "$_TMPBASE/flint-strmeth.XXXXXX" 2>/dev/null || echo "$_TMPBASE/flint-strmeth.$$")"
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

check_yes() { # $1=description $2=yes/no
    if [ "$2" = "yes" ]; then pass=$((pass+1));
    else fail=$((fail+1)); echo "FAIL: $1"; fi
}

cat > "$T/methods.fl" <<'EOF'
fn main() -> i64 {
    s = "  hello WORLD  "
    print(s.len())                    # 15
    print(s.strip())                  # hello WORLD
    print(s.trim().len())             # 11
    print("".isempty())               # 1
    print("x".is_empty())             # 0
    print("hello".capitalize())       # Hello
    print("hello world".title())      # Hello World
    print("hi".upper())               # HI
    print("HI".lower())               # hi
    print("  x".lstrip())             # x
    print("x  ".rstrip())             # x
    print("abc".reverse())            # cba
    print("abc".get(1))               # 98 (byte value)
    print("abc".charat(2))            # 99
    print("abc".char_at(0))           # 97
    print("é".codepointat(0))         # 233 (UTF-8)
    print("ab".repeat(3))             # ababab
    print("hello".startswith("he"))   # 1
    print("hello".starts_with("lo"))  # 0
    print("hello".endswith("lo"))     # 1
    print("hello".ends_with("he"))    # 0
    print("hello".indexof("l"))       # 2
    print("hello".find("z"))          # -1
    print("hello".rfind("l"))         # 3
    print("hello".last_index_of("l")) # 3
    print("hello".contains("ell"))    # 1
    print("hello".contains("xyz"))    # 0
    print("banana".count("an"))       # 2
    print("abc".count(""))            # 4 (Python semantics)
    print("hello".replace("l", "L"))  # heLLo
    print("hello".substring(1, 4))    # ell
    print("hello".slice(0, 2))        # he
    print("abc".isascii())            # 1
    print("é".is_ascii())             # 0
    print("42".to_i64() + 1)          # 43
    print("2.5".to_f64() + 0.5)       # 3
    print("true".to_bool())           # 1
    print("no".tobool())              # 0
    print("  Hi  ".strip().upper())   # HI
    print("a b".title().reverse())    # B A
    0
}
EOF

want="15
hello WORLD
11
1
0
Hello
Hello World
HI
hi
x
x
cba
98
99
97
233
ababab
1
0
1
0
2
-1
3
3
1
0
2
4
heLLo
ell
he
1
0
43
3
1
0
HI
B A"

# 1. One-command AOT: builds, runs once, keeps the executable.
if ! out="$(timeout -s KILL 300 "$FLINTC" "$T/methods.fl" -o "$T/methods.bin" 2>"$T/build.log")"; then
    echo "FAIL: string-method fixture did not compile"; cat "$T/build.log"; exit 1
fi
check "AOT string-method output (-o runs it)" "$want" "$out"
check "AOT string-method executable remains" "yes" \
    "$(test -x "$T/methods.bin" && echo yes || echo no)"
out="$("$T/methods.bin" 2>"$T/run.err")"
check "AOT string-method output (rerun)" "$want" "$out"

# 2. The JIT path produces the same answer.
out="$(timeout -s KILL 300 "$FLINTC" "$T/methods.fl" 2>"$T/jit.err")"
check "JIT string-method output" "$want" "$out"

# 3. An unknown method is E0126 and names the available methods.
cat > "$T/unknown.fl" <<'EOF'
fn main() -> i64 {
    s = "x"
    print(s.nosuch())
    0
}
EOF
if timeout -s KILL 300 "$FLINTC" "$T/unknown.fl" -o "$T/unknown.bin" >"$T/u.out" 2>"$T/u.err"; then
    fail=$((fail+1)); echo "FAIL: unknown method compiled"
else
    pass=$((pass+1))
fi
check_yes "unknown method reports E0126" \
    "$(grep -q 'E0126' "$T/u.err" && echo yes || echo no)"
check_yes "E0126 lists capitalize" \
    "$(grep -q 'capitalize' "$T/u.err" && echo yes || echo no)"
check_yes "unknown method did not produce an artifact" \
    "$(test -e "$T/unknown.bin" && echo no || echo yes)"

# 4. Wrong arity for a known method is E0097.
cat > "$T/arity.fl" <<'EOF'
fn main() -> i64 {
    s = "x"
    print(s.contains())
    0
}
EOF
if timeout -s KILL 300 "$FLINTC" "$T/arity.fl" -o "$T/arity.bin" >"$T/a.out" 2>"$T/a.err"; then
    fail=$((fail+1)); echo "FAIL: wrong-arity method compiled"
else
    pass=$((pass+1))
fi
check_yes "wrong arity reports E0097" \
    "$(grep -q 'E0097' "$T/a.err" && echo yes || echo no)"
check_yes "wrong arity names the method" \
    "$(grep -q "method 'contains' needs 1 argument" "$T/a.err" && echo yes || echo no)"

echo "string-methods: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
